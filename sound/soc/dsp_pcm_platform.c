#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/kernel.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/of_reserved_mem.h>
#include <linux/dma-mapping.h>
#include <linux/dma-direct.h>
#include <linux/rpmsg.h>
#include <linux/delay.h>
#include <linux/wait.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/errno.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <sound/soc.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#define BUFFER_LEN (32768U)         /* 32kB */
#define MAX_DEVICES (4U)            /* Maximum number of ALSA instances */
#define DSP_RESP_TIMEOUT_MS (100U)

/* DSP Control Message type */
#define CONFIG_REQ       0x2001     /* CA-DSP */
#define PCM_START        0x2002     /* CA-DSP */
#define PCM_PAUSE        0x2003     /* CA-DSP */
#define PCM_RESUME       0x2004     /* CA-DSP */
#define PCM_STOP         0x2005     /* CA-DSP */
#define POS_QUERY        0x2006     /* CA-DSP */
#define PCM_STATUS       0x8000     /* DSP-CA (reply/heartbeat/event) */
#define CONFIG_REPLY     0xA001     /* DSP-CA (config reply) */
#define POS_REPLY        0xA006     /* DSP-CA (position/cursor reply) */

struct rpmsg_hdr {
	uint16_t version;
	uint16_t msg_type;
	uint32_t msg_id;
	uint32_t stream_id;
	uint32_t payload_len;
} __attribute__((packed));

struct open_req_messsage {
	uint32_t dir;               /* playback=0, capture=1 */
	uint32_t rate;              /* app rate */
	uint32_t channels;          /* app channels */
	uint32_t format;            /* ALSA snd_pcm_format_t */
	uint32_t period_frames;     /* ALSA period size in frames */
	uint32_t periods;           /* period count */
	uint64_t pcm_rb_phys;       /* phys addr of PCM RB */
	uint32_t pcm_rb_size;
} __attribute__((packed));

struct open_resp_message {
	int32_t status;
	int32_t stream_id;
	uint32_t hw_rate;
	uint32_t hw_channels;
	uint32_t hw_format;
	uint64_t hw_rb_phys;
	uint32_t hw_rb_size;
} __attribute__((packed));

struct trigger_req {
	uint32_t cmd;               /* START/STOP/PAUSE/RESUME/DRAIN */
} __attribute__((packed));

struct dsp_config_req_msg {
	uint32_t dir;               /* playback=0, capture=1 */
	uint32_t in_rate;           /* app rate for PB/ Hw rate for REC */
	uint32_t in_channels;
	uint32_t in_format;         /* ALSA format */
	uint32_t out_rate;          /* Hw rate for PB / app rate for REC */
	uint32_t out_channels;
	uint32_t out_format;
	uint64_t pcm_rb_phys;       /* DSP write/read target for DSP */
	uint32_t pcm_rb_size;
	uint64_t hw_rb_phys;
	uint32_t hw_rb_size;
	uint32_t period_frames;     /* matches ALSA period */
	uint32_t periods;
} __attribute__((packed));

struct dsp_status_msg {
	uint32_t dir;               /* playback=0, capture=1 */
	uint32_t status;            /* RUNNING, STOPPED, PAUSED, etc. */
	uint64_t pcm_dsp_pos;       /* frames consumed since stream start */
	uint64_t hw_rb_write_pos;   /* optional: frames written to LP HW RB */
	uint32_t period_frames;     /* echo for safety */
	uint32_t rb_frames;         /* total frames in PCM RB */
} __attribute__((packed));

union rpmsg_payload {
	struct trigger_req trigger;
	struct dsp_config_req_msg dsp_config;
	struct dsp_status_msg dsp_status;
};

struct rpmsg_packet {
	struct rpmsg_hdr header;
	union rpmsg_payload payload;
} __attribute__((packed));

/* Per-stream state */
struct rcar_alsa_stream {
	size_t hw_ptr_bytes;
	size_t hw_buf_size;
	dma_addr_t hw_dma_handle;
	void *hw_cpu_addr;
};

struct rcar_alsa_priv {
	/* common parameters */
	int group_id;
	const char *cr_chan;
	const char *dsp_chan;

	/* PCM parameters */
	struct platform_device *pdev;

	struct snd_card *card;
	struct snd_pcm *pcm;

	struct rcar_alsa_stream rcar_pb_stream;
	struct rcar_alsa_stream rcar_cap_stream;

	/* RpMsg Parameters */
	struct rpmsg_device *cr_rpdev;
	struct rpmsg_device *dsp_rpdev;

	struct workqueue_struct *rpmsg_wq_cr;
	struct workqueue_struct *rpmsg_wq_dsp;

	volatile bool dsp_reply_flag;
	volatile struct rpmsg_packet dsp_reply_msg;

	wait_queue_head_t dsp_wait_q;
	spinlock_t dsp_status_lock;
};

struct rpmsg_work_dsp {
	struct work_struct work;
	struct rpmsg_packet msg;
	struct rcar_alsa_priv *priv;
};

struct rcar_alsa_priv *global_alsa_priv[MAX_DEVICES];

static int rpmsg_send_dsp(struct rpmsg_packet msg, struct rcar_alsa_priv *d)
{
	if (!d->dsp_rpdev) {
		pr_err("rcar_audio_fe: %s failed: No rpmsg_device available\n",
				__func__);
		return -ENODEV;
	}

	return rpmsg_send(d->dsp_rpdev->ept, &msg, sizeof(msg));
}

static int rpmsg_recv_dsp_blocking(int *msg_type, struct dsp_status_msg *status,
		unsigned int timeout_ms, struct rcar_alsa_priv *d)
{
	long ret;
	unsigned long flags;

	ret = wait_event_interruptible_timeout(d->dsp_wait_q,
			READ_ONCE(d->dsp_reply_flag),
			msecs_to_jiffies(timeout_ms));

	if (ret == 0) {
		pr_err("%s: timeout\n", __func__);
		return -ETIMEDOUT;
	}

	if (ret < 0) {
		pr_err("%s: error(%d)\n", __func__, ret);
		return ret;
	}

	pr_info("%s: got response after %dms, timeout %dms\n", __func__,
			(timeout_ms - jiffies_to_msecs(ret)), timeout_ms);

	spin_lock_irqsave(&d->dsp_status_lock, flags);
	if (!d->dsp_reply_flag) {
		spin_unlock_irqrestore(&d->dsp_status_lock, flags);
		return -EAGAIN;
	}

	*status = d->dsp_reply_msg.payload.dsp_status;
	*msg_type = d->dsp_reply_msg.header.msg_type;
	d->dsp_reply_flag = false;
	spin_unlock_irqrestore(&d->dsp_status_lock, flags);

	return 0;
}

static int rcar_alsa_fe_pcm_open(struct snd_pcm_substream *sub)
{
	struct rcar_alsa_priv *d = snd_pcm_substream_chip(sub);
	struct snd_pcm_runtime *runtime = sub->runtime;

	pr_info("rcar_audio_fe: %s\n", __func__);

	runtime->hw.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_BLOCK_TRANSFER;
	runtime->hw.formats = SNDRV_PCM_FMTBIT_S16_LE;
	runtime->hw.rates = SNDRV_PCM_RATE_48000;
	runtime->hw.rate_min = 48000;
	runtime->hw.rate_max = 48000;
	runtime->hw.channels_min = 2;
	runtime->hw.channels_max = 2;
	runtime->hw.buffer_bytes_max = BUFFER_LEN;
	runtime->hw.period_bytes_min = 1024;
	runtime->hw.period_bytes_max = 1024;
	runtime->hw.periods_min = 32;
	runtime->hw.periods_max = 32;

	runtime->private_data = d;

	return 0;
}

static int rcar_audio_fe_pcm_hw_params(struct snd_pcm_substream *sub,
		struct snd_pcm_hw_params *params)
{
	size_t buffer_bytes = params_buffer_bytes(params);
	struct rcar_alsa_priv *d = snd_pcm_substream_chip(sub);
	struct platform_device *pdev = d->pdev;
	struct rcar_alsa_stream *s =
		(sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ?
		&d->rcar_pb_stream : &d->rcar_cap_stream;
	struct rpmsg_packet msg;
	int type;
	int ret;
	struct dsp_status_msg dsp_status;

	if (buffer_bytes != BUFFER_LEN) {
		pr_err("rcar_audio_fe: %s error invalid buffer_bytes\n",
				__func__);
		return -EINVAL;
	}

	pr_info("audio_ctrl_rpmsg: %s PCM buffer_bytes=%zu PCM dma_addr=%pad\n",
			__func__, buffer_bytes, &sub->dma_buffer.addr);

	s->hw_buf_size = BUFFER_LEN;
	s->hw_cpu_addr = dma_alloc_coherent(&pdev->dev, s->hw_buf_size,
			&s->hw_dma_handle, GFP_KERNEL);
	if (!s->hw_cpu_addr) {
		pr_err("rcar_audio_fe: %s dma_alloc_coherent failed\n",
				__func__);
		return -ENOMEM;
	}

	pr_info("audio_ctrl_rpmsg: %s HW buffer_bytes=%zu HW dma_addr=%pad\n",
			__func__, s->hw_buf_size, &s->hw_dma_handle);

	/* header */
	msg.header.version = 1;
	msg.header.msg_type = CONFIG_REQ;
	msg.header.msg_id = 1;
	msg.header.stream_id =
		(sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ? 0: 1;
	msg.header.payload_len = sizeof(struct dsp_config_req_msg);

	/* Payload */
	msg.payload.dsp_config.dir =
		(sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ? 0: 1;
	msg.payload.dsp_config.in_rate = 48000;
	msg.payload.dsp_config.in_channels = 2;
	msg.payload.dsp_config.in_format = SNDRV_PCM_FMTBIT_S16_LE;
	msg.payload.dsp_config.out_rate = 48000;
	msg.payload.dsp_config.out_channels = 2;
	msg.payload.dsp_config.out_format = SNDRV_PCM_FMTBIT_S16_LE;
	msg.payload.dsp_config.pcm_rb_phys =
		(uint64_t)dma_to_phys(&pdev->dev, sub->dma_buffer.addr);
	msg.payload.dsp_config.pcm_rb_size = (uint64_t)buffer_bytes;
	msg.payload.dsp_config.hw_rb_phys =
		(uint64_t)dma_to_phys(&pdev->dev, s->hw_dma_handle);
	msg.payload.dsp_config.hw_rb_size = (uint64_t)s->hw_buf_size;
	msg.payload.dsp_config.period_frames = 256;
	msg.payload.dsp_config.periods = 32;

	rpmsg_send_dsp(msg, d);

	/* Wait for response from DSP */
	ret = rpmsg_recv_dsp_blocking(&type, &dsp_status,
			DSP_RESP_TIMEOUT_MS, d);
	if (ret) {
		pr_err("rcar_audio_fe: %s: no response from DSP ret(%d)\n",
				__func__, ret);
		return ret;
	}

	if (type != CONFIG_REPLY) {
		pr_err("rcar_audio_fe: %s: err response from DSP, type(0x%x)\n",
				__func__, type);
		return -EAGAIN;
	}

	return 0;
}

static int rcar_audio_fe_pcm_hw_free(struct snd_pcm_substream *sub)
{
	struct rcar_alsa_priv *d = snd_pcm_substream_chip(sub);
	struct platform_device *pdev = d->pdev;
	struct rcar_alsa_stream *s =
		(sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ?
		&d->rcar_pb_stream : &d->rcar_cap_stream;

	pr_info("rcar_audio_fe: %s\n", __func__);

	/* Free HW buffer */
	dma_free_coherent(&pdev->dev, s->hw_buf_size, s->hw_cpu_addr,
			s->hw_dma_handle);
	s->hw_cpu_addr = NULL;

	return 0;
}

static int rcar_audio_fe_pcm_prepare(struct snd_pcm_substream *sub)
{
	struct rcar_alsa_priv *d = snd_pcm_substream_chip(sub);

	pr_info("rcar_audio_fe: %s\n", __func__);

	if (sub->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		d->rcar_pb_stream.hw_ptr_bytes = 0;
	} else {
		d->rcar_cap_stream.hw_ptr_bytes = 0;
	}

	return 0;
}

static int rcar_audio_fe_pcm_trigger(struct snd_pcm_substream *sub, int cmd)
{
	struct rcar_alsa_priv *d = snd_pcm_substream_chip(sub);
	struct rpmsg_packet msg;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		pr_info("rcar_audio_fe: %s START\n", __func__);

		/* Notify DSP */
		msg.header.version = 1;
		msg.header.msg_type = PCM_START;
		msg.header.msg_id = 1;
		msg.header.stream_id =
			(sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ? 0: 1;
		msg.header.payload_len = sizeof(struct trigger_req);
		msg.payload.trigger.cmd = PCM_START;

		rpmsg_send_dsp(msg, d);
		return 0;

	case SNDRV_PCM_TRIGGER_RESUME:
		pr_info("rcar_audio_fe: %s RESUME\n", __func__);

		/* Notify DSP */
		msg.header.version = 1;
		msg.header.msg_type = PCM_RESUME;
		msg.header.msg_id = 1;
		msg.header.stream_id =
			(sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ? 0: 1;
		msg.header.payload_len = sizeof(struct trigger_req);
		msg.payload.trigger.cmd = PCM_RESUME;
		rpmsg_send_dsp(msg, d);

		return 0;

	case SNDRV_PCM_TRIGGER_STOP:
		pr_info("rcar_audio_fe: %s STOP\n", __func__);

		/* Notify DSP */
		msg.header.version = 1;
		msg.header.msg_type = PCM_STOP;
		msg.header.msg_id = 1;
		msg.header.stream_id =
			(sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ? 0: 1;
		msg.header.payload_len = sizeof(struct trigger_req);
		msg.payload.trigger.cmd = PCM_STOP;
		rpmsg_send_dsp(msg, d);
		return 0;

	case SNDRV_PCM_TRIGGER_SUSPEND:
		pr_info("rcar_audio_fe: %s SUSPEND\n", __func__);

		/* Notify DSP */
		msg.header.version = 1;
		msg.header.msg_type = PCM_PAUSE;
		msg.header.msg_id = 1;
		msg.header.stream_id =
			(sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ? 0: 1;
		msg.header.payload_len = sizeof(struct trigger_req);
		msg.payload.trigger.cmd = PCM_PAUSE;
		rpmsg_send_dsp(msg, d);

		return 0;

	default:
		return -EINVAL;
	}
}

static snd_pcm_uframes_t rcar_audio_fe_pcm_pointer(
		struct snd_pcm_substream *sub)
{
	struct rcar_alsa_priv *d = snd_pcm_substream_chip(sub);
	size_t hwptr =
		(sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ?
		d->rcar_pb_stream.hw_ptr_bytes:
		d->rcar_cap_stream.hw_ptr_bytes;

	return bytes_to_frames(sub->runtime, hwptr);
}

static int rcar_audio_fe_pcm_mmap(struct snd_pcm_substream *sub,
		struct vm_area_struct *vma)
{
	pr_info("rcar_audio_fe: %s\n", __func__);
	return snd_pcm_lib_default_mmap(sub, vma);
}

static int rcar_audio_fe_pcm_close(struct snd_pcm_substream *sub)
{
	pr_info("rcar_audio_fe: %s\n", __func__);
	return 0;
}

static int rcar_audio_fe_pcm_copy_user(struct snd_pcm_substream *substream,
		int channel, unsigned long pos,
		void __user *buf, unsigned long bytes)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	void *hwbuf = runtime->dma_area + pos;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		if (copy_from_user(hwbuf, buf, bytes))
			return -EFAULT;
	} else {
		if (copy_to_user(buf, hwbuf, bytes))
			return -EFAULT;
	}

	return 0;
}

static const struct snd_pcm_ops rcar_audio_pcm_ops = {
	.open      = rcar_alsa_fe_pcm_open,
	.close     = rcar_audio_fe_pcm_close,
	.hw_params = rcar_audio_fe_pcm_hw_params,
	.hw_free   = rcar_audio_fe_pcm_hw_free,
	.prepare   = rcar_audio_fe_pcm_prepare,
	.trigger   = rcar_audio_fe_pcm_trigger,
	.pointer   = rcar_audio_fe_pcm_pointer,
	.mmap      = rcar_audio_fe_pcm_mmap,
	.copy_user = rcar_audio_fe_pcm_copy_user,
};

/* ALSA card + PCM creation */
static int rcar_audio_pcm_create(struct platform_device *pdev,
		struct rcar_alsa_priv *d)
{
	int ret;

	pr_info("rcar_audio_fe: %s\n", __func__);

	/* Create sound card */
	ret = snd_card_new(&pdev->dev, -1, "rcar_alsa_fe",
			THIS_MODULE, 0, &d->card);
	if (ret < 0)
		return ret;

	/* Create PCM instance: 1 playback, 1 capture */
	ret = snd_pcm_new(d->card, "rcar_alsa_fe_pcm", 0, 1, 1, &d->pcm);
	if (ret < 0)
		return ret;

	d->pcm->private_data = d;

	/* Assign PCM ops */
	snd_pcm_set_ops(d->pcm, SNDRV_PCM_STREAM_PLAYBACK,
			&rcar_audio_pcm_ops);
	snd_pcm_set_ops(d->pcm, SNDRV_PCM_STREAM_CAPTURE,
			&rcar_audio_pcm_ops);

	snd_pcm_set_managed_buffer_all(d->pcm, SNDRV_DMA_TYPE_DEV, &pdev->dev,
			BUFFER_LEN, BUFFER_LEN);

	/* Register the card */
	ret = snd_card_register(d->card);
	if (ret < 0)
		return ret;

	dev_info(&pdev->dev,
		"rcar_audio_fe: ALSA card + PCM created successfully\n");
	return 0;
}

static void dsp_pcm_handle_period(int stream_dir,  struct rcar_alsa_priv *d)
{
	struct snd_pcm_substream *sub;
	struct snd_pcm_runtime *rt;
	size_t *hw_ptr;

	if (!d)
		return;

	if (stream_dir == SNDRV_PCM_STREAM_PLAYBACK) {
		sub = d->pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream;
		hw_ptr = &d->rcar_pb_stream.hw_ptr_bytes;
	} else {
		sub = d->pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream;
		hw_ptr = &d->rcar_cap_stream.hw_ptr_bytes;
	}

	if (!sub || !sub->runtime)
		return;

	rt = sub->runtime;

	/* Advance hardware pointer */
	*hw_ptr += frames_to_bytes(rt, rt->period_size);

	if (*hw_ptr >= frames_to_bytes(rt, rt->buffer_size))
		*hw_ptr = 0;

	/* Tell ALSA a period elapsed */
	snd_pcm_period_elapsed(sub);
}

static int rcar_audio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct rcar_alsa_priv *d;
	int ret;
	int gid;

	pr_info("rcar_audio_fe: %s\n", __func__);

	ret = of_property_read_u32(np, "rcar,group-id", &gid);
	if (ret) {
		dev_err(dev, "rcar,group-id not mentioned in DT, ret: %d\n",
				ret);
		return ret;
	}

	if (global_alsa_priv[gid] == NULL) {
		global_alsa_priv[gid] =
			devm_kzalloc(&pdev->dev, sizeof(*d), GFP_KERNEL);
		if (!global_alsa_priv[gid]) {
			dev_err(dev, "devm_kzalloc failed\n");
			return -ENOMEM;
		}
	} else {
		dev_err(dev, "ALSA instance-%d already exist\n", gid);
		return -EPROTO;
	}

	d = global_alsa_priv[gid];
	d->group_id = gid;
	d->pdev = pdev;

	platform_set_drvdata(pdev, d);

	ret = of_property_read_string(np, "rcar,rpmsg-cr-chan", &d->cr_chan);
	if (ret) {
		dev_err(dev,
			"rcar,rpmsg-cr-chan not mentioned in DT, ret: %d\n",
			ret);
		return ret;
	}

	ret = of_property_read_string(np, "rcar,rpmsg-dsp-chan", &d->dsp_chan);
	if (ret) {
		dev_err(dev,
			"rcar,rpmsg-dsp-chan not mentioned in DT, ret: %d\n",
			ret);
		return ret;
	}

	/* Attach this device to the reserved-memory pool from DT */
	ret = of_reserved_mem_device_init(dev);
	if (ret) {
		dev_err(dev, "of_reserved_mem_device_init failed: %d\n", ret);
		return ret;
	}

	ret = dma_set_coherent_mask(dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(dev, "dma_set_coherent_mask failed: %d\n", ret);
		of_reserved_mem_device_release(dev);
		return ret;
	}

	/* Create ALSA card + PCM device */
	ret = rcar_audio_pcm_create(pdev, d);
	if (ret) {
		dev_err(dev, "rcar_audio_pcm_create failed: %d\n", ret);
		of_reserved_mem_device_release(dev);
		return ret;
	}

	dev_info(dev, "rcar_audio_fe: platform driver probed\n");
	return 0;
}

static int rcar_audio_remove(struct platform_device *pdev)
{
	struct rcar_alsa_priv *d = platform_get_drvdata(pdev);

	if (d && d->card) {
		snd_card_free(d->card);
	}

	of_reserved_mem_device_release(&pdev->dev);
	global_alsa_priv[d->group_id] = NULL;

	dev_info(&pdev->dev, "rcar_audio_fe: platform driver removed\n");
	return 0;
}

static const struct of_device_id rcar_audio_of_match[] = {
	{ .compatible = "renesas,rcar_audio_fe" },
	{}
};
MODULE_DEVICE_TABLE(of, rcar_audio_of_match);

static struct platform_driver dsp_pcm_driver = {
	.probe = rcar_audio_probe,
	.remove = rcar_audio_remove,
	.driver = {
		.name = "rcar-audio-fe",
		.of_match_table = rcar_audio_of_match,
	},
};
module_platform_driver(dsp_pcm_driver);

static int rpmsg_parse_gid(char *name)
{
	int i;

	for (i = 0; i < MAX_DEVICES; i++) {
		if (global_alsa_priv[i]) {
			if (!strncmp(global_alsa_priv[i]->cr_chan,
						name, strlen(name)) ||
					!strncmp(global_alsa_priv[i]->dsp_chan,
						name, strlen(name))) {
				return i;
			}
		}
	}

	return -1;
}

static bool rpmsg_is_cr(char *name, struct rcar_alsa_priv *d) {
	return !strncmp(d->cr_chan, name, strlen(name));
}

static void rpmsg_dsp_handle(struct work_struct *work)
{
	struct rpmsg_work_dsp *w =
		container_of(work, struct rpmsg_work_dsp, work);
	struct rcar_alsa_priv *d = w->priv;
	struct rpmsg_packet *msg = &w->msg;
	unsigned long flags;
	bool drop_msg = false;

	switch(msg->header.msg_type) {
	case CONFIG_REPLY:
	case POS_REPLY:
		spin_lock_irqsave(&d->dsp_status_lock, flags);
		if (d->dsp_reply_flag) {
			/* drop unhandled previous message */
			drop_msg = true;
		}

		d->dsp_reply_flag = true;
		d->dsp_reply_msg = *msg;
		spin_unlock_irqrestore(&d->dsp_status_lock, flags);
		wake_up_interruptible(&d->dsp_wait_q);

		if (drop_msg) {
			pr_warn("audio_ctrl_rpmsg: %s: "
					"Discarding unused previous message\n",
					__func__);
			drop_msg = false;
		}
		break;

	case PCM_STATUS:
		dsp_pcm_handle_period(msg->payload.dsp_status.dir, d);
		break;

	default:
		pr_warn("audio_ctrl_rpmsg: unknown dsp message type %u\n",
				msg->header.msg_type);
		break;
	}
	kfree(w);
}

static int rpmsg_audio_ctrl_cb(struct rpmsg_device *rpdev, void *data, int len,
		void *priv, u32 src)
{
	struct rcar_alsa_priv *d = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_packet *msg = data;
	struct rpmsg_work_dsp *w;

	if (len < sizeof(struct rpmsg_hdr)) {
		pr_err("audio_ctrl_rpmsg: %s failed: "
				"Invalid message length %d\n", __func__, len);
		return -EINVAL;
	}

	if (rpmsg_is_cr(rpdev->id.name, d)) {
		/* Implement later */
	} else {
		if (d->rpmsg_wq_dsp) {
			w = kmalloc(sizeof(struct rpmsg_work_dsp), GFP_ATOMIC);
			if (w) {
				INIT_WORK(&w->work, rpmsg_dsp_handle);
				w->msg = *msg;
				w->priv = d;
				queue_work(d->rpmsg_wq_dsp, &w->work);
			} else {
				pr_err("audio_ctrl_rpmsg: %s: kmalloc failed\n",
						__func__);
				return -ENOMEM;
			}
		} else {
			pr_err("audio_ctrl_rpmsg: %s: No workqueue\n",
					__func__);
			return -EINVAL;
		}
	}
	return 0;
}

static int rpmsg_audio_ctrl_probe(struct rpmsg_device *rpdev)
{
	struct rcar_alsa_priv *d;
	int ret;
	int gid;
	char *msg = "START";

	gid = rpmsg_parse_gid(rpdev->id.name);
	if (gid < 0) {
		dev_err(&rpdev->dev, "wrong gid: %d\n", gid);
		return -EPROTO;
	}

	if (global_alsa_priv[gid] == NULL) {
		dev_err(&rpdev->dev,
				"ALSA driver for instance-%d is not probed\n",
				gid);
		return -EPROTO;
	}

	d = global_alsa_priv[gid];
	dev_set_drvdata(&rpdev->dev, d);

	if (rpmsg_is_cr(rpdev->id.name, d)) {
		d->cr_rpdev = rpdev;
	} else {
		d->rpmsg_wq_dsp =
			create_singlethread_workqueue("rpmsg_dsp_workqueue");
		if (!d->rpmsg_wq_dsp) {
			dev_err(&rpdev->dev,
				"create_singlethread_workqueue for dsp failed\n"
				);
			return -EPROTO;
		}

		d->dsp_rpdev = rpdev;
	}

	ret = rpmsg_send(rpdev->ept, msg, strlen(msg));
	if (ret) {
		dev_err(&rpdev->dev, "rpmsg_send failed: %d\n", ret);

		if (rpmsg_is_cr(rpdev->id.name, d)) {
			d->cr_rpdev = NULL;
		} else {
			d->dsp_rpdev = NULL;
		}

		return ret;
	}

	init_waitqueue_head(&d->dsp_wait_q);
	spin_lock_init(&d->dsp_status_lock);
	d->dsp_reply_flag = false;

	dev_info(&rpdev->dev, "rpmsg audio dsp new channel: 0x%x -> 0x%x!\n",
			rpdev->src, rpdev->dst);

	return 0;
}

static void rpmsg_audio_ctrl_remove(struct rpmsg_device *rpdev)
{
	struct rcar_alsa_priv *d = dev_get_drvdata(&rpdev->dev);

	if (rpmsg_is_cr(rpdev->id.name, d)) {
		d->cr_rpdev = NULL;
	} else {
		/* delete dsp workqueue */
		flush_workqueue(d->rpmsg_wq_dsp);
		destroy_workqueue(d->rpmsg_wq_dsp);

		d->dsp_rpdev = NULL;
	}

	dev_info(&rpdev->dev, "rpmsg audio dsp driver is removed\n");
}

static struct rpmsg_device_id rpmsg_driver_audio_ctrl_id_table[] = {
	{ .name	= "audio_cr_ctrl_g0" },
	{ .name	= "audio_dsp_ctrl_g0" },
	{ .name	= "audio_cr_ctrl_g1" },
	{ .name	= "audio_dsp_ctrl_g1" },
	{ .name	= "audio_cr_ctrl_g2" },
	{ .name	= "audio_dsp_ctrl_g2" },
	{ .name	= "audio_cr_ctrl_g3" },
	{ .name	= "audio_dsp_ctrl_g3" },
	{ },
};
MODULE_DEVICE_TABLE(rpmsg, rpmsg_driver_audio_ctrl_id_table);

static struct rpmsg_driver rpmsg_audio_ctrl = {
	.drv.name   = "audio_ctrl_rpmsg",
	.id_table   = rpmsg_driver_audio_ctrl_id_table,
	.probe      = rpmsg_audio_ctrl_probe,
	.callback   = rpmsg_audio_ctrl_cb,
	.remove     = rpmsg_audio_ctrl_remove,
};
module_rpmsg_driver(rpmsg_audio_ctrl);

MODULE_LICENSE("GPL");
