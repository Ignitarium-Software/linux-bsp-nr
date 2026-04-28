#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/kernel.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/of_reserved_mem.h>
#include <linux/dma-mapping.h>
#include <sound/soc.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#define BUFFER_LEN (65536U) /* 64kB */

/* Per-stream state */
struct dsp_pcm_stream {
    size_t hw_ptr_bytes;
};

struct dsp_pcm_dev {
    struct snd_card *card;
    struct snd_pcm *pcm;

    struct dsp_pcm_stream playback;
    struct dsp_pcm_stream capture;
};

#define TIMER_INTERVAL_NS (5333333UL) /* 5.33 ms */

static struct hrtimer period_timer;
static ktime_t kt_period;
static int stream_dir;

struct dsp_pcm_dev *global_pcm_dev = NULL;
EXPORT_SYMBOL_GPL(global_pcm_dev);

void dsp_pcm_handle_period(int stream_id);

static enum hrtimer_restart dsp_pcm_timer_fn(struct hrtimer *timer)
{
    hrtimer_forward_now(timer, kt_period);
    dsp_pcm_handle_period(stream_dir);

    return HRTIMER_RESTART;
}

static int dsp_pcm_open(struct snd_pcm_substream *sub)
{
    struct dsp_pcm_dev *d = snd_pcm_substream_chip(sub);
    struct snd_pcm_runtime *runtime = sub->runtime;

    pr_info("dsp_pcm: %s\n", __func__);

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
    runtime->hw.periods_min = 64;
    runtime->hw.periods_max = 64;

    runtime->private_data = d;

    return 0;
}

static int dsp_pcm_hw_params(struct snd_pcm_substream *sub, struct snd_pcm_hw_params *params)
{
    size_t buffer_bytes = params_buffer_bytes(params);

    if (buffer_bytes != BUFFER_LEN) {
        pr_err("dsp_pcm: %s error invalid buffer_bytes\n", __func__);
        return -EINVAL;
    }

    pr_info("dsp_pcm: %s buffer_bytes=%zu dma_addr=%pad\n", __func__, buffer_bytes, &sub->dma_buffer.addr);

    /* send prepare cmd to DSP with phys addr */

    return 0;
}

static int dsp_pcm_hw_free(struct snd_pcm_substream *sub)
{
    pr_info("dsp_pcm: %s\n", __func__);
    return 0;
}

static int dsp_pcm_prepare(struct snd_pcm_substream *sub)
{
    struct dsp_pcm_dev *d = snd_pcm_substream_chip(sub);

    pr_info("dsp_pcm: %s\n", __func__);

    if (sub->stream == SNDRV_PCM_STREAM_PLAYBACK) {
        d->playback.hw_ptr_bytes = 0;
    } else {
        d->capture.hw_ptr_bytes = 0;
    }

    stream_dir = sub->stream;

    return 0;
}

static int dsp_pcm_trigger(struct snd_pcm_substream *sub, int cmd)
{
    switch (cmd) {
    case SNDRV_PCM_TRIGGER_START:
        pr_info("dsp_pcm: %s START\n", __func__);
        hrtimer_start(&period_timer, kt_period, HRTIMER_MODE_REL);
        /* Notify DSP */
        return 0;

    case SNDRV_PCM_TRIGGER_STOP:
        pr_info("dsp_pcm: %s STOP\n", __func__);
        hrtimer_try_to_cancel(&period_timer);
        pr_info("dsp_pcm: %s STOP stopped hrtimer\n", __func__);
        /* Notify DSP */
        return 0;

    default:
        return -EINVAL;
    }
}

static snd_pcm_uframes_t dsp_pcm_pointer(struct snd_pcm_substream *sub)
{
    struct dsp_pcm_dev *d = snd_pcm_substream_chip(sub);
    size_t hwptr = (sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ? d->playback.hw_ptr_bytes: d->capture.hw_ptr_bytes;

    return bytes_to_frames(sub->runtime, hwptr);
}

static int dsp_pcm_mmap(struct snd_pcm_substream *sub, struct vm_area_struct *vma)
{
    pr_info("dsp_pcm: %s\n", __func__);
    return snd_pcm_lib_default_mmap(sub, vma);
}

static int dsp_pcm_close(struct snd_pcm_substream *sub)
{
    pr_info("dsp_pcm: %s\n", __func__);
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
    .open      = dsp_pcm_open,
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
static int dsp_pcm_create(struct platform_device *pdev, struct dsp_pcm_dev *d)
{
    int ret;

    pr_info("dsp_pcm: %s\n", __func__);

    /* Create sound card */
    ret = snd_card_new(&pdev->dev, -1, "dsp_pcm", THIS_MODULE, 0, &d->card);
    if (ret < 0)
        return ret;

    /* Create PCM instance: 1 playback, 1 capture */
    ret = snd_pcm_new(d->card, "dsp-pcm", 0, 1, 1, &d->pcm);
    if (ret < 0)
        return ret;

    d->pcm->private_data = d;

    /* Assign PCM ops */
    snd_pcm_set_ops(d->pcm, SNDRV_PCM_STREAM_PLAYBACK, &dsp_snd_pcm_ops);
    snd_pcm_set_ops(d->pcm, SNDRV_PCM_STREAM_CAPTURE, &dsp_snd_pcm_ops);

    snd_pcm_set_managed_buffer_all(d->pcm, SNDRV_DMA_TYPE_DEV, &pdev->dev, BUFFER_LEN, BUFFER_LEN);

    /* Register the card */
    ret = snd_card_register(d->card);
    if (ret < 0)
        return ret;

    dev_info(&pdev->dev, "dsp_pcm: ALSA card + PCM created successfully\n");
    return 0;
}

void dsp_pcm_handle_period(int stream_id)
{
    struct dsp_pcm_dev *d = global_pcm_dev;
    struct snd_pcm_substream *sub;
    struct snd_pcm_runtime *rt;
    size_t *hw_ptr;
    size_t buffer_bytes;
    size_t period_bytes;

    if (!d)
        return;

    /* Select stream */
    if (stream_id == SNDRV_PCM_STREAM_PLAYBACK)
        sub = d->pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream;
    else
        sub = d->pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream;

    if (!sub || !sub->runtime)
        return;

    rt = sub->runtime;

    buffer_bytes = frames_to_bytes(rt, rt->buffer_size);
    period_bytes = frames_to_bytes(rt, rt->period_size);

    /* Access the hw_ptr_bytes for the correct stream */
    hw_ptr = (stream_id == SNDRV_PCM_STREAM_PLAYBACK) ?
        &d->playback.hw_ptr_bytes : &d->capture.hw_ptr_bytes;

    /* Advance hardware pointer */
    *hw_ptr += period_bytes;

    if (*hw_ptr >= buffer_bytes)
        *hw_ptr = 0;

    /* Tell ALSA a period elapsed */
    snd_pcm_period_elapsed(sub);
}
EXPORT_SYMBOL_GPL(dsp_pcm_handle_period);

static int dsp_pcm_probe(struct platform_device *pdev)
{
    struct dsp_pcm_dev *d;
    int ret;

    pr_info("dsp_pcm: %s\n", __func__);

    d = devm_kzalloc(&pdev->dev, sizeof(*d), GFP_KERNEL);
    if (!d)
        return -ENOMEM;

    platform_set_drvdata(pdev, d);

    /* Attach this device to the reserved-memory pool from DT */
    ret = of_reserved_mem_device_init(&pdev->dev);
    if (ret) {
        dev_err(&pdev->dev, "of_reserved_mem_device_init failed: %d\n", ret);
        return ret;
    }

    ret = dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32));
    if (ret) {
        dev_err(&pdev->dev, "dma_set_coherent_mask failed: %d\n", ret);
        of_reserved_mem_device_release(&pdev->dev);
        return ret;
    }

    /* IMPORTANT: Create ALSA card + PCM device */
    ret = dsp_pcm_create(pdev, d);
    if (ret) {
        dev_err(&pdev->dev, "dsp_pcm_create failed: %d\n", ret);
        of_reserved_mem_device_release(&pdev->dev);
        return ret;
    }

    kt_period = ktime_set(0, TIMER_INTERVAL_NS);
    hrtimer_init(&period_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    period_timer.function = dsp_pcm_timer_fn;

    global_pcm_dev = d;

    dev_info(&pdev->dev, "dsp_pcm: platform driver probed\n");
    return 0;
}

static int dsp_pcm_remove(struct platform_device *pdev)
{
    struct dsp_pcm_dev *d = platform_get_drvdata(pdev);

    if (d && d->card) {
        snd_card_free(d->card);
    }

    of_reserved_mem_device_release(&pdev->dev);

    dev_info(&pdev->dev, "dsp_pcm: platform driver removed\n");
    return 0;
}

static const struct of_device_id dsp_pcm_of_match[] = {
    { .compatible = "renesas,dsp-pcm-legacy" },
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

MODULE_LICENSE("GPL");
