/*
 * TLV320AIC3106 register definitions
 *
 * Every value in this file comes from the TLV320AIC3106 datasheet
 * (SLAS509G, Rev. G), section 10.6 "Register Maps". Table numbers are given
 * next to each register. Only the registers this driver uses are listed.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_AUDIO_TLV320AIC3106_H_
#define ZEPHYR_DRIVERS_AUDIO_TLV320AIC3106_H_

#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A register is identified by {page, address}. Only pages 0 and 1 exist. */
struct aic3106_reg {
	uint8_t page;
	uint8_t addr;
};

#define AIC3106_REG(_page, _addr)	((struct aic3106_reg){.page = (_page), .addr = (_addr)})

/** Maximum ADC input gain in 0.5 dB steps (59.5 dB) */
#define TLV320AIC3106_INPUT_GAIN_MAX    119

/* Table 10-8: Page 0 / Reg 0, page select (address 0 on every page) */
#define AIC3106_PAGE_CTRL_ADDR		0
#define AIC3106_PAGE_MASK		BIT(0)

/* Table 10-9: Software reset (self clearing) */
#define AIC3106_SW_RESET		AIC3106_REG(0, 1)
#define AIC3106_SW_RESET_ASSERT		BIT(7)

/* Table 10-10: Codec sample rate select. Value n gives fS = fS(ref) / ((n + 2) / 2) */
#define AIC3106_SRATE			AIC3106_REG(0, 2)
#define AIC3106_SRATE_ADC(n)		(((n) & 0xF) << 4)
#define AIC3106_SRATE_DAC(n)		((n) & 0xF)
#define AIC3106_SRATE_CODE_MAX		10	/* fS(ref) / 6 */

/* Table 10-11: PLL programming A */
#define AIC3106_PLL_A			AIC3106_REG(0, 3)
#define AIC3106_PLL_A_RESET		0x10	/* PLL off, Q = 2, P = 8 */
#define AIC3106_PLL_A_ENABLE		BIT(7)
#define AIC3106_PLL_A_Q(q)		(((q) & 0xF) << 3)	/* Q = 16 -> 0, Q = 17 -> 1 */
#define AIC3106_PLL_A_P(p)		((p) & 0x7)		/* P = 8 -> 0 */

/* Table 10-12 .. 10-14: PLL J, D */
#define AIC3106_PLL_B			AIC3106_REG(0, 4)
#define AIC3106_PLL_B_J(j)		(((j) & 0x3F) << 2)
#define AIC3106_PLL_C			AIC3106_REG(0, 5)	/* D[13:6] */
#define AIC3106_PLL_D			AIC3106_REG(0, 6)	/* D[5:0] in bits 7..2 */
#define AIC3106_PLL_D_LSB(d)		(((d) & 0x3F) << 2)

/* Table 10-15: Codec datapath setup */
#define AIC3106_DATAPATH		AIC3106_REG(0, 7)
#define AIC3106_DATAPATH_FSREF_44100	BIT(7)	/* 0: fS(ref) = 48 kHz */
#define AIC3106_DATAPATH_LDAC_LEFT	(1 << 3)	/* left DAC plays left input */
#define AIC3106_DATAPATH_RDAC_RIGHT	(1 << 1)	/* right DAC plays right input */

/* Table 10-16: Audio serial data interface A */
#define AIC3106_ASI_A			AIC3106_REG(0, 8)
#define AIC3106_ASI_A_BCLK_OUT		BIT(7)	/* 1: codec drives BCLK (master) */
#define AIC3106_ASI_A_WCLK_OUT		BIT(6)	/* 1: codec drives WCLK (master) */

/* Table 10-17: Audio serial data interface B */
#define AIC3106_ASI_B			AIC3106_REG(0, 9)
#define AIC3106_ASI_B_MODE_I2S		(0 << 6)
#define AIC3106_ASI_B_MODE_DSP		(1 << 6)
#define AIC3106_ASI_B_MODE_RJ		(2 << 6)
#define AIC3106_ASI_B_MODE_LJ		(3 << 6)
#define AIC3106_ASI_B_WLEN_16		(0 << 4)
#define AIC3106_ASI_B_WLEN_20		(1 << 4)
#define AIC3106_ASI_B_WLEN_24		(2 << 4)
#define AIC3106_ASI_B_WLEN_32		(3 << 4)

/* Table 10-18: Audio serial data interface C, data word offset in bit clocks */
#define AIC3106_ASI_C			AIC3106_REG(0, 10)

/* Table 10-19: Overflow flags (D7-D4, read only) and PLL R (D3-D0) */
#define AIC3106_PLL_R			AIC3106_REG(0, 11)
#define AIC3106_PLL_R_VAL(r)		((r) & 0xF)		/* R = 16 -> 0 */

/* Table 10-22: Headset/button detect B. Only D7 (driver coupling) is used */
#define AIC3106_HP_CFG			AIC3106_REG(0, 14)
#define AIC3106_HP_CFG_AC_COUPLED	BIT(7)	/* 0: capless */

/* Table 10-23, 10-24: ADC PGA gain, D7 = mute, D6-D0 = 0.5 dB steps */
#define AIC3106_ADC_PGA_L		AIC3106_REG(0, 15)
#define AIC3106_ADC_PGA_R		AIC3106_REG(0, 16)
#define AIC3106_ADC_PGA_MUTE		BIT(7)
#define AIC3106_ADC_PGA_GAIN_MAX	119	/* 0x77 = 59.5 dB */

/*
 * Table 10-25, 10-26: MIC3L/R to left/right ADC. Two 4-bit fields,
 * 0x0 = 0 dB (connected), 0xF = not connected.
 */
#define AIC3106_MIC3_TO_LADC		AIC3106_REG(0, 17)	/* D7-D4 MIC3L, D3-D0 MIC3R */
#define AIC3106_MIC3_TO_RADC		AIC3106_REG(0, 18)	/* D7-D4 MIC3L, D3-D0 MIC3R */

/*
 * Table 10-27 .. 10-32: LINE input to ADC. D7 = single-ended (0) / differential (1),
 * D6-D3 = input level (0x0 = 0 dB connected, 0xF = not connected).
 * D2 of registers 19 (left ADC) and 22 (right ADC) is the ADC power bit.
 */
#define AIC3106_LINE1L_TO_LADC		AIC3106_REG(0, 19)
#define AIC3106_LINE2L_TO_LADC		AIC3106_REG(0, 20)
#define AIC3106_LINE1R_TO_LADC		AIC3106_REG(0, 21)
#define AIC3106_LINE1R_TO_RADC		AIC3106_REG(0, 22)
#define AIC3106_LINE2R_TO_RADC		AIC3106_REG(0, 23)
#define AIC3106_LINE1L_TO_RADC		AIC3106_REG(0, 24)
#define AIC3106_LINE_LEVEL_0DB		(0x0 << 3)
#define AIC3106_LINE_NC			(0xF << 3)
#define AIC3106_ADC_POWER		BIT(2)

/* Table 10-33: MICBIAS, D7-D6 level (0 off, 1 2.0 V, 2 2.5 V, 3 AVDD) */
#define AIC3106_MICBIAS			AIC3106_REG(0, 25)
#define AIC3106_MICBIAS_LEVEL(x)	(((x) & 0x3) << 6)

/* Table 10-44: ADC flag register */
#define AIC3106_ADC_FLAG		AIC3106_REG(0, 36)
#define AIC3106_ADC_FLAG_LADC_ON	BIT(6)
#define AIC3106_ADC_FLAG_RADC_ON	BIT(2)

/* Table 10-45: DAC power (D7 left, D6 right) */
#define AIC3106_DAC_PWR			AIC3106_REG(0, 37)
#define AIC3106_DAC_PWR_LEFT		BIT(7)
#define AIC3106_DAC_PWR_RIGHT		BIT(6)

/* Table 10-48: Output stage control, D7-D6 = output common-mode voltage */
#define AIC3106_OUT_STAGE		AIC3106_REG(0, 40)
#define AIC3106_OUT_STAGE_CM_MASK	(0x3 << 6)
#define AIC3106_OUT_STAGE_CM(x)		(((x) & 0x3) << 6)

/*
 * Table 10-49: DAC output switching. D7-D6 left DAC path, D5-D4 right DAC path,
 * D1-D0 = 0: independent left/right digital volumes.
 *   path 1: DAC_x1 -> analog volume/mixer -> any output (needed for HP + line together)
 *   path 3: DAC_x3 -> line output driver directly (datasheet 10.3.5: best quality, low power)
 *   path 2: DAC_x2 -> HPLOUT/HPROUT directly (datasheet 10.3.6: best quality, low power)
 */
#define AIC3106_DAC_OUT_SW		AIC3106_REG(0, 41)
#define AIC3106_DAC_PATH_1		0
#define AIC3106_DAC_PATH_3		1
#define AIC3106_DAC_PATH_2		2
#define AIC3106_DAC_OUT_SW_VAL(path)	((((path) & 0x3) << 6) | (((path) & 0x3) << 4))

/* Table 10-51, 10-52: DAC digital volume, D7 = mute, D6-D0 = 0 .. -63.5 dB in 0.5 dB steps */
#define AIC3106_DAC_VOL_L		AIC3106_REG(0, 43)
#define AIC3106_DAC_VOL_R		AIC3106_REG(0, 44)
#define AIC3106_DAC_VOL_MUTE		BIT(7)
#define AIC3106_DAC_VOL_MASK		0x7F	/* register value == attenuation in 0.5 dB */

/*
 * Table 10-56, 10-73, 10-91, 10-101: DAC_x1 to output volume.
 * D7 = route enable, D6-D0 = analog gain (0 = 0 dB).
 */
#define AIC3106_DACL1_TO_HPLOUT		AIC3106_REG(0, 47)
#define AIC3106_DACR1_TO_HPROUT		AIC3106_REG(0, 64)
#define AIC3106_DACL1_TO_LEFT_LOP	AIC3106_REG(0, 82)
#define AIC3106_DACR1_TO_RIGHT_LOP	AIC3106_REG(0, 92)
#define AIC3106_ROUTE_ENABLE		BIT(7)

/*
 * Table 10-60, 10-74, 10-95, 10-102: output level control / mute / power.
 * D7-D4 = output level (0 = 0 dB), D3 = 1: NOT muted, D2 (HP only) = hi-Z when
 * powered down, D1 = volume status (read only), D0 = power.
 *
 * NOTE: for HPLOUT/HPROUT D0 is "Power Control" (R/W). For LEFT_LOP/M and
 * RIGHT_LOP/M datasheet Rev. G lists D0 as read-only "Power Status", yet Linux
 * (tlv320aic3x.c, DAPM "Left Line Out") sets it as a power control. The driver
 * sets D0 and then checks register 94; verify on real hardware.
 */
#define AIC3106_HPLOUT_CTRL		AIC3106_REG(0, 51)
#define AIC3106_HPROUT_CTRL		AIC3106_REG(0, 65)
#define AIC3106_LEFT_LOP_CTRL		AIC3106_REG(0, 86)
#define AIC3106_RIGHT_LOP_CTRL		AIC3106_REG(0, 93)
#define AIC3106_OUT_LEVEL_MASK		(0xF << 4)
#define AIC3106_OUT_UNMUTE		BIT(3)
#define AIC3106_OUT_POWER		BIT(0)

/* Table 10-103: Module power status (read only, 1 = fully powered up) */
#define AIC3106_PWR_STATUS		AIC3106_REG(0, 94)
#define AIC3106_PWR_STATUS_LDAC		BIT(7)
#define AIC3106_PWR_STATUS_RDAC		BIT(6)
#define AIC3106_PWR_STATUS_LEFT_LOP	BIT(4)
#define AIC3106_PWR_STATUS_RIGHT_LOP	BIT(3)
#define AIC3106_PWR_STATUS_HPLOUT	BIT(2)
#define AIC3106_PWR_STATUS_HPROUT	BIT(1)

/* Table 10-110: D0 = CODEC_CLKIN source: 0 = PLL, 1 = clock divider (PLL bypassed) */
#define AIC3106_CLKIN_SEL		AIC3106_REG(0, 101)
#define AIC3106_CLKIN_SEL_CLKDIV	BIT(0)

/* Output selection bit mask (from devicetree "ti,output-select") */
#define AIC3106_OUT_HEADPHONE		BIT(0)
#define AIC3106_OUT_LINEOUT		BIT(1)

/* Clock generation limits, datasheet section 10.3.2 */
#define AIC3106_MCLK_MIN_HZ		512000U
#define AIC3106_MCLK_MAX_HZ		50000000U

/* Allowed PLL error when no exact solution exists (TI's own table shows <= 7 ppm) */
#define AIC3106_PLL_MAX_ERR_PPM		100U

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_DRIVERS_AUDIO_TLV320AIC3106_H_ */
