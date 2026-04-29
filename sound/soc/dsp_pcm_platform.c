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
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <sound/soc.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#define BUFFER_LEN (4096U) /* 4kB */
#define MAX_DEVICES (4U) /* Maximum number of pairs ALSA instances */

/* DSP Control Message type */
#define kConfigReq       0x2001   /* CA→DSP */
#define kPcmStart        0x2002   /* CA→DSP */
#define kPcmPause        0x2003   /* CA→DSP */
#define kPcmResume       0x2004   /* CA→DSP */
#define kPcmStop         0x2005   /* CA→DSP */
#define kPosQuery        0x2006   /* CA→DSP */
#define kStatus          0x8000   /* DSP→CA (reply/heartbeat/event) */
#define kConfigReply     0xA001   /* DSP→CA (config reply) */
#define kPosReply        0xA006   /* DSP→CA (position/cursor reply) */

struct RpMsgHdr {
    uint16_t version;
    uint16_t msg_type;
    uint32_t msg_id;
    uint32_t stream_id;
    uint32_t payload_len;
} __attribute__((packed));

struct OpenReqMesssage {
    uint32_t dir;               /* playback=0, capture=1 */
    uint32_t rate;              /* app rate */
    uint32_t channels;          /* app channels */
    uint32_t format;            /* ALSA snd_pcm_format_t */
    uint32_t period_frames;     /* ALSA period size in frames */
    uint32_t periods;           /* period count */
    uint64_t pcm_rb_phys;       /* phys addr of PCM RB */
    uint32_t pcm_rb_size;
} __attribute__((packed));

struct OpenRespMessage {
    int32_t status;
    int32_t stream_id;
    uint32_t hw_rate;
    uint32_t hw_channels;
    uint32_t hw_format;
    uint64_t hw_rb_phys;
    uint32_t hw_rb_size;
} __attribute__((packed));

struct TriggerReq {
    uint32_t cmd;              /* START/STOP/PAUSE/RESUME/DRAIN */
} __attribute__((packed));

struct DspConfigReqMsg {
    uint32_t dir;               /* playback=0, capture=1 */
    uint32_t in_rate;           /* app rate for PB/ Hw rate for REC */
    uint32_t in_channels;
    uint32_t in_format;         /* ALSA format */
    uint32_t out_rate;          /* Hw rate for PB 48000/app rate for REC */
    uint32_t out_channels;
    uint32_t out_format;
    uint64_t pcm_rb_phys;       /* DSP write/read target for DSP */
    uint32_t pcm_rb_size;
    uint64_t hw_rb_phys;
    uint32_t hw_rb_size;
    uint32_t period_frames;     /* matches ALSA period */
    uint32_t periods;
} __attribute__((packed));

struct DspStatusMsg {
    uint32_t dir;               /* playback=0, capture=1 */
    uint32_t status;            /* RUNNING, STOPPED, PAUSED, etc. */
    uint64_t pcm_dsp_pos;       /* frames consumed since stream start (monotonic) */
    uint64_t hw_rb_write_pos;   /* optional: frames written to LP HW RB */
    uint32_t period_frames;     /* echo for safety */
    uint32_t rb_frames;         /* total frames in PCM RB */
} __attribute__((packed));

union RpMsgPayload {
    struct TriggerReq trigger;
    struct DspConfigReqMsg dsp_config;
    struct DspStatusMsg dsp_status;
};

struct RpMsgPacket {
    struct RpMsgHdr header;
    union RpMsgPayload payload;
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

    volatile int dsp_reply_flag;
    volatile struct RpMsgPacket dsp_reply_msg;

    spinlock_t dsp_status_lock;
};

struct rpmsg_work_dsp {
    struct work_struct work;
    struct RpMsgPacket msg;
    struct rcar_alsa_priv priv;
};

struct rcar_alsa_priv *global_alsa_priv[MAX_DEVICES];

static int rpmsg_send_dsp(struct RpMsgPacket msg, struct rcar_alsa_priv *d)
{
    if (!d->dsp_rpdev) {
        pr_err("rcar_audio_fe: %s failed: No rpmsg_device available\n", __func__);
        return -ENODEV;
    }

    return rpmsg_send(d->dsp_rpdev->ept, &msg, sizeof(msg));
}

static struct DspStatusMsg rpmsg_recv_dsp_blocking(int *status, unsigned int timeout_ms, struct rcar_alsa_priv *d)
{
    unsigned long flags;
    int temp_reply;
    struct RpMsgPacket temp_msg;

    do {
        spin_lock_irqsave(&d->dsp_status_lock, flags);
        temp_reply = d->dsp_reply_flag;
        temp_msg = d->dsp_reply_msg;
        spin_unlock_irqrestore(&d->dsp_status_lock, flags);

        if (temp_reply) {
            *status = temp_msg.header.msg_type;

            spin_lock_irqsave(&d->dsp_status_lock, flags);
            d->dsp_reply_flag = 0;
            spin_unlock_irqrestore(&d->dsp_status_lock, flags);

            return temp_msg.payload.dsp_status;
        }

        if (timeout_ms != 0) msleep(1);
    } while(timeout_ms --);

    return temp_msg.payload.dsp_status;
}

static int rcar_alsa_fe_pcm_open(struct snd_pcm_substream *sub)
{
    struct rcar_alsa_priv *d = snd_pcm_substream_chip(sub);
    struct snd_pcm_runtime *runtime = sub->runtime;

    pr_info("rcar_audio_fe: %s\n", __func__);

    runtime->hw.info = SNDRV_PCM_INFO_INTERLEAVED |
            SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
            SNDRV_PCM_INFO_BLOCK_TRANSFER;
    runtime->hw.formats = SNDRV_PCM_FMTBIT_S16_LE;
    runtime->hw.rates = SNDRV_PCM_RATE_48000;
    runtime->hw.rate_min = 48000;
    runtime->hw.rate_max = 48000;
    runtime->hw.channels_min = 2;
    runtime->hw.channels_max = 2;
    runtime->hw.buffer_bytes_max = BUFFER_LEN;
    runtime->hw.period_bytes_min = 1024;
    runtime->hw.period_bytes_max = 1024;
    runtime->hw.periods_min = 4;
    runtime->hw.periods_max = 4;

    runtime->private_data = d;

    return 0;
}

static int dsp_pcm_hw_params(struct snd_pcm_substream *sub, struct snd_pcm_hw_params *params)
{
    size_t buffer_bytes = params_buffer_bytes(params);
    struct rcar_alsa_priv *d = snd_pcm_substream_chip(sub);
    struct platform_device *pdev = d->pdev;
    struct rcar_alsa_stream *s = (sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ? &d->rcar_pb_stream : &d->rcar_cap_stream;

    if (buffer_bytes != BUFFER_LEN) {
        pr_err("rcar_audio_fe: %s error invalid buffer_bytes\n", __func__);
        return -EINVAL;
    }

    snd_pcm_set_managed_buffer(sub, SNDRV_DMA_TYPE_DEV, &pdev->dev, BUFFER_LEN, BUFFER_LEN);

    pr_info("audio_ctrl_rpmsg: %s PCM buffer_bytes=%zu PCM dma_addr=%pad\n", __func__, buffer_bytes, &sub->dma_buffer.addr);

    s->hw_buf_size = BUFFER_LEN;
    s->hw_cpu_addr = dma_alloc_coherent(&pdev->dev, s->hw_buf_size, &s->hw_dma_handle, GFP_KERNEL);
    if (!s->hw_cpu_addr) {
        pr_err("rcar_audio_fe: %s dma_alloc_coherent failed\n", __func__);
        return -ENOMEM;
    }

    pr_info("audio_ctrl_rpmsg: %s HW buffer_bytes=%zu HW dma_addr=%pad\n", __func__, s->hw_buf_size, &s->hw_dma_handle);

    struct RpMsgPacket msg;
    int status;
    struct DspStatusMsg dsp_status;

    /* header */
    msg.header.version = 1;
    msg.header.msg_type = kConfigReq;
    msg.header.msg_id = 1;
    msg.header.stream_id = (sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ? 0: 1;
    msg.header.payload_len = sizeof(struct DspConfigReqMsg);

    /* Payload */
    msg.payload.dsp_config.dir = (sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ? 0: 1;
    msg.payload.dsp_config.in_rate = 48000;
    msg.payload.dsp_config.in_channels = 2;
    msg.payload.dsp_config.in_format = SNDRV_PCM_FMTBIT_S16_LE;
    msg.payload.dsp_config.out_rate = 48000;
    msg.payload.dsp_config.out_channels = 2;
    msg.payload.dsp_config.out_format = SNDRV_PCM_FMTBIT_S16_LE;
    msg.payload.dsp_config.pcm_rb_phys = (uint64_t)dma_to_phys(&pdev->dev, sub->dma_buffer.addr);
    msg.payload.dsp_config.pcm_rb_size = (uint64_t)buffer_bytes;
    msg.payload.dsp_config.hw_rb_phys = (uint64_t)dma_to_phys(&pdev->dev, s->hw_dma_handle);
    msg.payload.dsp_config.hw_rb_size = (uint64_t)s->hw_buf_size;
    msg.payload.dsp_config.period_frames = 256;
    msg.payload.dsp_config.periods = 4;

    rpmsg_send_dsp(msg, d);

    /* Wait for response from DSP */
    dsp_status = rpmsg_recv_dsp_blocking(&status, 5, d);
    if (status != kConfigReply) {
        pr_err("rcar_audio_fe: %s: error response from DSP\n", __func__);
        return -EAGAIN;
    }

    return 0;
}

static int dsp_pcm_hw_free(struct snd_pcm_substream *sub)
{
    struct rcar_alsa_priv *d = snd_pcm_substream_chip(sub);
    struct platform_device *pdev = d->pdev;
    struct rcar_alsa_stream *s = (sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ? &d->rcar_pb_stream : &d->rcar_cap_stream;

    pr_info("rcar_audio_fe: %s\n", __func__);

    /* Free HW buffer */
    dma_free_coherent(&pdev->dev, s->hw_buf_size, s->hw_cpu_addr, s->hw_dma_handle);
    s->hw_cpu_addr = NULL;

    return 0;
}

static int dsp_pcm_prepare(struct snd_pcm_substream *sub)
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

static int dsp_pcm_trigger(struct snd_pcm_substream *sub, int cmd)
{
    struct rcar_alsa_priv *d = snd_pcm_substream_chip(sub);
    struct RpMsgPacket msg;

    switch (cmd) {
    case SNDRV_PCM_TRIGGER_START:
        pr_info("rcar_audio_fe: %s START\n", __func__);

        /* Notify DSP */
        msg.header.version = 1;
        msg.header.msg_type = kPcmStart;
        msg.header.msg_id = 1;
        msg.header.stream_id = (sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ? 0: 1;
        msg.header.payload_len = sizeof(struct TriggerReq);
        msg.payload.trigger.cmd = kPcmStart;

        rpmsg_send_dsp(msg, d);
        return 0;

    case SNDRV_PCM_TRIGGER_RESUME:
        pr_info("rcar_audio_fe: %s RESUME\n", __func__);

        /* Notify DSP */
        msg.header.version = 1;
        msg.header.msg_type = kPcmResume;
        msg.header.msg_id = 1;
        msg.header.stream_id = (sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ? 0: 1;
        msg.header.payload_len = sizeof(struct TriggerReq);
        msg.payload.trigger.cmd = kPcmResume;
        rpmsg_send_dsp(msg, d);

        return 0;

    case SNDRV_PCM_TRIGGER_STOP:
        pr_info("rcar_audio_fe: %s STOP\n", __func__);

        /* Notify DSP */
        msg.header.version = 1;
        msg.header.msg_type = kPcmStop;
        msg.header.msg_id = 1;
        msg.header.stream_id = (sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ? 0: 1;
        msg.header.payload_len = sizeof(struct TriggerReq);
        msg.payload.trigger.cmd = kPcmStop;
        rpmsg_send_dsp(msg, d);
        return 0;

    case SNDRV_PCM_TRIGGER_SUSPEND:
        pr_info("rcar_audio_fe: %s SUSPEND\n", __func__);

        /* Notify DSP */
        msg.header.version = 1;
        msg.header.msg_type = kPcmPause;
        msg.header.msg_id = 1;
        msg.header.stream_id = (sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ? 0: 1;
        msg.header.payload_len = sizeof(struct TriggerReq);
        msg.payload.trigger.cmd = kPcmPause;
        rpmsg_send_dsp(msg, d);

        return 0;

    default:
        return -EINVAL;
    }
}

static snd_pcm_uframes_t dsp_pcm_pointer(struct snd_pcm_substream *sub)
{
    struct rcar_alsa_priv *d = snd_pcm_substream_chip(sub);
    size_t hwptr = (sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ? d->rcar_pb_stream.hw_ptr_bytes: d->rcar_cap_stream.hw_ptr_bytes;

    return bytes_to_frames(sub->runtime, hwptr);
}

static int dsp_pcm_mmap(struct snd_pcm_substream *sub, struct vm_area_struct *vma)
{
    pr_info("rcar_audio_fe: %s\n", __func__);
    return snd_pcm_lib_default_mmap(sub, vma);
}

static int dsp_pcm_close(struct snd_pcm_substream *sub)
{
    pr_info("rcar_audio_fe: %s\n", __func__);
    return 0;
}

static int dsp_pcm_copy_user(struct snd_pcm_substream *substream, int channel, unsigned long pos,
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

static const struct snd_pcm_ops dsp_snd_pcm_ops = {
    .open      = rcar_alsa_fe_pcm_open,
    .close     = dsp_pcm_close,
    .hw_params = dsp_pcm_hw_params,
    .hw_free   = dsp_pcm_hw_free,
    .prepare   = dsp_pcm_prepare,
    .trigger   = dsp_pcm_trigger,
    .pointer   = dsp_pcm_pointer,
    .mmap      = dsp_pcm_mmap,
    .copy_user = dsp_pcm_copy_user,
};

/* ALSA card + PCM creation */
static int dsp_pcm_create(struct platform_device *pdev, struct rcar_alsa_priv *d)
{
    int ret;

    pr_info("rcar_audio_fe: %s\n", __func__);

    /* Create sound card */
    ret = snd_card_new(&pdev->dev, -1, "rcar_alsa_fe", THIS_MODULE, 0, &d->card);
    if (ret < 0)
        return ret;

    /* Create PCM instance: 1 playback, 1 capture */
    ret = snd_pcm_new(d->card, "rcar_alsa_fe_pcm", 0, 1, 1, &d->pcm);
    if (ret < 0)
        return ret;

    d->pcm->private_data = d;

    /* Assign PCM ops */
    snd_pcm_set_ops(d->pcm, SNDRV_PCM_STREAM_PLAYBACK, &dsp_snd_pcm_ops);
    snd_pcm_set_ops(d->pcm, SNDRV_PCM_STREAM_CAPTURE, &dsp_snd_pcm_ops);

    /* Register the card */
    ret = snd_card_register(d->card);
    if (ret < 0)
        return ret;

    dev_info(&pdev->dev, "rcar_audio_fe: ALSA card + PCM created successfully\n");
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

static int dsp_pcm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
    struct rcar_alsa_priv *d;
    int ret;
    int gid;

    pr_info("rcar_audio_fe: %s\n", __func__);

    ret = of_property_read_u32(np, "rcar,group-id", &gid);
    if (ret) {
        dev_err(dev, "rcar,group-id not mentioned in DT, ret: %d\n", ret);
        return ret;
    }

    if (global_alsa_priv[gid] == NULL) {
        global_alsa_priv[gid] = devm_kzalloc(&pdev->dev, sizeof(*d), GFP_KERNEL);
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
        dev_err(dev, "rcar,rpmsg-cr-chan not mentioned in DT, ret: %d\n", ret);
        return ret;
    }

    ret = of_property_read_string(np, "rcar,rpmsg-dsp-chan", &d->dsp_chan);
    if (ret) {
        dev_err(dev, "rcar,rpmsg-dsp-chan not mentioned in DT, ret: %d\n", ret);
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

    /* IMPORTANT: Create ALSA card + PCM device */
    ret = dsp_pcm_create(pdev, d);
    if (ret) {
        dev_err(dev, "dsp_pcm_create failed: %d\n", ret);
        of_reserved_mem_device_release(dev);
        return ret;
    }

    dev_info(dev, "rcar_audio_fe: platform driver probed\n");
    return 0;
}

static int dsp_pcm_remove(struct platform_device *pdev)
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

static const struct of_device_id dsp_pcm_of_match[] = {
    { .compatible = "renesas,rcar_audio_fe" },
    {}
};
MODULE_DEVICE_TABLE(of, dsp_pcm_of_match);

static struct platform_driver dsp_pcm_driver = {
    .probe = dsp_pcm_probe,
    .remove = dsp_pcm_remove,
    .driver = {
        .name = "dsp-pcm-platform",
        .of_match_table = dsp_pcm_of_match,
    },
};
module_platform_driver(dsp_pcm_driver);

static int rpmsg_parse_gid(char *name)
{
    int i;

    for (i = 0; i < MAX_DEVICES; i++) {
        if (global_alsa_priv[i]) {
            if (!strncmp(global_alsa_priv[i]->cr_chan, name, strlen(name)) ||
                !strncmp(global_alsa_priv[i]->dsp_chan, name, strlen(name))) {
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
    struct rpmsg_work_dsp *w = container_of(work, struct rpmsg_work_dsp, work);
    struct rcar_alsa_priv *d = &w->priv;
    struct RpMsgPacket *msg = &w->msg;
    unsigned long flags;

    switch(msg->header.msg_type) {
    case kConfigReply:
    case kPosReply:
        spin_lock_irqsave(&d->dsp_status_lock, flags);
        d->dsp_reply_flag = 1;
        d->dsp_reply_msg = *msg;
        spin_unlock_irqrestore(&d->dsp_status_lock, flags);
        break;

    case kStatus:
        dsp_pcm_handle_period(msg->payload.dsp_status.dir, d);
        break;

    default:
        pr_warn("audio_ctrl_rpmsg: unknown dsp message type %u\n", msg->header.msg_type);
        break;
    }
    kfree(w);
}

static int rpmsg_audio_ctrl_cb(struct rpmsg_device *rpdev, void *data, int len,
    void *priv, u32 src)
{
    struct rcar_alsa_priv *d = dev_get_drvdata(&rpdev->dev);
    struct RpMsgPacket *msg = data;
    struct rpmsg_work_dsp *w;

    if (len < sizeof(struct RpMsgHdr)) {
        pr_err("audio_ctrl_rpmsg: %s failed: Invalid message length %d\n", __func__, len);
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
                w->priv = *d;
                queue_work(d->rpmsg_wq_dsp, &w->work);
            }
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
        dev_err(&rpdev->dev, "ALSA driver for instance-%d is not probed\n", gid);
        return -EPROTO;
    }

    d = global_alsa_priv[gid];
    dev_set_drvdata(&rpdev->dev, d);

    if (rpmsg_is_cr(rpdev->id.name, d)) {
        d->cr_rpdev = rpdev;
    } else {
        d->rpmsg_wq_dsp = create_workqueue("rpmsg_dsp_workqueue");
        if (!d->rpmsg_wq_dsp) {
            dev_err(&rpdev->dev, "create_workqueue for dsp failed\n");
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

    spin_lock_init(&d->dsp_status_lock);

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
