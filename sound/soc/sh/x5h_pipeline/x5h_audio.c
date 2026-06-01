#include <linux/delay.h>
#include <linux/io.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/slab.h>
#include "x5h_audio.h"

/* ============================================================================
 *  Internal helpers
 * ============================================================================
 */
static void x5h_writel(void __iomem *base, u32 offset, u32 val)
{
	writel(val, base + offset);
}

static u32 x5h_readl(void __iomem *base, u32 offset)
{
	return readl(base + offset);
}

static void x5h_bset(void __iomem *base, u32 offset, u32 mask, u32 val)
{
	u32 v = readl(base + offset);
	v &= ~mask;
	v |= (val & mask);
	writel(v, base + offset);
}

/* Read SSI register (lives in SSIU space on Gen5) */
static u32 x5h_ssi_readl(const struct x5h_audio *ctx, u32 reg)
{
	return x5h_readl(ctx->cfg->ssiu_base,
			 x5h_ssiu_per_ssi_base(ctx->cfg->ssi_id) + reg);
}

/* Write SSI register */
static void x5h_ssi_writel(const struct x5h_audio *ctx, u32 reg, u32 val)
{
	x5h_writel(ctx->cfg->ssiu_base,
		   x5h_ssiu_per_ssi_base(ctx->cfg->ssi_id) + reg, val);
}

/* Write SSIU per-SSI register */
static void x5h_ssiu_writel(const struct x5h_audio *ctx, u32 reg, u32 val)
{
	x5h_writel(ctx->cfg->ssiu_base,
		   x5h_ssiu_per_ssi_base(ctx->cfg->ssi_id) + reg, val);
}

/* Write SCU register */
static void x5h_scu_writel(const struct x5h_audio *ctx, u32 reg, u32 val)
{
	x5h_writel(ctx->cfg->scu_base, reg, val);
}

#if 0
static u32 x5h_scu_readl(const struct x5h_audio *ctx, u32 reg)
{
	return x5h_readl(ctx->cfg->scu_base, reg);
}

static u32 x5h_adg_readl(const struct x5h_audio *ctx, u32 reg)
{
	return x5h_readl(ctx->cfg->adg_base, reg);
}
#endif

/* Write ADG register */
static void x5h_adg_writel(const struct x5h_audio *ctx, u32 reg, u32 val)
{
	x5h_writel(ctx->cfg->adg_base, reg, val);
}

static void x5h_adg_bset(const struct x5h_audio *ctx, u32 reg, u32 mask, u32 val)
{
	x5h_bset(ctx->cfg->adg_base, reg, mask, val);
}

/* Write DMA peri-peri register */
static void x5h_dma_writel(const struct x5h_audio *ctx, u32 dmapp_id, u32 reg, u32 val)
{
	x5h_writel(ctx->cfg->dma_base,
		   x5h_dma_per_ch(dmapp_id) + reg, val);
}

static u32 x5h_dma_readl(const struct x5h_audio *ctx, u32 dmapp_id, u32 reg)
{
	return x5h_readl(ctx->cfg->dma_base,
			 x5h_dma_per_ch(dmapp_id) + reg);
}

static void x5h_dma_bset(const struct x5h_audio *ctx, u32 dmapp_id, u32 reg,
			 u32 mask, u32 val)
{
	x5h_bset(ctx->cfg->dma_base,
		 x5h_dma_per_ch(dmapp_id) + reg, mask, val);
}

/* ============================================================================
 *  ADG — Audio Clock Generator
 * ============================================================================
 */

/* Gen5 SSIU_AUDIO_CLK_SEL source values (from rcar rsnd_adg_clk_query) */
static const u8 x5h_adg_clk_to_ssiu_sel[X5H_CLK_MAX] = {
	[X5H_CLK_CLKA] = 0x01,
	[X5H_CLK_CLKB] = 0x02,
	[X5H_CLK_CLKC] = 0x03,
	[X5H_CLK_RBGA] = 0x10,
	[X5H_CLK_RBGB] = 0x20,
};

static u32 x5h_adg_calc_rbgx(unsigned long div)
{
	int i;

	if (!div)
		return 0;

	for (i = 3; i >= 0; i--) {
		int ratio = 2 << (i * 2);
		if ((div % ratio) == 0)
			return (u32)((i << 8) | ((div / ratio) - 1));
	}
	return ~0;
}

static void x5h_adg_get_clk_sel(const struct x5h_audio *ctx,
				unsigned int target_rate,
				unsigned int *target_val,
				unsigned int *target_en)
{
	const struct x5h_audio_config *cfg = ctx->cfg;
	unsigned long sel_rate[] = {
		[X5H_CLK_CLKA] = cfg->clk_a_rate,
		[X5H_CLK_CLKB] = cfg->clk_b_rate,
		[X5H_CLK_CLKC] = cfg->clk_c_rate,
		[X5H_CLK_RBGA] = 0,
		[X5H_CLK_RBGB] = cfg->clkout_rate,
	};
	unsigned int min = ~0;
	unsigned int val = 0;
	unsigned int en = 0;
	int idx, sel, div;
	int step;

	for (sel = 0; sel < ARRAY_SIZE(sel_rate); sel++) {
		if (!sel_rate[sel])
			continue;

		idx = 0;
		step = 2;

		for (div = 2; div <= 98304; div += step) {
			unsigned long diff;
			if (sel_rate[sel] / div == 0)
				continue;
			diff = (target_rate > sel_rate[sel] / div) ?
				target_rate - sel_rate[sel] / div :
				sel_rate[sel] / div - target_rate;
			if (min > diff) {
				val = (sel << 8) | idx;
				min = diff;
				en = 1 << (sel + 1);
			}
			if ((idx > 2) && (idx % 2))
				step *= 2;
			if (idx == 0x1c) {
				div += step;
				step *= 2;
			}
			idx++;
		}
	}

	*target_val = val;
	if (target_en)
		*target_en = en;
}

static unsigned int x5h_ssi_clk_query(struct x5h_audio *ctx,
				      unsigned int rate,
				      unsigned int chan,
				      unsigned int *ckdv_idx)
{
	const struct x5h_audio_config *cfg = ctx->cfg;
	unsigned int main_rate;
	int j;

	for (j = 1; j < X5H_SSI_CKDV_NUM; j++) {
		main_rate = cfg->slot_width * rate * chan * x5h_ssi_clk_mul[j];
		if (cfg->clkout_rate && cfg->clkout_rate == main_rate) {
			if (ckdv_idx)
				*ckdv_idx = j;
			return main_rate;
		}
	}
	for (j = 1; j < X5H_SSI_CKDV_NUM; j++) {
		main_rate = cfg->slot_width * rate * chan * x5h_ssi_clk_mul[j];
		if (cfg->clk_a_rate && cfg->clk_a_rate == main_rate) {
			if (ckdv_idx)
				*ckdv_idx = j;
			return main_rate;
		}
	}
	return 0;
}

static int x5h_adg_init(struct x5h_audio *ctx)
{
	const struct x5h_audio_config *cfg = ctx->cfg;
	unsigned long clkout = cfg->clkout_rate;
	u32 rbga, rbgb;
	unsigned int aud_clk_sel_en;
	unsigned long rate;

	rate = x5h_ssi_clk_query(ctx, cfg->sample_rate, cfg->channels, NULL);
	if (rate == cfg->clkout_rate) {
		x5h_ssiu_writel(ctx, X5H_SSIU_AUDIO_CLK_SEL,
				x5h_adg_clk_to_ssiu_sel[X5H_CLK_RBGB]);
	} else if (rate == cfg->clk_a_rate) {
		x5h_ssiu_writel(ctx, X5H_SSIU_AUDIO_CLK_SEL, 0x1);
	} else if (rate == cfg->clk_b_rate) {
		x5h_ssiu_writel(ctx, X5H_SSIU_AUDIO_CLK_SEL, 0x2);
	} else if (rate == cfg->clk_c_rate) {
		x5h_ssiu_writel(ctx, X5H_SSIU_AUDIO_CLK_SEL, 0x3);
	}

	x5h_adg_bset(ctx, X5H_ADG_BRGCKR, 0x80770000, 0x80000000);

	rbga = 2; rbgb = 2;
	if (cfg->sample_rate == 48000)
		rbgb = x5h_adg_calc_rbgx(cfg->clk_b_rate / clkout);
	else
		rbga = x5h_adg_calc_rbgx(cfg->clk_a_rate / clkout);
	x5h_adg_writel(ctx, X5H_ADG_BRRA, rbga & ADG_BRRA_MASK);
	x5h_adg_writel(ctx, X5H_ADG_BRRB, rbgb & ADG_BRRB_MASK);

	aud_clk_sel_en = 0;
	if (aud_clk_sel_en)
		x5h_adg_bset(ctx, X5H_ADG_DIV_EN, aud_clk_sel_en, aud_clk_sel_en);

	return 0;
}

/* ============================================================================
 *  SSI — Serial Sound Interface
 * ============================================================================
 */
static u32 x5h_ssi_swl(unsigned int slot_width)
{
	switch (slot_width) {
	case 32: return SSICR_SWL_32;
	case 24: return SSICR_SWL_24;
	case 16:
	default:  return SSICR_SWL_16;
	}
}

static u32 x5h_ssi_dwl(unsigned int bit_width)
{
	switch (bit_width) {
	case 8:  return SSICR_DWL_8;
	case 16: return SSICR_DWL_16;
	case 18: return SSICR_DWL_18;
	case 20: return SSICR_DWL_20;
	case 22: return SSICR_DWL_22;
	case 24: return SSICR_DWL_24;
	case 32: return SSICR_DWL_32;
	default: return SSICR_DWL_16;
	}
}

static int x5h_ssi_master_clk_start(struct x5h_audio *ctx)
{
	const struct x5h_audio_config *cfg = ctx->cfg;
	unsigned int main_rate;
	unsigned int ckdv_idx;

	if (!cfg->clk_master)
		return 0;

	main_rate = x5h_ssi_clk_query(ctx, cfg->sample_rate,
				       cfg->channels, &ckdv_idx);
	if (!main_rate)
		return -EIO;

	ctx->sckdv_idx = ckdv_idx;
	ctx->ssi_rate  = cfg->sample_rate;
	ctx->ssi_chan  = cfg->channels;

	ctx->ssi_cr_clk = SSICR_FORCE | x5h_ssi_swl(cfg->slot_width) |
			  SSICR_SCKD | SSICR_SWSD |
			  SSICR_CKDV(ckdv_idx);
	ctx->ssi_wsr = SSIWSR_CONT;

	return 0;
}

static void x5h_ssi_config_init(struct x5h_audio *ctx)
{
	const struct x5h_audio_config *cfg = ctx->cfg;
	u32 cr_own;
	u32 cr_mode;

	cr_own = SSICR_FORCE | x5h_ssi_swl(cfg->slot_width);

	if (cfg->bit_clk_inv)
		cr_own |= SSICR_SCKP;
	if (cfg->frm_clk_inv)
		cr_own |= SSICR_SWSP;
	if (cfg->data_alignment)
		cr_own |= SSICR_SDTA;
	if (cfg->sys_delay)
		cr_own |= SSICR_DEL;

	if (cfg->is_playback)
		cr_own |= SSICR_TRMD;

	cr_own |= x5h_ssi_dwl(cfg->bit_width);

	cr_mode = SSICR_UIEN | SSICR_OIEN | SSICR_DMEN;

	ctx->ssi_cr_own  = cr_own;
	ctx->ssi_cr_mode = cr_mode;
}

static void x5h_ssi_register_setup(struct x5h_audio *ctx)
{
	x5h_bset(ctx->cfg->ssiu_base,
		 x5h_ssiu_per_ssi_base(ctx->cfg->ssi_id) + X5H_SSI_SSICR,
		 0xC000,
		 ctx->ssi_cr_own | ctx->ssi_cr_clk |
		 ctx->ssi_cr_mode | ctx->ssi_cr_en);

	x5h_ssi_writel(ctx, X5H_SSI_SSIWSR, ctx->ssi_wsr);
	x5h_ssi_writel(ctx, X5H_SSI_SSICR,
		       ctx->ssi_cr_own | ctx->ssi_cr_clk |
		       ctx->ssi_cr_mode | ctx->ssi_cr_en);
}

static void x5h_ssi_status_clear(const struct x5h_audio *ctx)
{
	x5h_ssi_writel(ctx, X5H_SSI_SSISR, 0);
}

static void x5h_ssi_status_wait(const struct x5h_audio *ctx, u32 bit)
{
	int i;
	for (i = 0; i < 1024; i++) {
		if (x5h_ssi_readl(ctx, X5H_SSI_SSISR) & bit)
			return;
		udelay(5);
	}
}

static int x5h_ssi_init(struct x5h_audio *ctx)
{
	int ret;

	ret = x5h_ssi_master_clk_start(ctx);
	if (ret < 0)
		return ret;

	x5h_ssi_config_init(ctx);
	x5h_ssi_register_setup(ctx);
	x5h_ssi_status_clear(ctx);

	return 0;
}

static void x5h_ssi_start(struct x5h_audio *ctx)
{
	ctx->ssi_cr_en = SSICR_EN;
	x5h_ssi_writel(ctx, X5H_SSI_SSICR,
		       ctx->ssi_cr_own | ctx->ssi_cr_clk |
		       ctx->ssi_cr_mode | ctx->ssi_cr_en);
}

static void x5h_ssi_stop(struct x5h_audio *ctx)
{
	x5h_ssi_writel(ctx, X5H_SSI_SSICR,
		       ctx->ssi_cr_own | ctx->ssi_cr_clk |
		       ctx->ssi_cr_mode | SSICR_EN);
	x5h_ssi_status_wait(ctx, SSISR_DIRQ);

	ctx->ssi_cr_en = 0;
	x5h_ssi_writel(ctx, X5H_SSI_SSICR,
		       ctx->ssi_cr_own | ctx->ssi_cr_clk | ctx->ssi_cr_mode);

	x5h_ssi_status_wait(ctx, SSISR_IIRQ);
}

static void x5h_ssi_quit(struct x5h_audio *ctx)
{
	ctx->ssi_cr_clk = 0;
	ctx->ssi_rate   = 0;
	ctx->ssi_chan   = 0;
	ctx->ssi_cr_own = 0;
	ctx->ssi_cr_mode = 0;
	ctx->ssi_wsr    = 0;
}

/* ============================================================================
 *  SSIU — SSI Unit (BUSIF configuration)
 * ============================================================================
 */
static void x5h_ssiu_busif_err_irq_enable(const struct x5h_audio *ctx)
{
	unsigned int ssi_id = ctx->cfg->ssi_id;
	u32 val;

	switch (ssi_id) {
	case 0 ... 4:
	case 9:
		val = 0xff00ff;
		break;
	case 5 ... 8:
		val = 0x10001;
		break;
	default:
		return;
	}
	x5h_bset(ctx->cfg->ssiu_base,
		 x5h_ssiu_per_ssi_base(ssi_id) + X5H_SSI_INT_ENABLE,
		 val, val);
}

static void x5h_ssiu_busif_err_irq_disable(const struct x5h_audio *ctx)
{
	unsigned int ssi_id = ctx->cfg->ssi_id;
	u32 val;

	switch (ssi_id) {
	case 0 ... 4:
	case 9:
		val = 0xff00ff;
		break;
	case 5 ... 8:
		val = 0x10001;
		break;
	default:
		return;
	}
	x5h_bset(ctx->cfg->ssiu_base,
		 x5h_ssiu_per_ssi_base(ssi_id) + X5H_SSI_INT_ENABLE,
		 val, 0);
}

static void x5h_ssiu_busif_err_status_clear(const struct x5h_audio *ctx)
{
	unsigned int ssi_id = ctx->cfg->ssi_id;
	u32 val;

	switch (ssi_id) {
	case 0 ... 4:
	case 9:
		val = 0xff00ff;
		break;
	case 5 ... 8:
		val = 0x10001;
		break;
	default:
		return;
	}
	x5h_bset(ctx->cfg->ssiu_base,
		 x5h_ssiu_per_ssi_base(ssi_id) + X5H_SSI_STATUS,
		 val, val);
}

static u32 x5h_get_adinr_bit(void)
{
	return X5H_ADINR_BIT;
}

static u32 x5h_get_busif_shift(void)
{
	return 0;
}

static u32 x5h_get_dalign(void)
{
	return 0x76543210;
}

static void x5h_ssiu_init(struct x5h_audio *ctx)
{
	const struct x5h_audio_config *cfg = ctx->cfg;
	unsigned int busif = cfg->ssiu_busif;
	u32 mode_reg, adinr_reg, dalign_reg;

	x5h_ssiu_busif_err_status_clear(ctx);

	x5h_ssiu_writel(ctx, X5H_SSI_MODE4, 0);

	mode_reg  = X5H_SSI_BUSIF_MODE(busif);
	adinr_reg = X5H_SSI_BUSIF_ADINR(busif);
	dalign_reg = X5H_SSI_BUSIF_DALIGN(busif);

	x5h_ssiu_writel(ctx, adinr_reg,
			x5h_get_adinr_bit() | cfg->channels);
	x5h_ssiu_writel(ctx, mode_reg,
			x5h_get_busif_shift() | 1);
	x5h_ssiu_writel(ctx, dalign_reg,
			x5h_get_dalign());

	x5h_ssiu_busif_err_irq_enable(ctx);
}

static void x5h_ssiu_start(struct x5h_audio *ctx)
{
	unsigned int busif = ctx->cfg->ssiu_busif;
	x5h_ssiu_writel(ctx, X5H_SSI_CTRL(busif), 1);
}

static void x5h_ssiu_stop(struct x5h_audio *ctx)
{
	unsigned int busif = ctx->cfg->ssiu_busif;
	x5h_ssiu_writel(ctx, X5H_SSI_CTRL(busif), 0);
}

static void x5h_ssiu_quit(struct x5h_audio *ctx)
{
	x5h_ssiu_busif_err_irq_disable(ctx);
}

/* ============================================================================
 *  SRC — Sampling Rate Converter
 * ============================================================================
 */
static void x5h_src_activation(const struct x5h_audio *ctx, int dai_id)
{
	u32 base;

	if (dai_id == 0)
		base = x5h_src_base(ctx->cfg->src_id);
	else
		base = x5h_src_base(ctx->cfg->src_id2);

	x5h_scu_writel(ctx, base + X5H_SRC_SWRSR, 0);
	x5h_scu_writel(ctx, base + X5H_SRC_SWRSR, 1);
}

static void x5h_src_halt(const struct x5h_audio *ctx, int dai_id)
{
	u32 base;

	if (dai_id == 0)
		base = x5h_src_base(ctx->cfg->src_id);
	else
		base = x5h_src_base(ctx->cfg->src_id2);

	x5h_scu_writel(ctx, base + X5H_SRC_SRCIR, 1);
	x5h_scu_writel(ctx, base + X5H_SRC_SWRSR, 0);
}

static void x5h_src_init_convert_rate(struct x5h_audio *ctx, int dai_id)
{
	const struct x5h_audio_config *cfg = ctx->cfg;
	unsigned int src_id;
	u32 src_base;
	unsigned int fin  = cfg->sample_rate;
	unsigned int fout = cfg->sample_rate;
	unsigned int chan = cfg->channels;
	int use_src;
	u32 ifscr, adinr;
	u32 cr, route;
	u32 i_busif, o_busif;
	const u32 *bsdsr_table;
	const u32 *chptn;
	int idx;

	if (dai_id == 0)
		src_id = cfg->src_id;
	else
		src_id = cfg->src_id2;

	src_base = x5h_src_base(src_id);
	use_src = (fin != fout) && !cfg->src_bypass;

	adinr = x5h_get_adinr_bit() | chan;

	ifscr = 0;
	cr    = 0x00011110;
	route = 0x0;

	if (use_src) {
		route = 0x1;
		ifscr = 0x1;
	}

	switch (src_id) {
	case 0:
		chptn      = x5h_chan288888;
		bsdsr_table = x5h_bsdsr_pat1;
		break;
	case 1:
	case 3:
	case 4:
		chptn      = x5h_chan244888;
		bsdsr_table = x5h_bsdsr_pat1;
		break;
	case 2:
	case 9:
		chptn      = x5h_chan222222;
		bsdsr_table = x5h_bsdsr_pat1;
		break;
	case 5 ... 8:
		chptn      = x5h_chan222222;
		bsdsr_table = x5h_bsdsr_pat2;
		break;
	default:
		return;
	}

	for (idx = 0; idx < 6; idx++)
		if (chptn[idx] & (1u << chan))
			break;

	if (chan > 8 || idx >= 6)
		return;

	i_busif = (cfg->is_playback ? x5h_get_busif_shift() : 0) | 1;
	o_busif = (!cfg->is_playback ? x5h_get_busif_shift() : 0) | 1;

	x5h_scu_writel(ctx, src_base + X5H_SRC_ROUTE_MODE0, route);

	x5h_scu_writel(ctx, src_base + X5H_SRC_DVC_DVUCR2, 0x1);
	x5h_scu_writel(ctx, src_base + X5H_SRC_DVC_DVUIR, 1);
	x5h_scu_writel(ctx, src_base + X5H_SRC_DVC_DVUIR, 0);

	x5h_scu_writel(ctx, src_base + X5H_SRC_SRCIR, 1);
	x5h_scu_writel(ctx, src_base + X5H_SRC_ADINR, adinr);
	x5h_scu_writel(ctx, src_base + X5H_SRC_IFSCR, ifscr);
	x5h_scu_writel(ctx, src_base + X5H_SRC_SRCCR, cr);
	x5h_scu_writel(ctx, src_base + X5H_SRC_BSDSR, bsdsr_table[idx]);
	x5h_scu_writel(ctx, src_base + X5H_SRC_BSISR, x5h_bsisr[idx]);
	x5h_scu_writel(ctx, src_base + X5H_SRC_SRCIR, 0);

	x5h_scu_writel(ctx, src_base + X5H_SRC_I_BUSIF_MODE, i_busif);
	x5h_scu_writel(ctx, src_base + X5H_SRC_O_BUSIF_MODE, o_busif);
	x5h_scu_writel(ctx, src_base + X5H_SRC_BUSIF_DALIGN, x5h_get_dalign());

	{
		unsigned int out_val = (0x6 + cfg->ssi_id) << 8;
		if (fin != fout)
			x5h_adg_get_clk_sel(ctx, fout, &out_val, NULL);

		x5h_scu_writel(ctx, src_base + X5H_SRC_IN_TIMSEL, out_val);
		x5h_scu_writel(ctx, src_base + X5H_SRC_OUT_TIMSEL, out_val);
	}

	ctx->src_fin  = fin;
	ctx->src_fout = fout;
}

static void x5h_src_status_clear(const struct x5h_audio *ctx, int dai_id)
{
	u32 base;
	u32 val = 0x10001;

	if (dai_id == 0)
		base = x5h_src_base(ctx->cfg->src_id);
	else
		base = x5h_src_base(ctx->cfg->src_id2);

	x5h_scu_writel(ctx, base + X5H_SRC_STATUS0, val);
	x5h_scu_writel(ctx, base + X5H_SRC_STATUS1, val);
}

static int x5h_src_init(struct x5h_audio *ctx, int dai_id)
{
	x5h_src_activation(ctx, dai_id);
	x5h_src_init_convert_rate(ctx, dai_id);
	x5h_src_status_clear(ctx, dai_id);
	return 0;
}

static void x5h_src_start(const struct x5h_audio *ctx, int dai_id)
{
	u32 base;

	if (dai_id == 0)
		base = x5h_src_base(ctx->cfg->src_id);
	else
		base = x5h_src_base(ctx->cfg->src_id2);

	x5h_scu_writel(ctx, base + X5H_SRC_CTRL, 0x01);
}

static void x5h_src_stop(const struct x5h_audio *ctx, int dai_id)
{
	u32 base;

	if (dai_id == 0)
		base = x5h_src_base(ctx->cfg->src_id);
	else
		base = x5h_src_base(ctx->cfg->src_id2);

	x5h_scu_writel(ctx, base + X5H_SRC_CTRL, 0);
}

static void x5h_src_quit(struct x5h_audio *ctx, int dai_id)
{
	x5h_src_halt(ctx, dai_id);
	ctx->src_fin  = 0;
	ctx->src_fout = 0;
}

/* ============================================================================
 *  CTU — Channel Count Conversion Unit
 * ============================================================================
 */
static void x5h_ctu_activation(const struct x5h_audio *ctx, int dai_id)
{
	u32 base;
	if (dai_id == 0)
		base = x5h_ctu_base(ctx->cfg->ctu_id);
	else
		base = x5h_ctu_base(ctx->cfg->ctu_id2);

	x5h_scu_writel(ctx, base + X5H_CTU_SWRSR, 0);
	x5h_scu_writel(ctx, base + X5H_CTU_SWRSR, 1);
}

static void x5h_ctu_halt(const struct x5h_audio *ctx, int dai_id)
{
	u32 base;
	if (dai_id == 0)
		base = x5h_ctu_base(ctx->cfg->ctu_id);
	else
		base = x5h_ctu_base(ctx->cfg->ctu_id2);

	x5h_scu_writel(ctx, base + X5H_CTU_CTUIR, 1);
	x5h_scu_writel(ctx, base + X5H_CTU_SWRSR, 0);
}

static void x5h_ctu_value_init(const struct x5h_audio *ctx, int dai_id)
{
	const struct x5h_audio_config *cfg = ctx->cfg;
	u32 base;
	unsigned int row, col;

	if (dai_id == 0)
		base = x5h_ctu_base(cfg->ctu_id);
	else
		base = x5h_ctu_base(cfg->ctu_id2);

	x5h_scu_writel(ctx, base + X5H_CTU_CTUIR, 1);

	x5h_scu_writel(ctx, base + X5H_CTU_ADINR, 0 | cfg->channels);

	x5h_scu_writel(ctx, base + X5H_CTU_CPMDR, 0x0);

	x5h_scu_writel(ctx, base + X5H_CTU_SCMDR, 0);

	for (row = 0; row < 4; row++) {
		for (col = 0; col < 8; col++) {
			u32 val = (row == col) ? 0x007FFFFF : 0;
			x5h_scu_writel(ctx,
				base + X5H_CTU_SV00R + (row * 8 + col) * 4, val);
		}
	}

	x5h_scu_writel(ctx, base + X5H_CTU_CTUIR, 0);
}

static int x5h_ctu_init(struct x5h_audio *ctx, int dai_id)
{
	x5h_ctu_activation(ctx, dai_id);
	x5h_ctu_value_init(ctx, dai_id);
	return 0;
}

static void x5h_ctu_quit(struct x5h_audio *ctx, int dai_id)
{
	x5h_ctu_halt(ctx, dai_id);
}

/* ============================================================================
 *  DVC — Digital Volume Controller
 * ============================================================================
 */
static void x5h_dvc_activation(const struct x5h_audio *ctx)
{
	u32 base = x5h_dvc_base(ctx->cfg->dvc_id);

	x5h_scu_writel(ctx, base + X5H_DVC_SWRSR, 0);
	x5h_scu_writel(ctx, base + X5H_DVC_SWRSR, 1);
}

static void x5h_dvc_halt(const struct x5h_audio *ctx)
{
	u32 base = x5h_dvc_base(ctx->cfg->dvc_id);

	x5h_scu_writel(ctx, base + X5H_DVC_DVUIR, 1);
	x5h_scu_writel(ctx, base + X5H_DVC_SWRSR, 0);
}

static void x5h_dvc_volume_init(struct x5h_audio *ctx)
{
	const struct x5h_audio_config *cfg = ctx->cfg;
	u32 base = x5h_dvc_base(cfg->dvc_id);
	int i;

	x5h_scu_writel(ctx, base + X5H_DVC_DVUIR, 1);

	x5h_scu_writel(ctx, base + X5H_DVC_ADINR,
		       x5h_get_adinr_bit() | cfg->channels);

	x5h_scu_writel(ctx, base + X5H_DVC_DVUCR, 0x101);

	for (i = 0; i < 8; i++) {
		ctx->dvc_vol[i] = 0x2CCCCC;
		ctx->dvc_mute[i] = false;
	}

	x5h_scu_writel(ctx, base + X5H_DVC_VOL0R, ctx->dvc_vol[0]);
	x5h_scu_writel(ctx, base + X5H_DVC_VOL1R, ctx->dvc_vol[1]);
	x5h_scu_writel(ctx, base + X5H_DVC_VOL2R, ctx->dvc_vol[2]);
	x5h_scu_writel(ctx, base + X5H_DVC_VOL3R, ctx->dvc_vol[3]);
	x5h_scu_writel(ctx, base + X5H_DVC_VOL4R, ctx->dvc_vol[4]);
	x5h_scu_writel(ctx, base + X5H_DVC_VOL5R, ctx->dvc_vol[5]);
	x5h_scu_writel(ctx, base + X5H_DVC_VOL6R, ctx->dvc_vol[6]);
	x5h_scu_writel(ctx, base + X5H_DVC_VOL7R, ctx->dvc_vol[7]);

	x5h_scu_writel(ctx, base + X5H_DVC_DVUIR, 0);
}

static void x5h_dvc_volume_update(struct x5h_audio *ctx)
{
	u32 base = x5h_dvc_base(ctx->cfg->dvc_id);
	u32 zmask = 0;
	int i;

	x5h_scu_writel(ctx, base + X5H_DVC_DVUER, 0);

	for (i = 0; i < 8; i++)
		if (ctx->dvc_mute[i])
			zmask |= (1u << i);
	x5h_scu_writel(ctx, base + X5H_DVC_ZCMCR, zmask);

	x5h_scu_writel(ctx, base + X5H_DVC_VRPDR, 0x0101);
	x5h_scu_writel(ctx, base + X5H_DVC_VRDBR, 0);

	x5h_scu_writel(ctx, base + X5H_DVC_DVUER, 1);
}

static int x5h_dvc_init(struct x5h_audio *ctx)
{
	x5h_dvc_activation(ctx);
	x5h_dvc_volume_init(ctx);
	x5h_dvc_volume_update(ctx);
	return 0;
}

static void x5h_dvc_quit(struct x5h_audio *ctx)
{
	x5h_dvc_halt(ctx);
}

/* ============================================================================
 *  CMD — Command Interface
 * ============================================================================
 */

#define X5H_CMD_SYNCO_SEL 0
#define X5H_CMD_OUT_SEL 0
#define X5H_CMD_SYNCO_SRC 0

#define X5H_CMD_IN_AUD_DATA_SEL(data_idx, src_id) \
	(src_id << (data_idx * 4))

#define X5H_CMD_OUT_TIMING_SRC (X5H_CMD_SYNCO_SRC << 24)
#define X5H_CMD_OUT_TIMING (X5H_CMD_SYNCO_SEL << 28)
#define X5H_CMD_OUT_MUX (X5H_CMD_OUT_SEL << 16)

static int x5h_cmd_init(struct x5h_audio *ctx)
{
	const struct x5h_audio_config *cfg = ctx->cfg;
	u32 base = x5h_cmd_base(0);
	u32 route_slct;
	unsigned int out_val;

	route_slct =
		X5H_CMD_IN_AUD_DATA_SEL(0, cfg->src_id) |
		X5H_CMD_IN_AUD_DATA_SEL(1, cfg->src_id2)|
		X5H_CMD_IN_AUD_DATA_SEL(2, cfg->src_id) |
		X5H_CMD_IN_AUD_DATA_SEL(3, cfg->src_id2)|
		X5H_CMD_OUT_MUX | X5H_CMD_OUT_TIMING_SRC | X5H_CMD_OUT_TIMING;

	x5h_scu_writel(ctx, base + X5H_CMD_ROUTE_SLCT, route_slct);

	x5h_scu_writel(ctx, base + X5H_CMD_BUSIF_MODE,
			x5h_get_busif_shift() | 1);
	x5h_scu_writel(ctx, base + X5H_CMD_BUSIF_DALIGN,
			x5h_get_dalign());

	out_val = (0x6 + cfg->ssi_id) << 8;
	x5h_scu_writel(ctx, base + X5H_CMDOUT_TIMSEL, out_val);

	return 0;
}

static void x5h_cmd_start(const struct x5h_audio *ctx)
{
	u32 base = x5h_cmd_base(0);
	x5h_scu_writel(ctx, base + X5H_CMD_CTRL, CMD_CTRL_START);
}

static void x5h_cmd_stop(const struct x5h_audio *ctx)
{
	u32 base = x5h_cmd_base(0);
	x5h_scu_writel(ctx, base + X5H_CMD_CTRL, 0);
}

/* ============================================================================
 *  DMA — peri-peri (AUDMAPP) configuration
 * ============================================================================
 */

/* ============================================================================
 *  SSIU peri-peri DMA module ID lookup from Table 128.8
 * ============================================================================
 */
static u8 x5h_ssiu_dmapp_id(unsigned int ssi_id, unsigned int busif)
{
	static const u8 tbl[] = {
		/* SSI00~07 */ 0x00, 0x01, 0x02, 0x03, 0x39, 0x3a, 0x3b, 0x3c,
		/* SSI10~17 */ 0x04, 0x05, 0x06, 0x07, 0x3d, 0x3e, 0x3f, 0x40,
		/* SSI20~27 */ 0x08, 0x09, 0x0a, 0x0b, 0x41, 0x42, 0x43, 0x44,
		/* SSI30~37 */ 0x0c, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b,
		/* SSI40~47 */ 0x0d, 0x4c, 0x4d, 0x4e, 0x4f, 0x50, 0x51, 0x52,
		/* SSI5      */ 0x0e, 0, 0, 0, 0, 0, 0, 0,
		/* SSI6      */ 0x0f, 0, 0, 0, 0, 0, 0, 0,
		/* SSI7      */ 0x10, 0, 0, 0, 0, 0, 0, 0,
		/* SSI8      */ 0x11, 0, 0, 0, 0, 0, 0, 0,
		/* SSI90~97 */ 0x12, 0x13, 0x14, 0x15, 0x53, 0x54, 0x55, 0x56,
	};
	unsigned int idx = ssi_id * 8 + busif;

	return idx < ARRAY_SIZE(tbl) ? tbl[idx] : 0;

}
static void x5h_dma_dmapp_start(struct x5h_audio *ctx)
{
	const struct x5h_audio_config *cfg = ctx->cfg;
	unsigned int dma_chcr;
	u32 id = cfg->dmapp_id_tx;

	if (!cfg->dma_base || !cfg->dma_src_addr || !cfg->dma_dst_addr)
		return;

	dma_chcr = (0x37u << 24)
			| ((u32)x5h_ssiu_dmapp_id(cfg->ssi_id, cfg->ssiu_busif) << 16);

	x5h_dma_writel(ctx, id, X5H_DMA_PDMASAR, (u32)cfg->dma_src_addr);
	x5h_dma_writel(ctx, id, X5H_DMA_PDMADAR, (u32)cfg->dma_dst_addr);
	x5h_dma_writel(ctx, id, X5H_DMA_PDMACHCR,
			dma_chcr | X5H_DMA_PDMACHCR_DE);
}

static void x5h_dma_dmapp_stop(struct x5h_audio *ctx)
{
	const struct x5h_audio_config *cfg = ctx->cfg;
	u32 id = cfg->dmapp_id_tx;
	int i;

	if (!cfg->dma_base)
		return;

	x5h_dma_bset(ctx, id, X5H_DMA_PDMACHCR,
		     X5H_DMA_PDMACHCR_DE, 0);

	for (i = 0; i < 1024; i++) {
		if (!(x5h_dma_readl(ctx, id, X5H_DMA_PDMACHCR) &
		      X5H_DMA_PDMACHCR_DE))
			return;
		udelay(1);
	}
}

/* ============================================================================
 * MIX - Mixer
 * ============================================================================
 */
#define X5H_MIX_MIXMR_STEP 0
#define X5H_MIX_MIXMR_RAMP 1
static void x5h_mix_volume_init(struct x5h_audio *ctx)
{
	u32 base = x5h_mix_base(ctx->cfg->mix_id);

	x5h_scu_writel(ctx, base + X5H_MIX_MIXIR, 1);

	x5h_scu_writel(ctx, base + X5H_MIX_ADINR, ctx->cfg->channels);

	x5h_scu_writel(ctx, base + X5H_MIX_MIXMR, X5H_MIX_MIXMR_STEP);
	x5h_scu_writel(ctx, base + X5H_MIX_MVPDR, 0);

	x5h_scu_writel(ctx, base + X5H_MIX_MDBAR, 0);
	x5h_scu_writel(ctx, base + X5H_MIX_MDBBR, 0);
	x5h_scu_writel(ctx, base + X5H_MIX_MDBCR, 0);
	x5h_scu_writel(ctx, base + X5H_MIX_MDBDR, 0);

	x5h_scu_writel(ctx, base + X5H_MIX_MIXIR, 0);
}

static void x5h_mix_volume_update(struct x5h_audio *ctx,
		u32 a, u32 b, u32 c, u32 d)
{
	u32 base = x5h_mix_base(ctx->cfg->mix_id);

	x5h_scu_writel(ctx, base + X5H_MIX_MDBER, 0);

	x5h_scu_writel(ctx, base + X5H_MIX_MDBAR, a);
	x5h_scu_writel(ctx, base + X5H_MIX_MDBBR, b);
	x5h_scu_writel(ctx, base + X5H_MIX_MDBCR, c);
	x5h_scu_writel(ctx, base + X5H_MIX_MDBDR, d);

	x5h_scu_writel(ctx, base + X5H_MIX_MDBER, 1);
}

static void x5h_mix_activation(struct x5h_audio *ctx)
{
	u32 base = x5h_mix_base(ctx->cfg->mix_id);

	x5h_scu_writel(ctx, base + X5H_MIX_SWRSR, 0);
	x5h_scu_writel(ctx, base + X5H_MIX_SWRSR, 1);
}

#define MIX_NO_ATTN 0
#define MIX_MAX_ATTN 0x3FF

static int x5h_mix_init(struct x5h_audio *ctx)
{
	x5h_mix_activation(ctx);
	x5h_mix_volume_init(ctx);
	x5h_mix_volume_update(ctx,
			MIX_NO_ATTN,
			MIX_NO_ATTN,
			MIX_MAX_ATTN,
			MIX_MAX_ATTN);
	return 0;
}

/* ============================================================================
 *  Pipeline orchestration — Public API
 * ============================================================================
 */
int x5h_audio_init(struct x5h_audio *ctx)
{
	int ret;

	if (!ctx || !ctx->cfg)
		return -EINVAL;

	if (!ctx->cfg->scu_base || !ctx->cfg->adg_base ||
	    !ctx->cfg->ssiu_base)
		return -EINVAL;

	ret = x5h_adg_init(ctx);
	if (ret)
		return ret;

	ret = x5h_ssi_init(ctx);
	if (ret)
		return ret;

	x5h_ssiu_init(ctx);

	ret = x5h_dvc_init(ctx);
	if (ret)
		return ret;

	ret = x5h_mix_init(ctx);
	if (ret)
		return ret;

	ret = x5h_ctu_init(ctx, 0);
	if (ret)
		return ret;

	ret = x5h_ctu_init(ctx, 1);
	if (ret)
		return ret;

	ret = x5h_cmd_init(ctx);
	if (ret)
		return ret;

	ret = x5h_src_init(ctx, 0);
	if (ret)
		return ret;

	ret = x5h_src_init(ctx, 1);
	if (ret)
		return ret;

	ctx->initialized = true;

	return 0;
}
EXPORT_SYMBOL_GPL(x5h_audio_init);

int x5h_audio_start(struct x5h_audio *ctx)
{
	x5h_dma_dmapp_start(ctx);

	x5h_ssi_start(ctx);

	x5h_ssiu_start(ctx);

	x5h_cmd_start(ctx);

	x5h_src_start(ctx, 0);
	x5h_src_start(ctx, 1);

	ctx->pipeline_started = true;

	return 0;
}
EXPORT_SYMBOL_GPL(x5h_audio_start);

void x5h_audio_stop(struct x5h_audio *ctx)
{
	x5h_src_stop(ctx, 0);
	x5h_src_stop(ctx, 1);

	x5h_cmd_stop(ctx);

	x5h_ssiu_stop(ctx);

	x5h_ssi_stop(ctx);

	x5h_dma_dmapp_stop(ctx);

	ctx->pipeline_started = false;
}
EXPORT_SYMBOL_GPL(x5h_audio_stop);

void x5h_audio_deinit(struct x5h_audio *ctx)
{
	x5h_src_quit(ctx, 0);
	x5h_src_quit(ctx, 1);

	x5h_ctu_quit(ctx, 0);
	x5h_ctu_quit(ctx, 1);

	x5h_dvc_quit(ctx);
	x5h_ssiu_quit(ctx);
	x5h_ssi_quit(ctx);

	ctx->initialized = false;
}
EXPORT_SYMBOL_GPL(x5h_audio_deinit);

void x5h_audio_set_playback_volume(struct x5h_audio *ctx, u32 vol[8])
{
	u32 base = x5h_dvc_base(ctx->cfg->dvc_id);
	int i;

	if (!ctx || !ctx->cfg)
		return;

	x5h_scu_writel(ctx, base + X5H_DVC_DVUER, 0);

	for (i = 0; i < 8; i++) {
		ctx->dvc_vol[i] = vol[i];
		x5h_scu_writel(ctx, base + X5H_DVC_VOL0R + i * 4, vol[i]);
	}

	x5h_scu_writel(ctx, base + X5H_DVC_DVUER, 1);
}
EXPORT_SYMBOL_GPL(x5h_audio_set_playback_volume);

void x5h_audio_set_playback_mute(struct x5h_audio *ctx, unsigned int ch,
				 bool mute)
{
	if (!ctx || ch >= 8)
		return;

	ctx->dvc_mute[ch] = mute;
	x5h_dvc_volume_update(ctx);
}
EXPORT_SYMBOL_GPL(x5h_audio_set_playback_mute);
/* ============================================================================
 *  Buffer access API (for ALSA to use the pre-allocated buffers)
 * ============================================================================
 */
void *x5h_audio_get_pcm_buffer(struct x5h_audio *ctx, int stream_id)
{
	if (!ctx || stream_id < 0 || stream_id >= X5H_AUDIO_MAX_STREAMS)
		return NULL;
	return ctx->pcm_buf_virt[stream_id];
}
EXPORT_SYMBOL_GPL(x5h_audio_get_pcm_buffer);

dma_addr_t x5h_audio_get_pcm_dma_addr(struct x5h_audio *ctx, int stream_id)
{
	if (!ctx || stream_id < 0 || stream_id >= X5H_AUDIO_MAX_STREAMS)
		return 0;
	return ctx->pcm_buf_dma[stream_id];
}
EXPORT_SYMBOL_GPL(x5h_audio_get_pcm_dma_addr);

size_t x5h_audio_get_pcm_buffer_size(struct x5h_audio *ctx)
{
	if (!ctx)
		return 0;
	return ctx->pcm_buf_size;
}
EXPORT_SYMBOL_GPL(x5h_audio_get_pcm_buffer_size);

void x5h_audio_set_period_cb(struct x5h_audio *ctx, int stream_id,
			     void (*cb)(void *), void *data)
{
	if (!ctx || stream_id < 0 || stream_id >= X5H_AUDIO_MAX_STREAMS)
		return;
	ctx->period_cb[stream_id] = cb;
	ctx->period_cb_data[stream_id] = data;
	wmb();
}
EXPORT_SYMBOL_GPL(x5h_audio_set_period_cb);

/* ============================================================================
 *  Global instance management
 * ============================================================================
 */
static struct x5h_audio *g_x5h_audio_ctx;

void x5h_audio_set_global_instance(struct x5h_audio *ctx)
{
	g_x5h_audio_ctx = ctx;
}
EXPORT_SYMBOL_GPL(x5h_audio_set_global_instance);

struct x5h_audio *x5h_audio_get_global_instance(void)
{
	return g_x5h_audio_ctx;
}
EXPORT_SYMBOL_GPL(x5h_audio_get_global_instance);

