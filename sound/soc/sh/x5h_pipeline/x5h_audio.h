/* SPDX-License-Identifier: GPL-2.0 */
/*
 * X5H Gen5 Audio Pipeline — Public API
 *
 * Hardware pipeline driver for R-Car X5H (Gen5).
 * The caller (Linux platform driver) provides DMA engine, IRQ,
 * and base address management.
 *
 * Copyright (C) 2025 Renesas Electronics Corp.
 */

#ifndef X5H_AUDIO_H
#define X5H_AUDIO_H

#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/device.h>

#include "x5h_audio_regs.h"

/* ============================================================================
 *  Compile-time defaults (override via Kconfig or -D flags)
 * ============================================================================
 */
#ifndef X5H_AUDIO_CLK_A_RATE
#define X5H_AUDIO_CLK_A_RATE		24576000UL
#endif
#ifndef X5H_AUDIO_CLKOUT_RATE
#define X5H_AUDIO_CLKOUT_RATE		12288000UL
#endif
#ifndef X5H_AUDIO_SAMPLE_RATE
#define X5H_AUDIO_SAMPLE_RATE		48000
#endif
#ifndef X5H_AUDIO_CHANNELS
#define X5H_AUDIO_CHANNELS		2
#endif
#ifndef X5H_AUDIO_BIT_WIDTH
#define X5H_AUDIO_BIT_WIDTH		16
#endif
#ifndef X5H_AUDIO_SLOT_WIDTH
#define X5H_AUDIO_SLOT_WIDTH		32
#endif
#ifndef X5H_AUDIO_CLK_MASTER
#define X5H_AUDIO_CLK_MASTER		1
#endif
#ifndef X5H_AUDIO_SSI_ID
#define X5H_AUDIO_SSI_ID		5
#endif
#ifndef X5H_AUDIO_SRC_ID
#define X5H_AUDIO_SRC_ID		0
#endif
#ifndef X5H_AUDIO_SRC_ID2
#define X5H_AUDIO_SRC_ID2		2
#endif
#ifndef X5H_AUDIO_CTU_ID
#define X5H_AUDIO_CTU_ID		0
#endif
#ifndef X5H_AUDIO_CTU_ID2
#define X5H_AUDIO_CTU_ID2		1
#endif
#ifndef X5H_AUDIO_DVC_ID
#define X5H_AUDIO_DVC_ID		0
#endif
#ifndef X5H_AUDIO_MIX_ID
#define X5H_AUDIO_MIX_ID		0
#endif
#ifndef X5H_AUDIO_SSIU_BUSIF
#define X5H_AUDIO_SSIU_BUSIF		0
#endif

#define X5H_AUDIO_MAX_STREAMS		3
#define X5H_AUDIO_PCM_BUFFER_SIZE	(64 * 1024)
#define X5H_AUDIO_PCM_PERIOD_SIZE	4096
#define X5H_AUDIO_PCM_PERIODS_MAX	16

/* ============================================================================
 *  Pipeline configuration (filled by caller)
 * ============================================================================
 */
struct x5h_audio_config {
	void __iomem *scu_base;
	void __iomem *adg_base;
	void __iomem *ssiu_base;
	void __iomem *dma_base;

	unsigned long clk_a_rate;
	unsigned long clk_b_rate;
	unsigned long clk_c_rate;
	unsigned long clkout_rate;

	unsigned int sample_rate;
	unsigned int channels;
	unsigned int bit_width;
	unsigned int slot_width;

	unsigned int ssi_id;
	unsigned int src_id;
	unsigned int ctu_id;
	unsigned int dvc_id;
	unsigned int mix_id;
	unsigned int ssiu_busif;
	unsigned int src_id2;
	unsigned int ctu_id2;

	bool clk_master;
	bool bit_clk_inv;
	bool frm_clk_inv;
	bool sys_delay;
	bool data_alignment;
	bool is_playback;

	bool src_bypass;

	unsigned int dmapp_id_tx;
	unsigned int dmapp_id_rx;

	dma_addr_t dma_src_addr;
	dma_addr_t dma_dst_addr;

	phys_addr_t scu_phys_base;
	phys_addr_t ssiu_phys_base;
};

/* ============================================================================
 *  Per-stream DMA callback wrapper — passed as dma_complete callback_param
 * ============================================================================
 */
struct x5h_dma_cb_wrapper {
	struct x5h_audio *ctx;
	int stream_id;
};

enum {
    X5H_STREAM_ID_PB0 = 0,
    X5H_STREAM_ID_PB1,
    X5H_STREAM_ID_CAP,
};

/* ============================================================================
 *  Runtime state (allocated by caller, owned by pipeline layer)
 * ============================================================================
 */
struct x5h_audio {
	const struct x5h_audio_config *cfg;

	u32 ssi_cr_own;
	u32 ssi_cr_clk;
	u32 ssi_cr_mode;
	u32 ssi_cr_en;
	u32 ssi_wsr;

	u32 sckdv_idx;
	unsigned int ssi_rate;
	unsigned int ssi_chan;
	unsigned int src_fin;
	unsigned int src_fout;
	u32 dvc_vol[8];
	bool dvc_mute[8];

	struct device *dev;

	/* DMA engine channels (Linux dmaengine API) */
	struct dma_chan *dma_rx[X5H_AUDIO_MAX_STREAMS];
	dma_cookie_t     dma_cookie[X5H_AUDIO_MAX_STREAMS];

	/* Pre-allocated PCM buffers (owned by this layer, exported to ALSA) */
	void         *pcm_buf_virt[X5H_AUDIO_MAX_STREAMS];
	dma_addr_t    pcm_buf_dma[X5H_AUDIO_MAX_STREAMS];
	size_t        pcm_buf_size;

	/* Per-stream period-elapsed callbacks (registered by ALSA layer) */
	void (*period_cb[X5H_AUDIO_MAX_STREAMS])(void *);
	void *period_cb_data[X5H_AUDIO_MAX_STREAMS];
	struct x5h_dma_cb_wrapper dma_cb_wrapper[X5H_AUDIO_MAX_STREAMS];

	bool pipeline_started;
	bool dma_running[X5H_AUDIO_MAX_STREAMS];
	bool initialized;
};

/* ============================================================================
 *  Public API — exported by x5h-audio-hw.ko, consumed by x5h-audio-alsa.ko
 * ============================================================================
 */

/* --- DMA buffer access (for ALSA to use as PCM buffer) --- */
void *x5h_audio_get_pcm_buffer(struct x5h_audio *ctx, int stream_id);
dma_addr_t x5h_audio_get_pcm_dma_addr(struct x5h_audio *ctx, int stream_id);
size_t x5h_audio_get_pcm_buffer_size(struct x5h_audio *ctx);

/* --- Period-elapsed callback registration --- */
void x5h_audio_set_period_cb(struct x5h_audio *ctx, int stream_id,
			     void (*cb)(void *), void *data);

/* --- DMA engine management --- */
int x5h_audio_dma_setup(struct x5h_audio *ctx, int stream_id, 
        const char *dma_name, dma_addr_t dst_addr);
void x5h_audio_dma_teardown(struct x5h_audio *ctx, int stream_id);
int x5h_audio_dma_start(struct x5h_audio *ctx, int stream_id);
void x5h_audio_dma_stop(struct x5h_audio *ctx, int stream_id);
unsigned int x5h_audio_dma_pointer(struct x5h_audio *ctx, int stream_id,
				   size_t buf_size);

/* --- Hardware pipeline control --- */
int  x5h_audio_init(struct x5h_audio *ctx);
int  x5h_audio_start(struct x5h_audio *ctx);
void x5h_audio_stop(struct x5h_audio *ctx);
void x5h_audio_deinit(struct x5h_audio *ctx);

void x5h_ssi_reinit(struct x5h_audio *ctx);
void x5h_audio_set_direction(struct x5h_audio *ctx, int is_play);
void x5h_audio_set_playback_volume(struct x5h_audio *ctx, u32 vol[8]);
void x5h_audio_set_playback_mute(struct x5h_audio *ctx, unsigned int ch,
				 bool mute);

void x5h_audio_dump_regs(const struct x5h_audio *ctx);

/* --- Global instance (set by HW platform driver) --- */
void x5h_audio_set_global_instance(struct x5h_audio *ctx);
struct x5h_audio *x5h_audio_get_global_instance(void);

#endif /* X5H_AUDIO_H */
