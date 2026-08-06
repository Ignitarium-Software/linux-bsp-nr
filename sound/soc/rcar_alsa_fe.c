// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Renesas Electronics Corporation
 */

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
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <sound/soc.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#include <sound/rcar_alsa_fe.h>

#define MAX_DEVICES (2U)            /* Maximum number of ALSA instances */
#define DSP_RESP_TIMEOUT_MS (100U)
#define CR_RESP_TIMEOUT_MS (100U)

/* audio format info */
struct hw_audio_format {
	uint64_t format;
	uint32_t rate;
	uint32_t channels;
	size_t period_frames;
	size_t period_bytes;
	uint32_t hw_periods;
};

/* Per-stream state */
struct rcar_alsa_stream {
	size_t hw_ptr_bytes;
	size_t hw_buf_size;
	uint64_t hw_phy_addr;
	void *hw_cpu_addr;
	struct hw_audio_format format;

	/* stream_id from CR core */
	uint32_t stream_id;
};

/* debugfs entries */
struct dsp_dbg_stream {
	struct mutex lock;
	void *cpu_addr;
	uint64_t phy_addr;
	size_t bytes;
	size_t period_bytes;
	uint32_t periods;
	bool valid;
	uint32_t rd_avail_count;
	uint32_t wr_avail_count;
	size_t rd_off;
	size_t wr_off;
	const char *name;
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

	volatile bool cr_reply_flag;
	volatile struct rpmsg_packet cr_reply_msg;

	volatile bool dsp_reply_flag;
	volatile struct rpmsg_packet dsp_reply_msg;

	wait_queue_head_t cr_wait_q;
	spinlock_t cr_status_lock;

	wait_queue_head_t dsp_wait_q;
	spinlock_t dsp_status_lock;

	/* debugfs entires */
	struct dentry *dbg_root;
	struct dsp_dbg_stream dbg_playback;
	struct dsp_dbg_stream dbg_capture;

	/* bitmask for active streams */
	uint32_t active_streams;
};

struct rpmsg_work {
	struct work_struct work;
	struct rpmsg_packet msg;
	struct rcar_alsa_priv *priv;
};

struct rcar_alsa_priv *global_alsa_priv[MAX_DEVICES];

/* Transmit request or command to remote processor via rpmsg */
static int rpmsg_send_remote(uint8_t remote, struct rpmsg_packet msg,
		uint32_t *id, struct rcar_alsa_priv *d)
{
	static uint32_t msg_id = 0;
	struct rpmsg_device *rpmsg_dev =
		(remote == REMOTE_CR) ? d->cr_rpdev : d->dsp_rpdev;

	if (!rpmsg_dev) {
		pr_err("%s(%s) failed: No rpmsg_device available\n",
			__func__, (remote == REMOTE_CR) ? "CR" : "DSP");
		return -ENODEV;
	}

	if (++msg_id == U32_MAX)
		msg_id = 1;

	*id = msg.header.msg_id = msg_id;
	return rpmsg_send(rpmsg_dev->ept, &msg, sizeof(msg));
}

/* Receive response from remote processor via rpmsg */
static int rpmsg_receive_remote(uint8_t remote, uint16_t msg_type,
		uint32_t msg_id, uint32_t stream_id,
		union rpmsg_payload *response, uint32_t timeout_ms,
		struct rcar_alsa_priv *d)
{
	long ret;
	unsigned long flags;
	wait_queue_head_t *wq;
	spinlock_t *status_lock;
	volatile bool *reply_flag;
	volatile struct rpmsg_packet *reply_msg;

	wq = (remote == REMOTE_CR) ? &d->cr_wait_q : &d->dsp_wait_q;
	status_lock = (remote == REMOTE_CR) ?
		&d->cr_status_lock : &d->dsp_status_lock;
	reply_flag = (remote == REMOTE_CR) ?
		&d->cr_reply_flag : &d->dsp_reply_flag;
	reply_msg = (remote == REMOTE_CR) ?
		&d->cr_reply_msg : &d->dsp_reply_msg;

	ret = wait_event_interruptible_timeout(*wq, READ_ONCE(*reply_flag),
			msecs_to_jiffies(timeout_ms));

	if (ret == 0) {
		pr_err("%s (%s): timeout\n",
			__func__, (remote == REMOTE_CR) ? "CR" : "DSP");
		return -ETIMEDOUT;
	}

	if (ret < 0) {
		pr_err("%s (%s): error(%ld)\n",
			__func__, (remote == REMOTE_CR) ? "CR" : "DSP", ret);
		return ret;
	}

	spin_lock_irqsave(status_lock, flags);
	if (!reply_flag) {
		spin_unlock_irqrestore(status_lock, flags);
		return -EAGAIN;
	}

	if (reply_msg->header.msg_id != msg_id) {
		pr_err("%s (%s): wrong msg_id(%u)\n",
			__func__, (remote == REMOTE_CR) ? "CR" : "DSP",
			reply_msg->header.msg_id);
		return -EAGAIN;
	}

	if (reply_msg->header.msg_type != msg_type) {
		pr_err("%s (%s): wrong msg_type(%u)\n",
			__func__, (remote == REMOTE_CR) ? "CR" : "DSP",
			reply_msg->header.msg_type);
		return -EAGAIN;
	}

	if ((msg_type != OPEN_RESP) &&
			(stream_id != reply_msg->header.stream_id)) {
		pr_err("%s (%s): wrong stream_id(%u)\n",
			__func__, (remote == REMOTE_CR) ? "CR" : "DSP",
			reply_msg->header.stream_id);
		return -EAGAIN;
	}

	*response = reply_msg->payload;
	*reply_flag = false;
	spin_unlock_irqrestore(status_lock, flags);

	return 0;
}

/* Transmit request to remote processor, and
 * get back the response from remote if
 * response parameter passed is not NULL
 */
static inline int rpmsg_request_remote(uint8_t remote, struct rpmsg_packet msg,
		union rpmsg_payload *response, struct rcar_alsa_priv *d)
{
	int ret;
	uint32_t msg_id;
	uint32_t timeout_ms = (remote == REMOTE_CR) ?
		CR_RESP_TIMEOUT_MS : DSP_RESP_TIMEOUT_MS;

	ret = rpmsg_send_remote(remote, msg, &msg_id, d);
	if (ret)
		return ret;

	if (response != NULL) {
		return rpmsg_receive_remote(remote,
				msg.header.msg_type | REPLY_MSG_MASK,
				msg_id, msg.header.stream_id,
				response, timeout_ms, d);
	}

	return 0;
}

/* Trigger PCM events to remote processor and getback the response */
static int trigger_request_remote(uint32_t stream_id, uint8_t remote,
		uint16_t cmd, struct rcar_alsa_priv *d)
{
	struct rpmsg_packet msg;
	int ret;

	/* header */
	msg.header.version = (remote == REMOTE_CR) ?
		CR_CTRL_RPMSG_VERSION : DSP_CTRL_RPMSG_VERSION;
	msg.header.msg_type = cmd;
	msg.header.stream_id = stream_id;
	msg.header.payload_len = sizeof(struct trigger_req);

	/* payload */
	msg.payload.trigger.cmd = cmd;

	/* trigger request to remote */
	return rpmsg_request_remote(remote, msg, NULL, d);
}

static inline int get_pcm_format(uint32_t format, uint64_t *pcm_format)
{
	switch (format) {
	case FORMAT_S8:
		*pcm_format = SNDRV_PCM_FMTBIT_S8;
		break;
	case FORMAT_U8:
		*pcm_format = SNDRV_PCM_FMTBIT_U8;
		break;
	case FORMAT_S16_LE:
		*pcm_format = SNDRV_PCM_FMTBIT_S16_LE;
		break;
	case FORMAT_S16_BE:
		*pcm_format = SNDRV_PCM_FMTBIT_S16_BE;
		break;
	case FORMAT_U16_LE:
		*pcm_format = SNDRV_PCM_FMTBIT_U16_LE;
		break;
	case FORMAT_U16_BE:
		*pcm_format = SNDRV_PCM_FMTBIT_U16_BE;
		break;
	case FORMAT_S24_LE:
		*pcm_format = SNDRV_PCM_FMTBIT_S24_LE;
		break;
	case FORMAT_S24_BE:
		*pcm_format = SNDRV_PCM_FMTBIT_S24_BE;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static inline int get_pcm_rate(uint32_t rate, uint32_t *pcm_rate)
{
	switch (rate) {
	case RATE_44100:
		*pcm_rate = SNDRV_PCM_RATE_44100;
		break;
	case RATE_48000:
		*pcm_rate = SNDRV_PCM_RATE_48000;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int rcar_alsa_fe_pcm_open(struct snd_pcm_substream *sub)
{
	struct rcar_alsa_priv *d = snd_pcm_substream_chip(sub);
	struct snd_pcm_runtime *runtime = sub->runtime;
	struct rcar_alsa_stream *s =
		(sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ?
		&d->rcar_pb_stream : &d->rcar_cap_stream;
	struct rpmsg_packet msg;
	int ret;
	union rpmsg_payload response;

	pr_info("rcar_audio_fe: %s\n", __func__);

	/* CR msg header*/
	msg.header.version = CR_CTRL_RPMSG_VERSION;
	msg.header.msg_type = OPEN_REQ;
	msg.header.payload_len = sizeof(struct open_req_msg);

	/* CR msg Payload */
	msg.payload.open_req.dir =
		(sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ?
		DIR_PLAYBACK: DIR_CAPTURE;

	/* send open requset to CR and get response back */
	ret = rpmsg_request_remote(REMOTE_CR, msg, &response, d);
	if (ret < 0)
		return ret;

	if (response.open_resp.status != STATUS_SUCCESS) {
		pr_err("rcar_audio_fe: %s: err status from CR, status(%d)\n",
				__func__, response.open_resp.status);
		return -EAGAIN;
	}

	if (!response.open_resp.hw_rb_phys || !response.open_resp.hw_rb_phys) {
		pr_err("rcar_audio_fe: %s: Bad address / size for rb\n",
				__func__);
		return -EAGAIN;
	}

	/* set per stream parameters */
	s->stream_id = response.open_resp.stream_id;
	s->hw_phy_addr = response.open_resp.hw_rb_phys;
	s->hw_buf_size = response.open_resp.hw_rb_size;

	s->hw_cpu_addr = ioremap(s->hw_phy_addr, s->hw_buf_size);
	if (!s->hw_cpu_addr) {
		pr_err("rcar_audio_fe: %s ioremap failed\n",
				__func__);
		return -ENOMEM;
	}

	s->format.format = response.open_resp.hw_format;
	s->format.rate = response.open_resp.hw_rate;
	s->format.channels = response.open_resp.hw_channels;
	s->format.period_frames = response.open_resp.hw_period_frames;
	s->format.period_bytes = response.open_resp.hw_period_bytes;
	s->format.hw_periods = response.open_resp.hw_periods;

	/* set runtime hw parameters*/
	ret = get_pcm_format(s->format.format, &runtime->hw.formats);
	if (ret < 0)
		return ret;

	ret = get_pcm_rate(s->format.rate, &runtime->hw.rates);
	if (ret < 0)
		return ret;

	runtime->hw.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_BLOCK_TRANSFER;
	runtime->hw.rate_min = s->format.rate;
	runtime->hw.rate_max = s->format.rate;
	runtime->hw.channels_min = s->format.channels;
	runtime->hw.channels_max = s->format.channels;
	runtime->hw.buffer_bytes_max = BUFFER_LEN_MAX;
	runtime->hw.period_bytes_min = s->format.period_bytes;
	runtime->hw.period_bytes_max = s->format.period_bytes;
	runtime->hw.periods_min = PERIODS_MIN;
	runtime->hw.periods_max = (BUFFER_LEN_MAX / s->format.period_bytes);

	/* set stream_id bit in active streams */
	d->active_streams |= (1 << s->stream_id);

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
	int ret;
	union rpmsg_payload response;
	struct dsp_dbg_stream *dbg;

	if (buffer_bytes > BUFFER_LEN_MAX) {
		pr_err("rcar_audio_fe: %s error invalid buffer_bytes(%lu)\n",
				__func__, buffer_bytes);
		return -EINVAL;
	}

	dbg = (sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ?
		&d->dbg_playback : &d->dbg_capture;
	mutex_lock(&dbg->lock);
	dbg->cpu_addr = s->hw_cpu_addr;
	dbg->phy_addr = s->hw_phy_addr;
	dbg->bytes = s->hw_buf_size;
	dbg->period_bytes = s->format.period_bytes;
	dbg->periods = s->format.hw_periods;
	dbg->valid = true;
	dbg->rd_avail_count = 0;
	dbg->rd_off = 0;
	dbg->wr_avail_count = dbg->periods;
	dbg->wr_off = 0;
	mutex_unlock(&dbg->lock);

	/* DSP msg header*/
	msg.header.version = DSP_CTRL_RPMSG_VERSION;
	msg.header.msg_type = CONFIG_REQ;
	msg.header.stream_id = s->stream_id;
	msg.header.payload_len = sizeof(struct dsp_config_req_msg);

	/* DSP msg Payload */
	msg.payload.dsp_config.dir =
		(sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ?
		DIR_PLAYBACK: DIR_CAPTURE;
	msg.payload.dsp_config.in_rate = s->format.rate;
	msg.payload.dsp_config.in_channels = s->format.channels;
	msg.payload.dsp_config.in_format = s->format.format;
	msg.payload.dsp_config.out_rate = s->format.rate;
	msg.payload.dsp_config.out_channels = s->format.channels;
	msg.payload.dsp_config.out_format = s->format.format;
	msg.payload.dsp_config.pcm_rb_phys =
		(uint64_t)dma_to_phys(&pdev->dev, sub->dma_buffer.addr);
	msg.payload.dsp_config.pcm_rb_size = (uint64_t)buffer_bytes;
	msg.payload.dsp_config.hw_rb_phys = s->hw_phy_addr;
	msg.payload.dsp_config.hw_rb_size = (uint64_t)s->hw_buf_size;
	msg.payload.dsp_config.period_frames = s->format.period_frames;
	msg.payload.dsp_config.period_bytes = s->format.period_bytes;

	pr_info("%s: hw_rb_phys(0x%llx) pcm_rb_phys(0x%llx)\n", __func__,
		msg.payload.dsp_config.hw_rb_phys,
		msg.payload.dsp_config.pcm_rb_phys);

	/* send config requset to DSP and get response back */
	ret = rpmsg_request_remote(REMOTE_DSP, msg, &response, d);
	if (ret)
		return ret;

	if (response.resp.status != STATUS_SUCCESS) {
		pr_err("rcar_audio_fe: %s: err status from DSP, status(%d)\n",
				__func__, response.resp.status);
		return -EAGAIN;
	}

	return 0;
}

static int rcar_audio_fe_pcm_hw_free(struct snd_pcm_substream *sub)
{
	struct rcar_alsa_priv *d = snd_pcm_substream_chip(sub);
	struct rcar_alsa_stream *s =
		(sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ?
		&d->rcar_pb_stream : &d->rcar_cap_stream;
	struct dsp_dbg_stream *dbg;

	pr_info("rcar_audio_fe: %s\n", __func__);

	dbg = (sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ?
		&d->dbg_playback : &d->dbg_capture;
	mutex_lock(&dbg->lock);
	dbg->cpu_addr = NULL;
	dbg->phy_addr = 0;
	dbg->bytes = 0;
	dbg->period_bytes = 0;
	dbg->valid = false;
	dbg->rd_avail_count = 0;
	dbg->wr_avail_count = 0;
	mutex_unlock(&dbg->lock);

	/* Free HW buffer */
	iounmap(s->hw_cpu_addr);
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
	struct rcar_alsa_stream *s =
		(sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ?
		&d->rcar_pb_stream : &d->rcar_cap_stream;
	uint32_t s_id = s->stream_id;
	int ret;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		pr_info("rcar_audio_fe: %s START\n", __func__);

		/* notify CR */
		ret = trigger_request_remote(s_id, REMOTE_CR, TRIGGER_START, d);
		if (ret < 0)
			return -EAGAIN;

		/* Notify DSP */
		ret = trigger_request_remote(s_id, REMOTE_DSP, PCM_START, d);
		if (ret < 0)
			return -EAGAIN;

		break;

    case SNDRV_PCM_TRIGGER_RESUME:
		pr_info("rcar_audio_fe: %s RESUME\n", __func__);

		/* notify CR */
		ret = trigger_request_remote(s_id, REMOTE_CR, TRIGGER_RESUME, d);
		if (ret < 0)
			return -EAGAIN;

		/* Notify DSP */
		ret = trigger_request_remote(s_id, REMOTE_DSP, PCM_RESUME, d);
		if (ret < 0)
			return -EAGAIN;

		break;

	case SNDRV_PCM_TRIGGER_STOP:
		pr_info("rcar_audio_fe: %s STOP\n", __func__);

		/* notify CR */
		ret = trigger_request_remote(s_id, REMOTE_CR, TRIGGER_STOP, d);
		if (ret < 0)
			return -EAGAIN;

		/* Notify DSP */
		ret = trigger_request_remote(s_id, REMOTE_DSP, PCM_STOP, d);
		if (ret < 0)
			return -EAGAIN;

		break;

	case SNDRV_PCM_TRIGGER_SUSPEND:
		pr_info("rcar_audio_fe: %s SUSPEND\n", __func__);

		/* notify CR */
		ret = trigger_request_remote(s_id, REMOTE_CR, TRIGGER_PAUSE, d);
		if (ret < 0)
			return -EAGAIN;

		/* Notify DSP */
		ret = trigger_request_remote(s_id, REMOTE_DSP, PCM_PAUSE, d);
		if (ret < 0)
			return -EAGAIN;

		break;

	default:
		pr_err("rcar_audio_fe: %s unknown command(%u)\n", __func__, cmd);
		return -EINVAL;
	}

	return 0;
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
	struct rcar_alsa_priv *d = snd_pcm_substream_chip(sub);
	struct rcar_alsa_stream *s =
		(sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ?
		&d->rcar_pb_stream : &d->rcar_cap_stream;
	uint32_t s_id = s->stream_id;
	int ret;
	struct rpmsg_packet msg;
	union rpmsg_payload response;

	pr_info("rcar_audio_fe: %s\n", __func__);

	/* CR msg header*/
	msg.header.version = CR_CTRL_RPMSG_VERSION;
	msg.header.msg_type = TRIGGER_CLOSE;
	msg.header.stream_id = s->stream_id;
	msg.header.payload_len = sizeof(struct trigger_req);

	/* CR msg Payload */
	msg.payload.trigger.cmd = TRIGGER_CLOSE;

	/* send close requset to CR and get response back */
	ret = rpmsg_request_remote(REMOTE_CR, msg, &response, d);
	if (ret < 0)
		return ret;

	if (response.resp.status != STATUS_SUCCESS) {
		pr_err("rcar_audio_fe: %s: err status from CR, status(%d)\n",
				__func__, response.resp.status);
		return -EAGAIN;
	}

	/* DSP msg header*/
	msg.header.version = DSP_CTRL_RPMSG_VERSION;
	msg.header.msg_type = PCM_CLOSE;
	msg.header.stream_id = s->stream_id;
	msg.header.payload_len = sizeof(struct trigger_req);

	/* DSP msg Payload */
	msg.payload.trigger.cmd = PCM_CLOSE;

	/* send config requset to DSP and get response back */
	ret = rpmsg_request_remote(REMOTE_DSP, msg, &response, d);
	if (ret)
		return ret;

	if (response.resp.status != STATUS_SUCCESS) {
		pr_err("rcar_audio_fe: %s: err status from DSP, status(%d)\n",
				__func__, response.resp.status);
		return -EAGAIN;
	}

	/* clear stream_id bit in active streams */
	d->active_streams &= ~(1 << s->stream_id);

	return 0;
}

static int rcar_audio_fe_pcm_copy_user(struct snd_pcm_substream *substream,
        int channel, unsigned long pos,
        struct iov_iter *iter, unsigned long bytes)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	void *hwbuf = runtime->dma_area + pos;
 
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		if (copy_from_iter(hwbuf, bytes, iter) != bytes)
		return -EFAULT;
	} else {
		if (copy_to_iter(hwbuf, bytes, iter) != bytes)
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
	.copy = rcar_audio_fe_pcm_copy_user,
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
			BUFFER_LEN_MAX, BUFFER_LEN_MAX);

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
	struct dsp_dbg_stream *dbg;

	if (!d)
		return;

	if (stream_dir == SNDRV_PCM_STREAM_PLAYBACK) {
		sub = d->pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream;
		hw_ptr = &d->rcar_pb_stream.hw_ptr_bytes;
		dbg = &d->dbg_playback;
	} else {
		sub = d->pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream;
		hw_ptr = &d->rcar_cap_stream.hw_ptr_bytes;
		dbg = &d->dbg_capture;
	}

	if (!sub || !sub->runtime)
		return;

	rt = sub->runtime;

	if (dbg->rd_avail_count < dbg->periods) {
		dbg->rd_avail_count++;
	} else {
		/* Discard old periodic frame */
		dbg->rd_off =
			(dbg->rd_off + dbg->period_bytes) %
			(dbg->periods * dbg->period_bytes);
	}

	/* Advance hardware pointer */
	*hw_ptr += frames_to_bytes(rt, rt->period_size);

	if (*hw_ptr >= frames_to_bytes(rt, rt->buffer_size))
		*hw_ptr = 0;

	if (dbg->wr_avail_count < dbg->periods) {
		dbg->wr_avail_count++;
	} else {
		/* Advance write offset */
		dbg->wr_off =
			(dbg->wr_off + dbg->period_bytes) %
			(dbg->periods * dbg->period_bytes);
	}

	/* Tell ALSA a period elapsed */
	snd_pcm_period_elapsed(sub);
}

static ssize_t rcar_audio_dbg_buf_read(struct file *file, char __user *ubuf,
		size_t count, loff_t *ppos)
{
	struct dsp_dbg_stream *dbg = file->private_data;
	void *snapshot;
	size_t n;
	ssize_t ret;

	mutex_lock(&dbg->lock);
	if (!dbg->valid || !dbg->cpu_addr ||
			!dbg->bytes || !dbg->rd_avail_count) {
		mutex_unlock(&dbg->lock);
		return -ENODATA;
	}

	n = dbg->period_bytes;
	snapshot = kmemdup(dbg->cpu_addr + dbg->rd_off, n, GFP_KERNEL);
	dbg->rd_off =
		(dbg->rd_off + dbg->period_bytes) %
		(dbg->periods * dbg->period_bytes);
	dbg->rd_avail_count--;
	mutex_unlock(&dbg->lock);

	if (!snapshot) {
		return -ENOMEM;
	}

	*ppos = 0;
	count = dbg->period_bytes;
	ret = simple_read_from_buffer(ubuf, count, ppos, snapshot, n);
	kfree(snapshot);
	return ret;
}

static ssize_t rcar_audio_dbg_buf_write(struct file *file,
		const char __user *ubuf, size_t count, loff_t *ppos)
{
	struct dsp_dbg_stream *dbg = file->private_data;
	void *tmp;

	mutex_lock(&dbg->lock);

	if (!dbg->valid || !dbg->cpu_addr ||
			!dbg->bytes || !dbg->wr_avail_count) {
		mutex_unlock(&dbg->lock);
		return -ENODATA;
	}

	mutex_unlock(&dbg->lock);

	tmp = memdup_user(ubuf, dbg->period_bytes);
	if (IS_ERR(tmp))
		return PTR_ERR(tmp);

	mutex_lock(&dbg->lock);

	memcpy(dbg->cpu_addr + dbg->wr_off, tmp, dbg->period_bytes);
	*ppos = 0;
	dbg->wr_off =
		(dbg->wr_off + dbg->period_bytes) %
		(dbg->periods * dbg->period_bytes);
	dbg->wr_avail_count--;
	mutex_unlock(&dbg->lock);

	kfree(tmp);
	return dbg->period_bytes;
}

static int rcar_audio_dbg_buf_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

static const struct file_operations rcar_audio_dbg_buf_fops = {
	.owner  = THIS_MODULE,
	.open   = rcar_audio_dbg_buf_open,
	.read   = rcar_audio_dbg_buf_read,
	.write  = rcar_audio_dbg_buf_write,
	.llseek = default_llseek,
};

static int rcar_audio_dbg_status_show(struct seq_file *m, void *p)
{
	struct dsp_dbg_stream *dbg = m->private;

	mutex_lock(&dbg->lock);
	seq_printf(m, "name=%s\n", dbg->name);
	seq_printf(m, "valid=%u\n", dbg->valid ? 1 : 0);
	seq_printf(m, "cpu_addr=%px\n", dbg->cpu_addr);
	seq_printf(m, "phy_addr=0x%llx\n", dbg->phy_addr);
	seq_printf(m, "bytes=%zu\n", dbg->bytes);
	seq_printf(m, "period_bytes=%zu\n", dbg->period_bytes);
	mutex_unlock(&dbg->lock);

	return 0;
}

static int rcar_audio_dbg_status_open(struct inode *inode, struct file *file)
{
	return single_open(file, rcar_audio_dbg_status_show, inode->i_private);
}

static const struct file_operations rcar_audio_dbg_status_fops = {
	.owner   = THIS_MODULE,
	.open    = rcar_audio_dbg_status_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

static int rcar_audio_debugfs_init(struct rcar_alsa_priv *d)
{
	struct dentry *root;
	char dir_name[15];

	sprintf(dir_name, "audio_fe_g%d", d->group_id);
	root = debugfs_create_dir(dir_name, NULL);
	if (IS_ERR(root)) {
		pr_err("rcar_audio_fe: %s: debugfs_create_dir err(%ld)\n",
				__func__, PTR_ERR(root));
		return PTR_ERR(root);
	}
	if (!root) {
		pr_err("rcar_audio_fe: %s: debugfs_create_dir failed\n",
				__func__);
		return -ENODEV;
	}

	d->dbg_root = root;

	debugfs_create_file("playback_status", 0400, root, &d->dbg_playback,
			&rcar_audio_dbg_status_fops);
	debugfs_create_file("capture_status", 0400, root, &d->dbg_capture,
			&rcar_audio_dbg_status_fops);

	debugfs_create_file("playback_buf", 0600, root, &d->dbg_playback,
			&rcar_audio_dbg_buf_fops);
	debugfs_create_file("capture_buf", 0600, root, &d->dbg_capture,
			&rcar_audio_dbg_buf_fops);

	return 0;
}

static void rcar_audio_debugfs_exit(struct rcar_alsa_priv *d)
{
	debugfs_remove_recursive(d->dbg_root);
	d->dbg_root = NULL;
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

	/* for debugfs */
	mutex_init(&d->dbg_playback.lock);
	mutex_init(&d->dbg_capture.lock);
	d->dbg_playback.name = "playback";
	d->dbg_capture.name = "capture";

	ret = rcar_audio_debugfs_init(d);
	if (ret) {
		dev_warn(&pdev->dev, "debugfs init failed: %d\n", ret);
	}

	dev_info(dev, "rcar_audio_fe: platform driver probed\n");
	return 0;
}

static void rcar_audio_remove(struct platform_device *pdev)
{
	struct rcar_alsa_priv *d = platform_get_drvdata(pdev);

	rcar_audio_debugfs_exit(d);

	if (d && d->card) {
		snd_card_free(d->card);
	}

	of_reserved_mem_device_release(&pdev->dev);
	global_alsa_priv[d->group_id] = NULL;

	dev_info(&pdev->dev, "rcar_audio_fe: platform driver removed\n");
}

static const struct of_device_id rcar_audio_of_match[] = {
	{ .compatible = "renesas,rcar_audio_fe" },
	{}
};
MODULE_DEVICE_TABLE(of, rcar_audio_of_match);

static struct platform_driver rcar_alsa_fe = {
	.probe = rcar_audio_probe,
	.remove = rcar_audio_remove,
	.driver = {
		.name = "rcar-audio-fe",
		.of_match_table = rcar_audio_of_match,
	},
};
module_platform_driver(rcar_alsa_fe);

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

static void rpmsg_cr_handle(struct work_struct *work)
{
	struct rpmsg_work *w =
		container_of(work, struct rpmsg_work, work);
	struct rcar_alsa_priv *d = w->priv;
	struct rpmsg_packet *msg = &w->msg;
	unsigned long flags;
	bool drop_msg = false;

	switch(msg->header.msg_type) {
	case OPEN_RESP:
	case EVENT_STARTED:
	case EVENT_PAUSED:
	case EVENT_RESUMED:
	case EVENT_STOPPED:
	case EVENT_CLOSED:
	case POS_REPLY_CR:
		spin_lock_irqsave(&d->cr_status_lock, flags);
		if (d->cr_reply_flag) {
			/* drop unhandled previous message */
			drop_msg = true;
		}

		d->cr_reply_flag = true;
		d->cr_reply_msg = *msg;
		spin_unlock_irqrestore(&d->cr_status_lock, flags);
		wake_up_interruptible(&d->cr_wait_q);

		if (drop_msg) {
			pr_warn("audio_ctrl_rpmsg: %s: "
					"Discarding unused previous message\n",
					__func__);
			drop_msg = false;
		}
		break;

	case EVENT_XRUN:
	case EVENT_HP_TAKEOVER:
	case EVENT_HP_RELEASED:
	case EVENT_ERROR:
		break;

	default:
		pr_warn("audio_ctrl_rpmsg: unknown cr message type %u\n",
				msg->header.msg_type);
		break;
	}
	kfree(w);
}

static void rpmsg_dsp_handle(struct work_struct *work)
{
	struct rpmsg_work *w =
		container_of(work, struct rpmsg_work, work);
	struct rcar_alsa_priv *d = w->priv;
	struct rpmsg_packet *msg = &w->msg;
	unsigned long flags;
	bool drop_msg = false;

	switch(msg->header.msg_type) {
	case CONFIG_REPLY:
	case PCM_STARTED:
	case PCM_PAUSED:
	case PCM_RESUMED:
	case PCM_STOPED:
	case PCM_CLOSED:
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
	struct rpmsg_work *w;

	if (len < sizeof(struct rpmsg_hdr)) {
		pr_err("audio_ctrl_rpmsg: %s failed: "
				"Invalid message length %d\n", __func__, len);
		return -EINVAL;
	}

	if (rpmsg_is_cr(rpdev->id.name, d)) {
		if (d->rpmsg_wq_cr) {
			w = kmalloc(sizeof(struct rpmsg_work), GFP_ATOMIC);
			if (w) {
				INIT_WORK(&w->work, rpmsg_cr_handle);
				w->msg = *msg;
				w->priv = d;
				queue_work(d->rpmsg_wq_cr, &w->work);
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
	} else {
		if (d->rpmsg_wq_dsp) {
			w = kmalloc(sizeof(struct rpmsg_work), GFP_ATOMIC);
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
	struct rpmsg_packet msg;
	bool is_cr = false;

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

	is_cr = rpmsg_is_cr(rpdev->id.name, d);
	if (is_cr) {
		d->rpmsg_wq_cr =
			create_singlethread_workqueue("rpmsg_cr_workqueue");
		if (!d->rpmsg_wq_cr) {
			dev_err(&rpdev->dev,
				"create_singlethread_workqueue for cr failed\n"
				);
			return -EPROTO;
		}

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

	/* send init mesage to remote core */
	msg.header.version = is_cr ?
		CR_CTRL_RPMSG_VERSION : DSP_CTRL_RPMSG_VERSION;
	msg.header.msg_type = RPMSG_INIT;
	ret = rpmsg_send(rpdev->ept, &msg, sizeof(msg));
	if (ret) {
		dev_err(&rpdev->dev, "rpmsg_send failed: %d\n", ret);

		if (is_cr) {
			d->cr_rpdev = NULL;
		} else {
			d->dsp_rpdev = NULL;
		}

		return ret;
	}

	if (is_cr) {
		init_waitqueue_head(&d->cr_wait_q);
		spin_lock_init(&d->cr_status_lock);
		d->cr_reply_flag = false;
	} else {
		init_waitqueue_head(&d->dsp_wait_q);
		spin_lock_init(&d->dsp_status_lock);
		d->dsp_reply_flag = false;
	}

	dev_info(&rpdev->dev, "rpmsg audio %s new channel: 0x%x -> 0x%x!\n",
			(is_cr ? "cr" : "dsp"), rpdev->src, rpdev->dst);

	return 0;
}

static void rpmsg_audio_ctrl_remove(struct rpmsg_device *rpdev)
{
	struct rcar_alsa_priv *d = dev_get_drvdata(&rpdev->dev);

	if (rpmsg_is_cr(rpdev->id.name, d)) {
		/* delete cr workqueue */
		flush_workqueue(d->rpmsg_wq_cr);
		destroy_workqueue(d->rpmsg_wq_cr);

		d->cr_rpdev = NULL;
	} else {
		/* delete dsp workqueue */
		flush_workqueue(d->rpmsg_wq_dsp);
		destroy_workqueue(d->rpmsg_wq_dsp);

		d->dsp_rpdev = NULL;
	}

	dev_info(&rpdev->dev, "rpmsg audio control driver is removed\n");
}

static struct rpmsg_device_id rpmsg_driver_audio_ctrl_id_table[] = {
	{ .name	= "audio_cr_ctrl_g0" },
	{ .name	= "audio_dsp_ctrl_g0" },
	{ .name	= "audio_cr_ctrl_g1" },
	{ .name	= "audio_dsp_ctrl_g1" },
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
MODULE_DESCRIPTION("Renesas R-Car ALSA audio control driver");
MODULE_AUTHOR("Jibin George");
