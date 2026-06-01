/* SPDX-License-Identifier: GPL-2.0 */
/*
 * X5H Gen5 Audio Pipeline — Hardware Register Definitions
 *
 * Extracted from Renesas R-Car sound driver (rsnd.h, gen.c, ssi.c, etc.)
 *
 * Copyright (C) 2025 Renesas Electronics Corp.
 */

#ifndef X5H_AUDIO_REGS_H
#define X5H_AUDIO_REGS_H

#include <linux/types.h>

/* ============================================================================
 *  MMIO Region Offsets (within each base)
 * ============================================================================
 *
 * Gen5 has 3 active MMIO regions:
 *   scu_base  = 0xec500000  (SCU: SRC, CTU, DVC, CMD)
 *   adg_base  = 0xec530000  (ADG: Audio Clock Generator)
 *   ssiu_base = 0xec540000  (SSIU + SSI inside SSIU space)
 *   dma_base  = 0xec740000  (Audio DMAC peri-peri)
 *
 * SSI registers on Gen5 live INSIDE SSIU memory space:
 *   SSICR = ssiu_base + ssi_id * 0x10000 + 0x9000
 */

/* ============================================================================
 *  SCU Registers (base + src_id * 0x1000)
 * ============================================================================
 */
#define X5H_SRC_I_BUSIF_MODE	0x0000
#define X5H_SRC_O_BUSIF_MODE	0x0004
#define X5H_SRC_BUSIF_DALIGN	0x0008
#define X5H_SRC_ROUTE_MODE0	0x000C
#define X5H_SRC_CTRL		0x0010
#define X5H_SRC_STATUS0		0x01C8
#define X5H_SRC_INT_EN0		0x01CC
#define X5H_SRC_STATUS1		0x01D0
#define X5H_SRC_INT_EN1		0x01D4
#define X5H_SRC_SWRSR		0x0200
#define X5H_SRC_SRCIR		0x0204
#define X5H_SRC_ADINR		0x0214
#define X5H_SRC_IFSCR		0x021C
#define X5H_SRC_IFSVR		0x0220
#define X5H_SRC_SRCCR		0x0224
#define X5H_SRC_BSDSR		0x022C
#define X5H_SRC_BSISR		0x0238
#define X5H_SRC_IN_TIMSEL	0x0300
#define X5H_SRC_OUT_TIMSEL	0x0304

/* SRC_DVC sub-block (Gen5, per SRC) */
#define X5H_SRC_DVC_SWRSR	0x0E00
#define X5H_SRC_DVC_DVUIR	0x0E04
#define X5H_SRC_DVC_ADINR	0x0E08
#define X5H_SRC_DVC_DVUCR	0x0E10
#define X5H_SRC_DVC_DVUCR2	0x0E54
#define X5H_SRC_DVC_ZCMCR	0x0E14
#define X5H_SRC_DVC_VRCTR	0x0E18
#define X5H_SRC_DVC_VRPDR	0x0E1C
#define X5H_SRC_DVC_VRDBR	0x0E20
#define X5H_SRC_DVC_VOL0R	0x0E28
#define X5H_SRC_DVC_VOL1R	0x0E2C
#define X5H_SRC_DVC_VOL2R	0x0E30
#define X5H_SRC_DVC_VOL3R	0x0E34
#define X5H_SRC_DVC_VOL4R	0x0E38
#define X5H_SRC_DVC_VOL5R	0x0E3C
#define X5H_SRC_DVC_VOL6R	0x0E40
#define X5H_SRC_DVC_VOL7R	0x0E44
#define X5H_SRC_DVC_DVUER	0x0E48

/* ============================================================================
 *  CMD Registers (base + cmd_id * 0x1000)
 * ============================================================================
 */
#define X5H_CMD_BUSIF_MODE	0xC184
#define X5H_CMD_BUSIF_DALIGN	0xC188
#define X5H_CMD_ROUTE_SLCT	0xC18C
#define X5H_CMD_CTRL		0xC190
#define X5H_CMDOUT_TIMSEL	0xC304

/* ============================================================================
 *  CTU Registers (ctu00: base + 0xC400 + 0x000; ctu01: +0x100, etc.)
 *  CTU sub-unit stride: 0x100 within CTU group
 *  CTU group offset: ctu_id/4 * 0x100
 * ============================================================================
 */
#define X5H_CTU_SWRSR	0x0000
#define X5H_CTU_CTUIR	0x0004
#define X5H_CTU_ADINR	0x0008
#define X5H_CTU_CPMDR	0x0010
#define X5H_CTU_SCMDR	0x0014
#define X5H_CTU_SV00R	0x0018
/* SV01R-SV37R follow: +0x4 each */
/* ============================================================================
 *  DVC Registers (base + 0xCE00 + dvc_id * 0x1000)
 * ============================================================================
 */
#define X5H_DVC_SWRSR	0x0000
#define X5H_DVC_DVUIR	0x0004
#define X5H_DVC_ADINR	0x0008
#define X5H_DVC_DVUCR	0x0010
#define X5H_DVC_ZCMCR	0x0014
#define X5H_DVC_VRCTR	0x0018
#define X5H_DVC_VRPDR	0x001C
#define X5H_DVC_VRDBR	0x0020
#define X5H_DVC_VOL0R	0x0028
#define X5H_DVC_VOL1R	0x002C
#define X5H_DVC_VOL2R	0x0030
#define X5H_DVC_VOL3R	0x0034
#define X5H_DVC_VOL4R	0x0038
#define X5H_DVC_VOL5R	0x003C
#define X5H_DVC_VOL6R	0x0040
#define X5H_DVC_VOL7R	0x0044
#define X5H_DVC_DVUER	0x0048

/* ============================================================================
 *  MIX Registers (base + 0xCD00 + mix_id * 0x1000)
 *  Offsets are relative to x5h_mix_base()
 * ============================================================================
 */
#define X5H_MIX_SWRSR	0x0000
#define X5H_MIX_MIXIR	0x0004
#define X5H_MIX_ADINR	0x0008
#define X5H_MIX_MIXMR	0x0010
#define X5H_MIX_MVPDR	0x0014
#define X5H_MIX_MDBAR	0x0018
#define X5H_MIX_MDBBR	0x001C
#define X5H_MIX_MDBCR	0x0020
#define X5H_MIX_MDBDR	0x0024
#define X5H_MIX_MDBER	0x0028

/* ============================================================================
 *  ADG Registers
 * ============================================================================
 */
#define X5H_ADG_BRRA		0x00
#define X5H_ADG_BRRB		0x04
#define X5H_ADG_BRGCKR		0x08
#define X5H_ADG_DIV_EN		0x30

/* ============================================================================
 *  SSIU/SSI Registers (per SSI, base + ssi_id * 0x10000)
 * ============================================================================
 */

/* --- Per-SSI global registers --- */
#define X5H_SSI_STATUS		0x8040
#define X5H_SSI_INT_ENABLE	0x8090
#define X5H_SSIU_AUDIO_CLK_SEL	0xA000
#define X5H_SSI_MODE4		0x8010

/* --- BUSIF registers (busif_id * 0x1000 offset) --- */
#define X5H_SSI_BUSIF_MODE(n)	(0x0000 + (n) * 0x1000)
#define X5H_SSI_BUSIF_ADINR(n)	(0x0004 + (n) * 0x1000)
#define X5H_SSI_BUSIF_DALIGN(n) (0x0020 + (n) * 0x1000)
#define X5H_SSI_CTRL(n)		(0x0010 + (n) * 0x1000)

/* --- SSI registers (embedded in SSIU space, per SSI) --- */
#define X5H_SSI_SSICR		0x9000
#define X5H_SSI_SSISR		0x9004
#define X5H_SSI_SSITDR		0x9008
#define X5H_SSI_SSIRDR		0x900C
#define X5H_SSI_SSIWSR		0x9020

/* ============================================================================
 *  SSICR bit definitions
 * ============================================================================
 */
#define SSICR_FORCE		(1u << 31)
#define SSICR_DMEN		(1u << 28)
#define SSICR_UIEN		(1u << 27)
#define SSICR_OIEN		(1u << 26)
#define SSICR_IIEN		(1u << 25)
#define SSICR_DIEN		(1u << 24)
#define SSICR_CHNL_4		(1u << 22)
#define SSICR_CHNL_6		(2u << 22)
#define SSICR_CHNL_8		(3u << 22)
#define SSICR_DWL_MASK		(7u << 19)
#define SSICR_DWL_8		(0u << 19)
#define SSICR_DWL_16		(1u << 19)
#define SSICR_DWL_18		(2u << 19)
#define SSICR_DWL_20		(3u << 19)
#define SSICR_DWL_22		(4u << 19)
#define SSICR_DWL_24		(5u << 19)
#define SSICR_DWL_32		(6u << 19)
#define SSICR_SWL_16		(1u << 16)
#define SSICR_SWL_24		(2u << 16)
#define SSICR_SWL_32		(3u << 16)
#define SSICR_SCKD		(1u << 15)
#define SSICR_SWSD		(1u << 14)
#define SSICR_SCKP		(1u << 13)
#define SSICR_SWSP		(1u << 12)
#define SSICR_SDTA		(1u << 10)
#define SSICR_PDTA		(1u << 9)
#define SSICR_DEL		(1u << 8)
#define SSICR_CKDV(v)		((u32)(v) << 4)
#define SSICR_TRMD		(1u << 1)
#define SSICR_EN		(1u << 0)

/* SSISR bits */
#define SSISR_UIRQ		(1u << 27)
#define SSISR_OIRQ		(1u << 26)
#define SSISR_IIRQ		(1u << 25)
#define SSISR_DIRQ		(1u << 24)

/* SSIWSR bits */
#define SSIWSR_CONT		(1u << 8)
#define SSIWSR_WS_MODE		(1u << 0)

/* ============================================================================
 *  ADG register bits
 * ============================================================================
 */
#define ADG_BRRA_MASK		0x3FF
#define ADG_BRRB_MASK		0x3FF

/* ============================================================================
 *  CMD_CTRL bits
 * ============================================================================
 */
#define CMD_CTRL_START		0x10

/* ============================================================================
 *  DMA peri-peri registers (relative to dma_base)
 * ============================================================================
 */
#define X5H_DMA_PDMASAR		0x00
#define X5H_DMA_PDMADAR		0x04
#define X5H_DMA_PDMACHCR	0x0C
#define X5H_DMA_PDMACHCR_DE	(1u << 0)

/* ============================================================================
 *  Module base offset helpers
 * ============================================================================
 */

/* Compute register address within SCU for a given module */
static inline u32 x5h_src_base(unsigned int src_id)
{
	return src_id * 0x1000;
}

static inline u32 x5h_src_reg(unsigned int src_id, u32 offset)
{
	return x5h_src_base(src_id) + offset;
}

static inline u32 x5h_mix_base(unsigned int mix_id)
{
    return 0xCD00 + mix_id * 0x1000;
}

static inline u32 x5h_ctu_base(unsigned int ctu_id)
{
	/* rcar gen.c: CTU regs at 0xc400 with stride 0x100 per ctu id
	 * (ctu00..ctu03 = 0..3, ctu10..ctu13 = 4..7) */
	return 0xC400 + ctu_id * 0x100;
}

static inline u32 x5h_ctu_reg(unsigned int ctu_id, u32 offset)
{
	return x5h_ctu_base(ctu_id) + offset;
}

static inline u32 x5h_ctu_sv_reg(unsigned int ctu_id, unsigned int row, unsigned int col)
{
	/* SV00R offset + (row * 8 + col) * 4 */
	return x5h_ctu_reg(ctu_id, 0x0018 + (row * 8 + col) * 4);
}

static inline u32 x5h_dvc_base(unsigned int dvc_id)
{
	return 0xCE00 + dvc_id * 0x1000;
}

static inline u32 x5h_dvc_reg(unsigned int dvc_id, u32 offset)
{
	return x5h_dvc_base(dvc_id) + offset;
}

static inline u32 x5h_cmd_base(unsigned int cmd_id)
{
	return cmd_id * 0x1000;
}

static inline u32 x5h_ssiu_per_ssi_base(unsigned int ssi_id)
{
	return ssi_id * 0x10000;
}

static inline u32 x5h_dma_per_ch(unsigned int dmapp_id)
{
	/* Gen5: +0x20 + ch * 0x1000 */
	return 0x20 + dmapp_id * 0x1000;
}

/* ============================================================================
 *  ADINR bit (Audio Data Input Register format indicator)
 * ============================================================================
 */
#define X5H_ADINR_BIT		(8 << 16)
// #define X5H_ADINR_BIT		(1u << 31)	[> I2S/TDM format marker <]

/* ============================================================================
 *  SRC conversion table data (from src.c)
 * ============================================================================
 */
static const u32 x5h_bsdsr_pat1[] = {
	0x01800000, 0x01000000, 0x00c00000, 0x00800000, 0x00600000, 0x00400000,
};

static const u32 x5h_bsdsr_pat2[] = {
	0x02400000, 0x01800000, 0x01200000, 0x00c00000, 0x00900000, 0x00600000,
};

static const u32 x5h_bsisr[] = {
	0x00100060, 0x00100040, 0x00100030, 0x00100020, 0x00100020, 0x00100020,
};

/* Channel pattern tables */
static const u32 x5h_chan288888[] = {
	0x00000006, 0x000001fe, 0x000001fe, 0x000001fe, 0x000001fe, 0x000001fe,
};

static const u32 x5h_chan244888[] = {
	0x00000006, 0x0000001e, 0x0000001e, 0x000001fe, 0x000001fe, 0x000001fe,
};

static const u32 x5h_chan222222[] = {
	0x00000006, 0x00000006, 0x00000006, 0x00000006, 0x00000006, 0x00000006,
};

/* ============================================================================
 *  SSI CKDV multiplier table
 * ============================================================================
 */
static const unsigned int x5h_ssi_clk_mul[] = {
	1, 2, 4, 8, 16, 6, 12,
};

#define X5H_SSI_CKDV_NUM	7

/* CKDV register encodings (index -> CKDV bit field) */
static const unsigned int x5h_ckdv_encode[] = {
	0, 1, 2, 3, 4, 5, 6,
};

/* ============================================================================
 *  Clock selection table indices
 * ============================================================================
 */
#define X5H_CLK_CLKA	0
#define X5H_CLK_CLKB	1
#define X5H_CLK_CLKC	2
#define X5H_CLK_RBGA	3
#define X5H_CLK_RBGB	4
#define X5H_CLK_MAX	5

#endif /* X5H_AUDIO_REGS_H */
