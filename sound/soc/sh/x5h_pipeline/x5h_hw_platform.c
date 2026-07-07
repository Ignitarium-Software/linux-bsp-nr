/* SPDX-License-Identifier: GPL-2.0 */
/*
 * X5H Gen5 Audio — Hardware Platform Driver
 *
 * Owns all hardware resources: DT parsing, clocks, ioremap, DMA channels,
 * pipeline init/start/stop, and buffer allocation.
 * Exports the x5h_audio context to the ALSA interface driver.
 *
 * Copyright (C) 2025 Renesas Electronics Corp.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/dmaengine.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include "x5h_audio.h"

#define DRV_NAME "x5h-audio-hw"

struct x5h_hw_priv {
	struct device *dev;
	struct x5h_audio audio_ctx;
	struct x5h_audio_config cfg;

	struct clk *clk_adg;
	struct clk *clk_ssi;
	struct clk *clk_scu;
	struct clk *clk_ssi_idx;
	struct clk *clk_src_idx;
	struct clk *clk_src2_idx;
	struct clk *clk_dvc_idx;
	struct clk *clk_ctu_idx;
	struct clk *clk_dmapp1;
	struct clk *clk_dmapp2;
	struct clk *clk_a;
	struct clk *clk_b;
	struct clk *clk_c;
	struct clk *clkout;

	void __iomem *scu_base;
	phys_addr_t   scu_phys_base;
	void __iomem *adg_base;
	void __iomem *ssiu_base;
	phys_addr_t   ssiu_phys_base;
	void __iomem *dma_base;

	int ssi_irq;

	struct platform_device *alsa_pdev;
};

/* ============================================================================
 *  DT parsing
 * ============================================================================
 */
#define RES_NAME_LEN 10
static int x5h_parse_dt(struct x5h_hw_priv *priv, struct platform_device *pdev)
{
	struct x5h_audio_config *cfg = &priv->cfg;
	struct resource *res;
	unsigned int busif;
    char res_name[RES_NAME_LEN];

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "scu");
	if (!res)
		return -ENODEV;
	priv->scu_phys_base = res->start;
	priv->scu_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(priv->scu_base))
		return PTR_ERR(priv->scu_base);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "adg");
	if (!res)
		return -ENODEV;
	priv->adg_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(priv->adg_base))
		return PTR_ERR(priv->adg_base);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ssiu");
	if (!res)
		return -ENODEV;
	priv->ssiu_phys_base = res->start;
	priv->ssiu_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(priv->ssiu_base))
		return PTR_ERR(priv->ssiu_base);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "audmapp");
	if (res) {
		priv->dma_base = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(priv->dma_base))
			priv->dma_base = NULL;
	}

	priv->clk_adg = devm_clk_get(&pdev->dev, "adg");
	priv->clk_ssi = devm_clk_get(&pdev->dev, "ssi-all");
	priv->clk_scu = devm_clk_get(&pdev->dev, "scu-all");

    snprintf(res_name, RES_NAME_LEN, "ssi%d", X5H_AUDIO_SSI_ID);
	priv->clk_ssi_idx = devm_clk_get(&pdev->dev, res_name);

    snprintf(res_name, RES_NAME_LEN, "src%d", X5H_AUDIO_SRC_ID);
	priv->clk_src_idx = devm_clk_get(&pdev->dev, res_name);

    snprintf(res_name, RES_NAME_LEN, "src%d", X5H_AUDIO_SRC_ID2);
    priv->clk_src2_idx = devm_clk_get(&pdev->dev, res_name);

    snprintf(res_name, RES_NAME_LEN, "dvc%d", X5H_AUDIO_CTU_ID);
    priv->clk_dvc_idx = devm_clk_get(&pdev->dev, res_name);

    snprintf(res_name, RES_NAME_LEN, "ctu%d", X5H_AUDIO_DVC_ID);
	priv->clk_ctu_idx = devm_clk_get(&pdev->dev, res_name);

	priv->clk_dmapp1 = devm_clk_get(&pdev->dev, "dmapp.0");
	priv->clk_dmapp2 = devm_clk_get(&pdev->dev, "dmapp.1");

	priv->clk_a = devm_clk_get(&pdev->dev, "clk_a");
	priv->clk_b = devm_clk_get(&pdev->dev, "clk_b");
	priv->clk_c = devm_clk_get(&pdev->dev, "clk_c");

	cfg->scu_base  = priv->scu_base;
	cfg->adg_base  = priv->adg_base;
	cfg->ssiu_base = priv->ssiu_base;
	cfg->dma_base  = priv->dma_base;

	cfg->clk_a_rate = X5H_AUDIO_CLK_A_RATE;
	cfg->clk_b_rate = 0;
	cfg->clk_c_rate = 0;
	cfg->clkout_rate = X5H_AUDIO_CLKOUT_RATE;

	priv->clkout = clk_register_fixed_rate(&pdev->dev, "audio_clkout",
			__clk_get_name(priv->clk_b), 0, cfg->clkout_rate);
	of_clk_add_provider(pdev->dev.of_node, of_clk_src_simple_get,
			    priv->clkout);

	cfg->is_playback = 1;
	device_property_read_u32(&pdev->dev, "clock-frequency",
				 (u32 *)&cfg->clkout_rate);

	cfg->sample_rate = X5H_AUDIO_SAMPLE_RATE;
	cfg->channels    = X5H_AUDIO_CHANNELS;
	cfg->bit_width   = X5H_AUDIO_BIT_WIDTH;
	cfg->slot_width  = X5H_AUDIO_SLOT_WIDTH;
	cfg->clk_master  = X5H_AUDIO_CLK_MASTER;
	cfg->ssi_id      = X5H_AUDIO_SSI_ID;
	cfg->src_id      = X5H_AUDIO_SRC_ID;
	cfg->ctu_id      = X5H_AUDIO_CTU_ID;
	cfg->dvc_id      = X5H_AUDIO_DVC_ID;
	cfg->mix_id      = X5H_AUDIO_MIX_ID;
	cfg->ssiu_busif  = X5H_AUDIO_SSIU_BUSIF;

	cfg->src_id2      = X5H_AUDIO_SRC_ID2;
	cfg->ctu_id2      = X5H_AUDIO_CTU_ID2;

	cfg->src_bypass     = false;
	cfg->bit_clk_inv    = false;
	cfg->frm_clk_inv    = false;
	cfg->sys_delay      = false;
	cfg->data_alignment = false;

	cfg->scu_phys_base  = priv->scu_phys_base;
	cfg->ssiu_phys_base = priv->ssiu_phys_base;

	busif = cfg->ssiu_busif;
	cfg->dma_src_addr = priv->scu_phys_base  - 0x001F8000;
	cfg->dma_dst_addr = priv->ssiu_phys_base - 0x00140000
			    + 0x1000 * cfg->ssi_id
			    + (busif / 4) * 0x9000
			    + (busif % 4) * 0x400;

	cfg->dmapp_id_tx = 0;
	cfg->dmapp_id_rx = 0;

	priv->ssi_irq = platform_get_irq_byname(pdev, "ssi");
	if (priv->ssi_irq < 0)
		priv->ssi_irq = platform_get_irq(pdev, 0);

	priv->audio_ctx.cfg = cfg;

	return 0;
}

/* ============================================================================
 *  DMA engine management — Public API (Linux dmaengine, not peri-peri P-DMA)
 * ============================================================================
 */

static void x5h_dma_complete(void *data)
{
	struct x5h_dma_cb_wrapper *w = data;
	struct x5h_audio *ctx = w->ctx;
	int id = w->stream_id;

	if (ctx->period_cb[id])
		ctx->period_cb[id](ctx->period_cb_data[id]);
}

int x5h_audio_dma_setup(struct x5h_audio *ctx, int stream_id, 
        const char *dma_name, dma_addr_t dst_addr)
{
	struct dma_chan *chan;
	struct dma_slave_config cfg;
	dma_addr_t dma_handle;
	void *buf;
	int ret;

	if (!ctx || !ctx->dev || stream_id < 0 ||
	    stream_id >= X5H_AUDIO_MAX_STREAMS)
		return -EINVAL;

    chan = dma_request_chan(ctx->dev, dma_name);
	if (IS_ERR(chan))
		return PTR_ERR(chan);

	buf = dma_alloc_coherent(chan->device->dev,
				 X5H_AUDIO_PCM_BUFFER_SIZE,
				 &dma_handle, GFP_KERNEL);
	if (!buf) {
		dma_release_channel(chan);
		return -ENOMEM;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.direction		= DMA_MEM_TO_DEV;
	cfg.src_addr_width	= DMA_SLAVE_BUSWIDTH_4_BYTES;
	cfg.dst_addr_width	= DMA_SLAVE_BUSWIDTH_4_BYTES;
	cfg.dst_addr		= dst_addr;
	ret = dmaengine_slave_config(chan, &cfg);
	if (ret) {
		dma_free_coherent(chan->device->dev,
				  X5H_AUDIO_PCM_BUFFER_SIZE, buf, dma_handle);
		dma_release_channel(chan);
		return ret;
	}

	ctx->dma_rx[stream_id]      = chan;
	ctx->pcm_buf_virt[stream_id] = buf;
	ctx->pcm_buf_dma[stream_id]  = dma_handle;
	ctx->pcm_buf_size            = X5H_AUDIO_PCM_BUFFER_SIZE;

	ctx->dma_cb_wrapper[stream_id].ctx = ctx;
	ctx->dma_cb_wrapper[stream_id].stream_id = stream_id;

	return 0;
}
EXPORT_SYMBOL_GPL(x5h_audio_dma_setup);

int x5h_audio_dma_start(struct x5h_audio *ctx, int stream_id)
{
	struct dma_chan *chan;
	struct dma_async_tx_descriptor *desc;

	if (!ctx || stream_id < 0 || stream_id >= X5H_AUDIO_MAX_STREAMS)
		return -EINVAL;

	chan = ctx->dma_rx[stream_id];
	if (!chan || ctx->dma_running[stream_id])
		return -EINVAL;

	desc = dmaengine_prep_dma_cyclic(chan,
			ctx->pcm_buf_dma[stream_id],
			ctx->pcm_buf_size,
			X5H_AUDIO_PCM_PERIOD_SIZE,
			DMA_MEM_TO_DEV,
			DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc)
		return -EIO;

	desc->callback = x5h_dma_complete;
	desc->callback_param = &ctx->dma_cb_wrapper[stream_id];

	ctx->dma_cookie[stream_id] = dmaengine_submit(desc);
	if (dma_submit_error(ctx->dma_cookie[stream_id]))
		return -EIO;

	dma_async_issue_pending(chan);
	ctx->dma_running[stream_id] = true;

	return 0;
}
EXPORT_SYMBOL_GPL(x5h_audio_dma_start);

void x5h_audio_dma_stop(struct x5h_audio *ctx, int stream_id)
{
	struct dma_chan *chan;

	if (!ctx || stream_id < 0 || stream_id >= X5H_AUDIO_MAX_STREAMS)
		return;

	chan = ctx->dma_rx[stream_id];
	if (chan && ctx->dma_running[stream_id]) {
		dmaengine_terminate_async(chan);
		ctx->dma_running[stream_id] = false;
		wmb();
	}
}
EXPORT_SYMBOL_GPL(x5h_audio_dma_stop);

unsigned int x5h_audio_dma_pointer(struct x5h_audio *ctx, int stream_id,
				   size_t buf_size)
{
	struct dma_chan *chan;
	struct dma_tx_state state = {};
	enum dma_status status;
	unsigned int pos = 0;

	if (!ctx || stream_id < 0 || stream_id >= X5H_AUDIO_MAX_STREAMS)
		return 0;

	chan = ctx->dma_rx[stream_id];
	if (!chan || !ctx->dma_running[stream_id])
		return 0;

	status = dmaengine_tx_status(chan, ctx->dma_cookie[stream_id], &state);
	if (status == DMA_IN_PROGRESS || status == DMA_PAUSED) {
		if (state.residue > 0 && state.residue <= buf_size)
			pos = buf_size - state.residue;
	}

	return pos;
}
EXPORT_SYMBOL_GPL(x5h_audio_dma_pointer);

void x5h_audio_dma_teardown(struct x5h_audio *ctx, int stream_id)
{
	struct dma_chan *chan;

	if (!ctx || stream_id < 0 || stream_id >= X5H_AUDIO_MAX_STREAMS)
		return;

	chan = ctx->dma_rx[stream_id];
	if (!chan)
		return;

	x5h_audio_dma_stop(ctx, stream_id);

	if (ctx->pcm_buf_virt[stream_id]) {
		dma_free_coherent(chan->device->dev,
				  X5H_AUDIO_PCM_BUFFER_SIZE,
				  ctx->pcm_buf_virt[stream_id],
				  ctx->pcm_buf_dma[stream_id]);
		ctx->pcm_buf_virt[stream_id] = NULL;
		ctx->pcm_buf_dma[stream_id] = 0;
	}
	dma_release_channel(chan);
	ctx->dma_rx[stream_id] = NULL;
}
EXPORT_SYMBOL_GPL(x5h_audio_dma_teardown);

/* ============================================================================
 *  Clocks enable/disable helpers
 * ============================================================================
 */
static int x5h_hw_clocks_enable(struct x5h_hw_priv *priv)
{
	if (!IS_ERR(priv->clk_adg))
		clk_prepare_enable(priv->clk_adg);
	if (!IS_ERR(priv->clk_ssi))
		clk_prepare_enable(priv->clk_ssi);
	if (!IS_ERR(priv->clk_scu))
		clk_prepare_enable(priv->clk_scu);

	if (!IS_ERR(priv->clk_ssi_idx))
		clk_prepare_enable(priv->clk_ssi_idx);
	if (!IS_ERR(priv->clk_src_idx))
		clk_prepare_enable(priv->clk_src_idx);
	if (!IS_ERR(priv->clk_src2_idx))
		clk_prepare_enable(priv->clk_src2_idx);
	if (!IS_ERR(priv->clk_dvc_idx))
		clk_prepare_enable(priv->clk_dvc_idx);
	if (!IS_ERR(priv->clk_ctu_idx))
		clk_prepare_enable(priv->clk_ctu_idx);
	if (!IS_ERR(priv->clk_dmapp1))
		clk_prepare_enable(priv->clk_dmapp1);
	if (!IS_ERR(priv->clk_dmapp2))
		clk_prepare_enable(priv->clk_dmapp2);

	clk_prepare_enable(priv->clk_a);
	clk_prepare_enable(priv->clk_b);
	clk_prepare_enable(priv->clk_c);

	return 0;
}

static void x5h_hw_clocks_disable(struct x5h_hw_priv *priv)
{
	if (!IS_ERR(priv->clk_adg))
		clk_disable_unprepare(priv->clk_adg);
	if (!IS_ERR(priv->clk_ssi))
		clk_disable_unprepare(priv->clk_ssi);
	if (!IS_ERR(priv->clk_scu))
		clk_disable_unprepare(priv->clk_scu);
}

/* ============================================================================
 *  Probe — init, start, DMA acquire, buffer allocation all at probe time
 * ============================================================================
 */
static int x5h_audio_probe(struct platform_device *pdev)
{
	struct x5h_hw_priv *priv;
	struct x5h_audio *ctx;
	dma_addr_t src_dst[2];
	int ret, i;
    char res_name[RES_NAME_LEN];

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &pdev->dev;

	ret = x5h_parse_dt(priv, pdev);
	if (ret)
		return ret;

	ctx = &priv->audio_ctx;
	ctx->dev = &pdev->dev;

	ret = x5h_hw_clocks_enable(priv);
	if (ret)
		return ret;

	pm_runtime_enable(&pdev->dev);

	/*
	 * DMA destination = SRC input FIFO address:
	 * RDMA_SRC_I_N_GEN5(addr, i) = addr - 0x00500000 + 0x1000 * i
	 */
	src_dst[0] = priv->scu_phys_base - 0x00500000
		     + (0x1000 * priv->cfg.src_id);
	src_dst[1] = priv->scu_phys_base - 0x00500000
		     + (0x1000 * priv->cfg.src_id2);

	/* Acquire DMA channels and allocate PCM buffers for both streams */
    snprintf(res_name, RES_NAME_LEN, "src%d_rx", X5H_AUDIO_SRC_ID);
	ret = x5h_audio_dma_setup(ctx, 0, res_name, src_dst[0]);
	if (ret) {
		dev_err(&pdev->dev, "DMA setup stream 0 failed: %d\n", ret);
		goto err_clocks;
	}

    snprintf(res_name, RES_NAME_LEN, "src%d_rx", X5H_AUDIO_SRC_ID2);
	ret = x5h_audio_dma_setup(ctx, 1, res_name, src_dst[1]);
	if (ret) {
		dev_err(&pdev->dev, "DMA setup stream 1 failed: %d\n", ret);
		goto err_dma0;
	}

	/* Configure hardware pipeline */
	ret = x5h_audio_init(ctx);
	if (ret) {
		dev_err(&pdev->dev, "x5h_audio_init failed: %d\n", ret);
		goto err_teardown;
	}

	/* Start the hardware pipeline */
	ret = x5h_audio_start(ctx);
	if (ret) {
		dev_err(&pdev->dev, "x5h_audio_start failed: %d\n", ret);
		goto err_deinit;
	}

	/* Start DMA cyclic on both streams (always-running DMA) */
	for (i = 0; i < X5H_AUDIO_MAX_STREAMS; i++) {
		ret = x5h_audio_dma_start(ctx, i);
		if (ret) {
			dev_err(&pdev->dev, "DMA start stream %d failed: %d\n",
				i, ret);
			goto err_dma_stop;
		}
	}

	/* Export global instance for ALSA driver */
	x5h_audio_set_global_instance(ctx);

	dev_set_drvdata(&pdev->dev, priv);

	/* Create child platform device for the ALSA driver to bind to */
	priv->alsa_pdev = platform_device_register_data(&pdev->dev,
			"x5h-audio-alsa", PLATFORM_DEVID_AUTO, NULL, 0);
	if (IS_ERR(priv->alsa_pdev)) {
		ret = PTR_ERR(priv->alsa_pdev);
		dev_err(&pdev->dev, "failed to create ALSA child: %d\n", ret);
		goto err_dma_stop;
	}

	dev_info(&pdev->dev,
		 "X5H Audio HW driver probed (SSI%d, SRC%d/%d, buf=%zu)\n",
		 ctx->cfg->ssi_id, ctx->cfg->src_id, ctx->cfg->src_id2,
		 ctx->pcm_buf_size);

	return 0;

err_dma_stop:
	while (--i >= 0)
		x5h_audio_dma_stop(ctx, i);
	x5h_audio_stop(ctx);
err_deinit:
	x5h_audio_deinit(ctx);
err_teardown:
	x5h_audio_dma_teardown(ctx, 1);
err_dma0:
	x5h_audio_dma_teardown(ctx, 0);
err_clocks:
	pm_runtime_disable(&pdev->dev);
	x5h_hw_clocks_disable(priv);
	return ret;
}

/* ============================================================================
 *  Remove
 * ============================================================================
 */
static int x5h_audio_remove(struct platform_device *pdev)
{
	struct x5h_hw_priv *priv = dev_get_drvdata(&pdev->dev);
	struct x5h_audio *ctx = &priv->audio_ctx;
	int i;

	x5h_audio_set_global_instance(NULL);

	if (priv->alsa_pdev && !IS_ERR(priv->alsa_pdev))
		platform_device_unregister(priv->alsa_pdev);

	for (i = X5H_AUDIO_MAX_STREAMS - 1; i >= 0; i--)
		x5h_audio_dma_stop(ctx, i);

	x5h_audio_stop(ctx);
	x5h_audio_deinit(ctx);

	for (i = 0; i < X5H_AUDIO_MAX_STREAMS; i++)
		x5h_audio_dma_teardown(ctx, i);

	pm_runtime_disable(&pdev->dev);
	x5h_hw_clocks_disable(priv);

	return 0;
}

/* ============================================================================
 *  OF match table
 * ============================================================================
 */
static const struct of_device_id x5h_audio_of_match[] = {
	{ .compatible = "renesas,x5h-audio", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, x5h_audio_of_match);

static struct platform_driver x5h_hw_driver = {
	.driver = {
		.name	= DRV_NAME,
		.of_match_table = x5h_audio_of_match,
	},
	.probe	= x5h_audio_probe,
	.remove	= x5h_audio_remove,
};
module_platform_driver(x5h_hw_driver);

MODULE_DESCRIPTION("X5H Gen5 Audio Hardware Platform Driver");
MODULE_AUTHOR("Renesas Electronics Corp.");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
