/* SPDX-License-Identifier: GPL-2.0 */
/*
 * X5H Gen5 Audio — ALSA Interface Driver
 *
 * Depends on x5h-audio-hw.ko for hardware pipeline, DMA buffer,
 * and control.  Registers an ASoC component with 2 DAI links.
 * Uses the HW driver's pre-allocated PCM buffer directly.
 *
 * Copyright (C) 2025 Renesas Electronics Corp.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>

#include "x5h_audio.h"

#define DRV_NAME "x5h-audio-alsa"

/* ============================================================================
 *  Period-elapsed bridge:  x5h_audio.c DMA callback → ALSA
 * ============================================================================
 */
static void x5h_alsa_period_elapsed(void *data)
{
	struct snd_pcm_substream *substream = data;
	snd_pcm_period_elapsed(substream);
}

/* ============================================================================
 *  PCM hardware constraint template
 * ============================================================================
 */
static const struct snd_pcm_hardware x5h_pcm_hardware = {
	.info =	SNDRV_PCM_INFO_INTERLEAVED	|
		SNDRV_PCM_INFO_MMAP		|
		SNDRV_PCM_INFO_MMAP_VALID,
	.buffer_bytes_max	= X5H_AUDIO_PCM_BUFFER_SIZE,
	.period_bytes_min	= 32,
	.period_bytes_max	= X5H_AUDIO_PCM_BUFFER_SIZE / 2,
	.periods_min		= 2,
	.periods_max		= X5H_AUDIO_PCM_PERIODS_MAX,
	.fifo_size		= 256,
};

/* ============================================================================
 *  DAI operations
 * ============================================================================
 */
static int x5h_stream_id(struct snd_soc_dai *dai,
        struct snd_pcm_substream *substream)
{
    if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
        return X5H_STREAM_ID_CAP;
    return dai->id;
}

static int x5h_soc_startup(struct snd_pcm_substream *substream,
        struct snd_soc_dai *dai)
{
	struct x5h_audio *ctx = x5h_audio_get_global_instance();
	struct snd_pcm_runtime *runtime = substream->runtime;
	int id = x5h_stream_id(dai, substream);

	if (!ctx)
		return -ENODEV;

	x5h_audio_set_direction(ctx, substream->stream == SNDRV_PCM_STREAM_PLAYBACK);

	snd_soc_set_runtime_hwparams(substream, &x5h_pcm_hardware);

	/* Use the HW driver's pre-allocated buffer as the PCM buffer */
	runtime->dma_area  = x5h_audio_get_pcm_buffer(ctx, id);
	runtime->dma_addr  = x5h_audio_get_pcm_dma_addr(ctx, id);
	runtime->dma_bytes = x5h_audio_get_pcm_buffer_size(ctx);

	snd_pcm_hw_constraint_integer(runtime,
				      SNDRV_PCM_HW_PARAM_PERIODS);

	return 0;
}

static int x5h_soc_prepare(struct snd_pcm_substream *substream,
			   struct snd_soc_dai *dai)
{
	/* DMA is always running; nothing to prepare */
	return 0;
}

static int x5h_soc_trigger(struct snd_pcm_substream *substream, int cmd,
			   struct snd_soc_dai *dai)
{
	struct x5h_audio *ctx = x5h_audio_get_global_instance();
	int id = x5h_stream_id(dai, substream);

	if (!ctx)
		return -ENODEV;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		x5h_audio_set_period_cb(ctx, id, x5h_alsa_period_elapsed,
					substream);
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		x5h_audio_set_period_cb(ctx, id, NULL, NULL);
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static void x5h_soc_shutdown(struct snd_pcm_substream *substream,
			     struct snd_soc_dai *dai)
{
	struct x5h_audio *ctx = x5h_audio_get_global_instance();
	int id = x5h_stream_id(dai, substream);

	if (ctx)
		x5h_audio_set_period_cb(ctx, id, NULL, NULL);
}

static int x5h_soc_set_dai_tdm_slot(struct snd_soc_dai *dai,
				    u32 tx_mask, u32 rx_mask,
				    int slots, int slot_width)
{
	return 0;
}

static int x5h_soc_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	return 0;
}

static u64 x5h_soc_dai_formats[] = {
	SND_SOC_POSSIBLE_DAIFMT_I2S	|
	SND_SOC_POSSIBLE_DAIFMT_RIGHT_J	|
	SND_SOC_POSSIBLE_DAIFMT_LEFT_J	|
	SND_SOC_POSSIBLE_DAIFMT_NB_NF	|
	SND_SOC_POSSIBLE_DAIFMT_NB_IF	|
	SND_SOC_POSSIBLE_DAIFMT_IB_NF	|
	SND_SOC_POSSIBLE_DAIFMT_IB_IF,
	SND_SOC_POSSIBLE_DAIFMT_DSP_A	|
	SND_SOC_POSSIBLE_DAIFMT_DSP_B,
};

/* ============================================================================
 *  Component operations
 * ============================================================================
 */
static int x5h_snd_probe(struct snd_soc_component *component)
{
	return 0;
}

static int x5h_hw_params(struct snd_soc_component *component,
			 struct snd_pcm_substream *substream,
			 struct snd_pcm_hw_params *hw_params)
{
	return 0;
}

static int x5h_hw_free(struct snd_soc_component *component,
		       struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;

	/* Prevent ALSA from freeing the HW driver's buffer */
	runtime->dma_area  = NULL;
	runtime->dma_addr  = 0;
	runtime->dma_bytes = 0;

	return 0;
}

static snd_pcm_uframes_t x5h_soc_pointer(struct snd_soc_component *component,
					 struct snd_pcm_substream *substream)
{
	struct x5h_audio *ctx = x5h_audio_get_global_instance();
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = asoc_rtd_to_cpu(rtd, 0);
	int id = x5h_stream_id(cpu_dai, substream);
	unsigned int pos;

	if (!ctx)
		return 0;

	pos = x5h_audio_dma_pointer(ctx, id,
				    x5h_audio_get_pcm_buffer_size(ctx));

	return bytes_to_frames(substream->runtime, pos);
}

/* ============================================================================
 *  PCM new — no preallocation (buffer owned by HW driver)
 * ============================================================================
 */
static int x5h_pcm_new(struct snd_soc_pcm_runtime *rtd,
		       struct snd_soc_dai *dai)
{
	return 0;
}

/* ============================================================================
 *  DAI and Component definitions
 * ============================================================================
 */
static const struct snd_soc_dai_ops x5h_dai_ops = {
	.startup	= x5h_soc_startup,
	.shutdown	= x5h_soc_shutdown,
	.trigger	= x5h_soc_trigger,
	.set_fmt	= x5h_soc_set_fmt,
	.set_tdm_slot	= x5h_soc_set_dai_tdm_slot,
	.prepare	= x5h_soc_prepare,
	.auto_selectable_formats	= x5h_soc_dai_formats,
	.num_auto_selectable_formats	= ARRAY_SIZE(x5h_soc_dai_formats),
};

static struct snd_soc_dai_driver x5h_dai[] = {
	{
		.name = "x5h-dai-0",
		.id = 0,
		.playback = {
			.rates		= SNDRV_PCM_RATE_8000_192000,
			.formats	= SNDRV_PCM_FMTBIT_S16_LE |
					  SNDRV_PCM_FMTBIT_S24_LE |
					  SNDRV_PCM_FMTBIT_S32_LE,
			.channels_min	= 1,
			.channels_max	= 8,
		},
		.capture = {
			.rates		= SNDRV_PCM_RATE_8000_192000,
			.formats	= SNDRV_PCM_FMTBIT_S16_LE |
					  SNDRV_PCM_FMTBIT_S24_LE |
					  SNDRV_PCM_FMTBIT_S32_LE,
			.channels_min	= 1,
			.channels_max	= 8,
		},
		.pcm_new = x5h_pcm_new,
		.ops = &x5h_dai_ops,
	},
	{
		.name = "x5h-dai-1",
		.id = 1,
		.playback = {
			.rates		= SNDRV_PCM_RATE_8000_192000,
			.formats	= SNDRV_PCM_FMTBIT_S16_LE |
					  SNDRV_PCM_FMTBIT_S24_LE |
					  SNDRV_PCM_FMTBIT_S32_LE,
			.channels_min	= 1,
			.channels_max	= 8,
		},
		.pcm_new = x5h_pcm_new,
		.ops = &x5h_dai_ops,
	}
};

static const struct snd_soc_component_driver x5h_component = {
	.name			= DRV_NAME,
	.probe			= x5h_snd_probe,
	.hw_params		= x5h_hw_params,
	.hw_free		= x5h_hw_free,
	.pointer		= x5h_soc_pointer,
	.legacy_dai_naming	= 1,
};

/* ============================================================================
 *  Platform driver — binds to child device created by x5h-audio-hw
 * ============================================================================
 */
static int x5h_alsa_probe(struct platform_device *pdev)
{
	struct x5h_audio *ctx = x5h_audio_get_global_instance();

	if (!ctx) {
		dev_err(&pdev->dev, "HW driver not yet probed\n");
		return -EPROBE_DEFER;
	}

	dev_set_drvdata(&pdev->dev, ctx);

	return devm_snd_soc_register_component(&pdev->dev, &x5h_component,
					       x5h_dai, ARRAY_SIZE(x5h_dai));
}

static int x5h_alsa_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver x5h_alsa_driver = {
	.driver = {
		.name	= "x5h-audio-alsa",
	},
	.probe	= x5h_alsa_probe,
	.remove	= x5h_alsa_remove,
};
module_platform_driver(x5h_alsa_driver);

MODULE_DESCRIPTION("X5H Gen5 Audio ALSA Interface Driver");
MODULE_AUTHOR("Renesas Electronics Corp.");
MODULE_LICENSE("GPL");
MODULE_SOFTDEP("pre: x5h-audio-hw");
