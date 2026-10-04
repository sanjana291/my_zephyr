/*
 * TLV320AIC3106 stereo audio codec driver for Zephyr 3.7.x
 *
 * Structure of this file (top to bottom):
 *   1. config / data structures
 *   2. register access   : page select (with read-back), read, write, update
 *   3. rate + clock math : fS(ref) selection, PLL bypass or PLL search  (pure functions)
 *   4. configuration     : soft reset, clocks, I2S interface, output routing
 *   5. runtime control   : start/stop output, volume, mute, ADC capture
 *   6. Zephyr API table and devicetree instantiation
 *
 * The datasheet (SLAS509G) is the only source of register values. Section
 * and table numbers in comments refer to that document.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT ti_tlv320aic3106

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/audio/codec.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "tlv320aic3106.h"

LOG_MODULE_REGISTER(tlv320aic3106, CONFIG_AUDIO_CODEC_LOG_LEVEL);

/* Order matches the "ti,input-select" enum in the devicetree binding */
enum aic3106_input {
	AIC3106_INPUT_LINE1,
	AIC3106_INPUT_LINE2,
	AIC3106_INPUT_MIC3,
};

#define AIC3106_POLL_TIMEOUT_MS	100

/* Return from the calling function if a call fails (negative errno) */
#define AIC3106_TRY(_expr)				\
	do {						\
		int _err = (_expr);			\
		if (_err < 0) {				\
			return _err;			\
		}					\
	} while (0)

/* ------------------------------------------------------------------------ */
/* 1. Structures                                                            */
/* ------------------------------------------------------------------------ */

struct aic3106_config {
	struct i2c_dt_spec bus;
	struct gpio_dt_spec reset_gpio;
	uint8_t outputs;		/* AIC3106_OUT_* mask */
	uint8_t input;			/* enum aic3106_input */
	uint8_t micbias;		/* register 25 D7-D6 code */
	uint8_t output_cm;		/* register 40 D7-D6 code */
	bool hp_ac_coupled;
};

struct aic3106_data {
	struct k_mutex lock;		/* protects everything below and the I2C sequences */
	uint8_t page;			/* currently active register page (cache) */
	bool configured;		/* audio_codec_configure() succeeded */
	bool output_on;
	bool input_on;
	uint8_t dac_vol[2];		/* [0] left, [1] right: attenuation in 0.5 dB steps */
	bool dac_mute[2];		/* application-requested mute */
	uint8_t adc_gain;		/* ADC PGA gain in 0.5 dB steps */
	bool adc_mute;
};

/* ------------------------------------------------------------------------ */
/* 2. Register access (caller holds data->lock, except during init)         */
/* ------------------------------------------------------------------------ */

/*
 * Section 10.6: address 0 of each page selects the active page. After writing
 * it the datasheet recommends reading it back, which is done here.
 * A hardware or software reset returns the codec to page 0.
 */
static int aic3106_select_page(const struct device *dev, uint8_t page)
{
	const struct aic3106_config *cfg = dev->config;
	struct aic3106_data *data = dev->data;
	uint8_t readback;
	int ret;

	if (data->page == page) {
		return 0;
	}

	ret = i2c_reg_write_byte_dt(&cfg->bus, AIC3106_PAGE_CTRL_ADDR, page);
	if (ret < 0) {
		return ret;
	}

	ret = i2c_reg_read_byte_dt(&cfg->bus, AIC3106_PAGE_CTRL_ADDR, &readback);
	if (ret < 0) {
		return ret;
	}

	if ((readback & AIC3106_PAGE_MASK) != page) {
		LOG_ERR("page select failed: wrote %u, read back %u", page, readback);
		return -EIO;
	}

	data->page = page;
	return 0;
}

static int aic3106_read(const struct device *dev, struct aic3106_reg reg, uint8_t *val)
{
	const struct aic3106_config *cfg = dev->config;
	int ret;

	ret = aic3106_select_page(dev, reg.page);
	if (ret < 0) {
		return ret;
	}

	ret = i2c_reg_read_byte_dt(&cfg->bus, reg.addr, val);
	if (ret < 0) {
		LOG_ERR("read PG%u REG%u failed (%d)", reg.page, reg.addr, ret);
		return ret;
	}

	LOG_DBG("RD PG%u REG%u = 0x%02x", reg.page, reg.addr, *val);
	return 0;
}

static int aic3106_write(const struct device *dev, struct aic3106_reg reg, uint8_t val)
{
	const struct aic3106_config *cfg = dev->config;
	int ret;

	ret = aic3106_select_page(dev, reg.page);
	if (ret < 0) {
		return ret;
	}

	ret = i2c_reg_write_byte_dt(&cfg->bus, reg.addr, val);
	if (ret < 0) {
		LOG_ERR("write PG%u REG%u failed (%d)", reg.page, reg.addr, ret);
		return ret;
	}

	LOG_DBG("WR PG%u REG%u = 0x%02x", reg.page, reg.addr, val);
	return 0;
}

static int aic3106_update(const struct device *dev, struct aic3106_reg reg, uint8_t mask,
			  uint8_t val)
{
	const struct aic3106_config *cfg = dev->config;
	int ret;

	ret = aic3106_select_page(dev, reg.page);
	if (ret < 0) {
		return ret;
	}

	ret = i2c_reg_update_byte_dt(&cfg->bus, reg.addr, mask, val);
	if (ret < 0) {
		LOG_ERR("update PG%u REG%u failed (%d)", reg.page, reg.addr, ret);
		return ret;
	}

	LOG_DBG("UP PG%u REG%u mask 0x%02x val 0x%02x", reg.page, reg.addr, mask, val);
	return 0;
}

/* Poll a read-only status register until (reg & mask) == expected */
static int aic3106_wait_status(const struct device *dev, struct aic3106_reg reg, uint8_t mask,
			       uint8_t expected)
{
	uint8_t val;
	int ret;

	for (int i = 0; i < AIC3106_POLL_TIMEOUT_MS; i++) {
		ret = aic3106_read(dev, reg, &val);
		if (ret < 0) {
			return ret;
		}
		if ((val & mask) == expected) {
			return 0;
		}
		k_msleep(1);
	}

	return -ETIMEDOUT;
}

/* ------------------------------------------------------------------------ */
/* 3. Sample rate and clock math (no hardware access)                       */
/* ------------------------------------------------------------------------ */

/*
 * Table 10-10 / section 10.3.3: the converters run at fS(ref) / NDAC with
 * NDAC in {1, 1.5, 2, ... 6}. fS(ref) is 44.1 kHz for the 11.025 kHz family
 * and 48 kHz for everything else. The register code is 2 * NDAC - 2.
 *
 * Dual-rate mode (88.2/96 kHz) is NOT implemented: the datasheet text gives too
 * little detail to implement it without guessing.
 */
static int aic3106_get_rate_config(uint32_t rate, uint32_t *fsref, uint8_t *code)
{
	uint32_t ref, twice_ndac;

	if (rate == 0U) {
		return -EINVAL;
	}

	ref = (rate % 11025U == 0U) ? 44100U : 48000U;

	if ((2U * ref) % rate != 0U) {
		return -ENOTSUP;
	}

	twice_ndac = (2U * ref) / rate;
	if (twice_ndac < 2U || twice_ndac > 12U) {
		return -ENOTSUP;
	}

	*fsref = ref;
	*code = (uint8_t)(twice_ndac - 2U);
	return 0;
}

struct aic3106_pll {
	uint8_t p;
	uint8_t r;
	uint8_t j;
	uint16_t d;
};

/*
 * Section 10.3.2, equation (2): fS(ref) = MCLK * K * R / (2048 * P), K = J.D
 * Constraints (with PLL enabled):
 *   D == 0 : 2 MHz <= MCLK/P <= 20 MHz, 4 <= J <= 55
 *   D != 0 : 10 MHz <= MCLK/P <= 20 MHz, 4 <= J <= 11, R = 1
 *   80 MHz <= MCLK*K*R/P <= 110 MHz  (always true for fS(ref) = 44.1/48 kHz)
 * The search tries every P (1..8) and R (1..16), rounds K to 4 decimals and keeps
 * the smallest error. The first exact solution wins, so P and R stay small.
 */
static int aic3106_find_pll(uint32_t mclk, uint32_t fsref, struct aic3106_pll *pll)
{
	uint64_t best_ppm = UINT64_MAX;
	bool found = false;

	for (uint32_t p = 1; p <= 8U; p++) {
		for (uint32_t r = 1; r <= 16U; r++) {
			/* K * 10000 = 2048 * fS(ref) * P * 10000 / (MCLK * R), rounded */
			uint64_t num = 2048ULL * fsref * p * 10000ULL;
			uint64_t den = (uint64_t)mclk * r;
			uint64_t k = (num + den / 2U) / den;
			uint32_t j = (uint32_t)(k / 10000U);
			uint32_t d = (uint32_t)(k % 10000U);
			uint64_t want, got, diff, ppm;

			if (d == 0U) {
				if (mclk < 2000000ULL * p || mclk > 20000000ULL * p ||
				    j < 4U || j > 55U) {
					continue;
				}
			} else {
				if (r != 1U || mclk < 10000000ULL * p ||
				    mclk > 20000000ULL * p || j < 4U || j > 11U) {
					continue;
				}
			}

			/* Compare fS(ref) * scale with MCLK * K * R, all integers */
			want = (uint64_t)fsref * 2048ULL * p * 10000ULL;
			got = (uint64_t)mclk * k * r;
			diff = (got > want) ? (got - want) : (want - got);
			if (diff > want / 1000U) {
				continue;	/* worse than 1000 ppm (also avoids overflow below) */
			}
			ppm = diff * 1000000ULL / want;

			if (ppm < best_ppm) {
				best_ppm = ppm;
				pll->p = (uint8_t)p;
				pll->r = (uint8_t)r;
				pll->j = (uint8_t)j;
				pll->d = (uint16_t)d;
				found = true;
			}
		}
	}

	if (!found || best_ppm > AIC3106_PLL_MAX_ERR_PPM) {
		return -EINVAL;
	}

	return 0;
}

/* ------------------------------------------------------------------------ */
/* 4. Configuration                                                         */
/* ------------------------------------------------------------------------ */

static int aic3106_soft_reset(const struct device *dev)
{
	struct aic3106_data *data = dev->data;

	AIC3106_TRY(aic3106_write(dev, AIC3106_SW_RESET, AIC3106_SW_RESET_ASSERT));
	/* Reset returns the codec to page 0. The datasheet gives no reset time; 1 ms is margin. */
	data->page = 0;
	k_msleep(1);
	return 0;
}

/*
 * Generate CODEC_CLKIN = 256 * fS(ref) from MCLK.
 *  1) preferred: bypass the PLL, fS(ref) = MCLK / (128 * Q), Q = 2..17 (equation 1)
 *  2) otherwise: use the PLL (equation 2)
 * frac_ndac: NDAC is 1.5, 2.5 ... in which case section 10.3.2 forbids odd Q.
 */
static int aic3106_program_clocks(const struct device *dev, uint32_t mclk, uint32_t fsref,
				  bool frac_ndac)
{
	struct aic3106_pll pll;
	uint32_t div = 128U * fsref;
	int ret;

	if (mclk < AIC3106_MCLK_MIN_HZ || mclk > AIC3106_MCLK_MAX_HZ) {
		LOG_ERR("MCLK %u Hz outside %u..%u Hz", mclk, AIC3106_MCLK_MIN_HZ,
			AIC3106_MCLK_MAX_HZ);
		return -EINVAL;
	}

	if ((mclk % div) == 0U) {
		uint32_t q = mclk / div;

		if (q >= 2U && q <= 17U && !(frac_ndac && (q & 1U))) {
			AIC3106_TRY(aic3106_write(dev, AIC3106_PLL_A,
						  AIC3106_PLL_A_Q(q) | AIC3106_PLL_A_P(1)));
			AIC3106_TRY(aic3106_update(dev, AIC3106_CLKIN_SEL,
						   AIC3106_CLKIN_SEL_CLKDIV,
						   AIC3106_CLKIN_SEL_CLKDIV));
			LOG_INF("MCLK %u Hz, fS(ref) %u Hz: PLL bypassed, Q=%u", mclk, fsref, q);
			return 0;
		}
	}

	ret = aic3106_find_pll(mclk, fsref, &pll);
	if (ret < 0) {
		LOG_ERR("no PLL setting for MCLK %u Hz -> fS(ref) %u Hz", mclk, fsref);
		return ret;
	}

	LOG_INF("MCLK %u Hz, fS(ref) %u Hz: PLL P=%u R=%u J=%u D=%u", mclk, fsref, pll.p, pll.r,
		pll.j, pll.d);

	AIC3106_TRY(aic3106_write(dev, AIC3106_PLL_B, AIC3106_PLL_B_J(pll.j)));
	/* Register 5 must be written immediately followed by register 6 (Table 10-13) */
	AIC3106_TRY(aic3106_write(dev, AIC3106_PLL_C, (uint8_t)(pll.d >> 6)));
	AIC3106_TRY(aic3106_write(dev, AIC3106_PLL_D, AIC3106_PLL_D_LSB(pll.d)));
	AIC3106_TRY(aic3106_write(dev, AIC3106_PLL_R, AIC3106_PLL_R_VAL(pll.r)));
	AIC3106_TRY(aic3106_update(dev, AIC3106_CLKIN_SEL, AIC3106_CLKIN_SEL_CLKDIV, 0));
	AIC3106_TRY(aic3106_write(dev, AIC3106_PLL_A,
				  AIC3106_PLL_A_ENABLE | AIC3106_PLL_A_Q(2) |
				  AIC3106_PLL_A_P(pll.p)));

	/* The datasheet specifies no PLL lock time; 10 ms is a conservative margin. */
	k_msleep(10);
	return 0;
}

/*
 * Translate the Zephyr I2S configuration into registers 8 and 9.
 *
 * Zephyr's i2s_config describes the I2S *controller* (the SoC side): the same
 * struct is passed to i2s_configure(). "BIT_CLK_MASTER" therefore means the SoC
 * drives BCLK, so the codec is a slave. Note that I2S_OPT_*_MASTER is defined as
 * (0 << n), i.e. zero, and cannot be tested with '&'; only the *_SLAVE bits can.
 * Codec master mode (BCLK/WCLK outputs) is selected when the SoC is a slave.
 */
static int aic3106_dai_regs(const struct i2s_config *i2s, uint8_t *asi_a, uint8_t *asi_b)
{
	uint8_t a = 0U;
	uint8_t b = 0U;

	if (i2s->channels != 2U) {
		LOG_ERR("only stereo (2 channels) is supported, got %u", i2s->channels);
		return -EINVAL;
	}

	if (i2s->format & (I2S_FMT_BIT_CLK_INV | I2S_FMT_FRAME_CLK_INV | I2S_FMT_DATA_ORDER_LSB)) {
		LOG_ERR("clock inversion / LSB-first are not supported");
		return -ENOTSUP;
	}

	switch (i2s->format & I2S_FMT_DATA_FORMAT_MASK) {
	case I2S_FMT_DATA_FORMAT_I2S:
		b |= AIC3106_ASI_B_MODE_I2S;
		break;
	case I2S_FMT_DATA_FORMAT_LEFT_JUSTIFIED:
		b |= AIC3106_ASI_B_MODE_LJ;
		break;
	case I2S_FMT_DATA_FORMAT_RIGHT_JUSTIFIED:
		b |= AIC3106_ASI_B_MODE_RJ;
		break;
	default:
		/* PCM short/long would map to DSP mode, which needs offset handling not done here */
		LOG_ERR("data format 0x%x not supported (I2S, LJ, RJ only)",
			i2s->format & I2S_FMT_DATA_FORMAT_MASK);
		return -ENOTSUP;
	}

	switch (i2s->word_size) {
	case 16:
		b |= AIC3106_ASI_B_WLEN_16;
		break;
	case 20:
		b |= AIC3106_ASI_B_WLEN_20;
		break;
	case 24:
		b |= AIC3106_ASI_B_WLEN_24;
		break;
	case 32:
		b |= AIC3106_ASI_B_WLEN_32;
		break;
	default:
		LOG_ERR("unsupported word size %u", i2s->word_size);
		return -EINVAL;
	}

	if (i2s->options & I2S_OPT_BIT_CLK_SLAVE) {
		a |= AIC3106_ASI_A_BCLK_OUT;
	}
	if (i2s->options & I2S_OPT_FRAME_CLK_SLAVE) {
		a |= AIC3106_ASI_A_WCLK_OUT;
	}

	*asi_a = a;
	*asi_b = b;
	return 0;
}

/* Apply a value to the level/mute/power register of every selected output driver */
static int aic3106_out_ctrl_update(const struct device *dev, uint8_t mask, uint8_t val)
{
	const struct aic3106_config *cfg = dev->config;

	if (cfg->outputs & AIC3106_OUT_HEADPHONE) {
		AIC3106_TRY(aic3106_update(dev, AIC3106_HPLOUT_CTRL, mask, val));
		AIC3106_TRY(aic3106_update(dev, AIC3106_HPROUT_CTRL, mask, val));
	}
	if (cfg->outputs & AIC3106_OUT_LINEOUT) {
		AIC3106_TRY(aic3106_update(dev, AIC3106_LEFT_LOP_CTRL, mask, val));
		AIC3106_TRY(aic3106_update(dev, AIC3106_RIGHT_LOP_CTRL, mask, val));
	}
	return 0;
}

/* DAC digital volume registers: the mute bit is set whenever output is off or muted */
static int aic3106_write_dac_vol(const struct device *dev)
{
	struct aic3106_data *data = dev->data;
	uint8_t l = data->dac_vol[0];
	uint8_t r = data->dac_vol[1];

	if (!data->output_on || data->dac_mute[0]) {
		l |= AIC3106_DAC_VOL_MUTE;
	}
	if (!data->output_on || data->dac_mute[1]) {
		r |= AIC3106_DAC_VOL_MUTE;
	}

	AIC3106_TRY(aic3106_write(dev, AIC3106_DAC_VOL_L, l));
	return aic3106_write(dev, AIC3106_DAC_VOL_R, r);
}

/* Route the DAC to the selected outputs. Drivers and DACs stay powered down. */
static int aic3106_setup_outputs(const struct device *dev)
{
	const struct aic3106_config *cfg = dev->config;
	uint8_t path;

	/* Section 10.3.6: program the output configuration before powering the drivers */
	AIC3106_TRY(aic3106_update(dev, AIC3106_HP_CFG, AIC3106_HP_CFG_AC_COUPLED,
				   cfg->hp_ac_coupled ? AIC3106_HP_CFG_AC_COUPLED : 0));
	AIC3106_TRY(aic3106_update(dev, AIC3106_OUT_STAGE, AIC3106_OUT_STAGE_CM_MASK,
				   AIC3106_OUT_STAGE_CM(cfg->output_cm)));

	/* Datasheet-recommended direct DAC paths when only one output type is used */
	if (cfg->outputs == AIC3106_OUT_HEADPHONE) {
		path = AIC3106_DAC_PATH_2;
	} else if (cfg->outputs == AIC3106_OUT_LINEOUT) {
		path = AIC3106_DAC_PATH_3;
	} else {
		path = AIC3106_DAC_PATH_1;
	}
	AIC3106_TRY(aic3106_write(dev, AIC3106_DAC_OUT_SW, AIC3106_DAC_OUT_SW_VAL(path)));

	if (path == AIC3106_DAC_PATH_1) {
		/* Mixer path: DAC_L1/R1 to every output at 0 dB analog gain */
		AIC3106_TRY(aic3106_write(dev, AIC3106_DACL1_TO_HPLOUT, AIC3106_ROUTE_ENABLE));
		AIC3106_TRY(aic3106_write(dev, AIC3106_DACR1_TO_HPROUT, AIC3106_ROUTE_ENABLE));
		AIC3106_TRY(aic3106_write(dev, AIC3106_DACL1_TO_LEFT_LOP, AIC3106_ROUTE_ENABLE));
		AIC3106_TRY(aic3106_write(dev, AIC3106_DACR1_TO_RIGHT_LOP, AIC3106_ROUTE_ENABLE));
	}

	/* Output level 0 dB, not muted, powered down */
	AIC3106_TRY(aic3106_out_ctrl_update(dev, AIC3106_OUT_LEVEL_MASK | AIC3106_OUT_UNMUTE |
					    AIC3106_OUT_POWER, AIC3106_OUT_UNMUTE));

	/* DACs muted until start_output() */
	return aic3106_write_dac_vol(dev);
}

static int aic3106_configure_locked(const struct device *dev, struct audio_codec_cfg *codec_cfg)
{
	const struct aic3106_config *cfg = dev->config;
	struct aic3106_data *data = dev->data;
	const struct i2s_config *i2s = &codec_cfg->dai_cfg.i2s;
	uint32_t fsref;
	uint8_t code, asi_a, asi_b;
	int ret;

	if (codec_cfg->dai_type != AUDIO_DAI_TYPE_I2S) {
		LOG_ERR("dai_type must be AUDIO_DAI_TYPE_I2S");
		return -EINVAL;
	}

	/* Validate everything first so a bad request leaves the hardware untouched */
	ret = aic3106_get_rate_config(i2s->frame_clk_freq, &fsref, &code);
	if (ret < 0) {
		LOG_ERR("unsupported sample rate %u Hz (8..48 kHz single-rate only)",
			i2s->frame_clk_freq);
		return ret;
	}

	ret = aic3106_dai_regs(i2s, &asi_a, &asi_b);
	if (ret < 0) {
		return ret;
	}

	/*
	 * Datasheet footnote on register 0: output routing/volume registers should be
	 * reset by writing them directly rather than by software reset. That is what
	 * aic3106_setup_outputs() does right after the reset below.
	 */
	AIC3106_TRY(aic3106_soft_reset(dev));
	data->configured = false;
	data->output_on = false;
	data->input_on = false;

	AIC3106_TRY(aic3106_program_clocks(dev, codec_cfg->mclk_freq, fsref, (code & 1U) != 0U));

	/* Same rate for ADC and DAC (one shared word clock) */
	AIC3106_TRY(aic3106_write(dev, AIC3106_SRATE,
				  AIC3106_SRATE_ADC(code) | AIC3106_SRATE_DAC(code)));

	AIC3106_TRY(aic3106_write(dev, AIC3106_DATAPATH,
				  ((fsref == 44100U) ? AIC3106_DATAPATH_FSREF_44100 : 0) |
				  AIC3106_DATAPATH_LDAC_LEFT | AIC3106_DATAPATH_RDAC_RIGHT));

	AIC3106_TRY(aic3106_write(dev, AIC3106_ASI_A, asi_a));
	AIC3106_TRY(aic3106_write(dev, AIC3106_ASI_B, asi_b));
	/* Data offset 0: the codec's I2S mode already includes the one-bit delay (Table 10-18) */
	AIC3106_TRY(aic3106_write(dev, AIC3106_ASI_C, 0));

	AIC3106_TRY(aic3106_setup_outputs(dev));

	AIC3106_TRY(aic3106_write(dev, AIC3106_MICBIAS, AIC3106_MICBIAS_LEVEL(cfg->micbias)));

	data->configured = true;
	LOG_INF("configured: %u Hz, %u-bit, I2S codec is %s", i2s->frame_clk_freq, i2s->word_size,
		(asi_a & AIC3106_ASI_A_BCLK_OUT) ? "master" : "slave");
	return 0;
}

/* ------------------------------------------------------------------------ */
/* 5. Runtime control                                                       */
/* ------------------------------------------------------------------------ */

static int aic3106_start_output_locked(const struct device *dev)
{
	const struct aic3106_config *cfg = dev->config;
	struct aic3106_data *data = dev->data;
	uint8_t expect = AIC3106_PWR_STATUS_LDAC | AIC3106_PWR_STATUS_RDAC;
	int ret;

	if (!data->configured) {
		return -EPERM;
	}

	if (cfg->outputs & AIC3106_OUT_HEADPHONE) {
		expect |= AIC3106_PWR_STATUS_HPLOUT | AIC3106_PWR_STATUS_HPROUT;
	}
	if (cfg->outputs & AIC3106_OUT_LINEOUT) {
		expect |= AIC3106_PWR_STATUS_LEFT_LOP | AIC3106_PWR_STATUS_RIGHT_LOP;
	}

	AIC3106_TRY(aic3106_update(dev, AIC3106_DAC_PWR,
				   AIC3106_DAC_PWR_LEFT | AIC3106_DAC_PWR_RIGHT,
				   AIC3106_DAC_PWR_LEFT | AIC3106_DAC_PWR_RIGHT));
	AIC3106_TRY(aic3106_out_ctrl_update(dev, AIC3106_OUT_POWER, AIC3106_OUT_POWER));

	ret = aic3106_wait_status(dev, AIC3106_PWR_STATUS, expect, expect);
	if (ret == -ETIMEDOUT) {
		LOG_WRN("output stage not reported powered up (reg 94 mask 0x%02x)", expect);
	} else if (ret < 0) {
		return ret;
	}

	/* Unmute (unless the application asked for mute) */
	data->output_on = true;
	return aic3106_write_dac_vol(dev);
}

static int aic3106_stop_output_locked(const struct device *dev)
{
	const struct aic3106_config *cfg = dev->config;
	struct aic3106_data *data = dev->data;
	uint8_t status = AIC3106_PWR_STATUS_LDAC | AIC3106_PWR_STATUS_RDAC;
	int ret;

	if (!data->configured) {
		return -EPERM;
	}

	if (cfg->outputs & AIC3106_OUT_HEADPHONE) {
		status |= AIC3106_PWR_STATUS_HPLOUT | AIC3106_PWR_STATUS_HPROUT;
	}
	if (cfg->outputs & AIC3106_OUT_LINEOUT) {
		status |= AIC3106_PWR_STATUS_LEFT_LOP | AIC3106_PWR_STATUS_RIGHT_LOP;
	}

	/* Mute first (soft-stepped by the codec), then power down drivers, then DACs */
	data->output_on = false;
	AIC3106_TRY(aic3106_write_dac_vol(dev));
	AIC3106_TRY(aic3106_out_ctrl_update(dev, AIC3106_OUT_POWER, 0));
	AIC3106_TRY(aic3106_update(dev, AIC3106_DAC_PWR,
				   AIC3106_DAC_PWR_LEFT | AIC3106_DAC_PWR_RIGHT, 0));

	/* Section 10.3.3.3.4: keep MCLK running until the DAC reports power-down */
	ret = aic3106_wait_status(dev, AIC3106_PWR_STATUS, status, 0);
	if (ret == -ETIMEDOUT) {
		LOG_WRN("output stage did not report powered down");
		return 0;
	}
	return ret;
}

static void aic3106_start_output(const struct device *dev)
{
	struct aic3106_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = aic3106_start_output_locked(dev);
	k_mutex_unlock(&data->lock);

	if (ret < 0) {
		LOG_ERR("start_output failed (%d)", ret);
	}
}

static void aic3106_stop_output(const struct device *dev)
{
	struct aic3106_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = aic3106_stop_output_locked(dev);
	k_mutex_unlock(&data->lock);

	if (ret < 0) {
		LOG_ERR("stop_output failed (%d)", ret);
	}
}

static int aic3106_set_property(const struct device *dev, audio_property_t property,
				audio_channel_t channel, audio_property_value_t val)
{
	struct aic3106_data *data = dev->data;
	int first, last, ret = 0;

	switch (channel) {
	case AUDIO_CHANNEL_FRONT_LEFT:
		first = 0;
		last = 0;
		break;
	case AUDIO_CHANNEL_FRONT_RIGHT:
		first = 1;
		last = 1;
		break;
	case AUDIO_CHANNEL_ALL:
		first = 0;
		last = 1;
		break;
	default:
		LOG_ERR("channel %d not supported (front left/right/all only)", channel);
		return -EINVAL;
	}

	switch (property) {
	case AUDIO_PROPERTY_OUTPUT_VOLUME:
		/* API unit is 0.5 dB; the DAC digital volume covers 0 .. -63.5 dB */
		if (val.vol > 0 || val.vol < -AIC3106_DAC_VOL_MASK) {
			LOG_ERR("volume %d out of range (%d..0 in 0.5 dB steps)", val.vol,
				-AIC3106_DAC_VOL_MASK);
			return -EINVAL;
		}
		k_mutex_lock(&data->lock, K_FOREVER);
		for (int ch = first; ch <= last; ch++) {
			data->dac_vol[ch] = (uint8_t)(-val.vol);
		}
		if (data->configured) {
			ret = aic3106_write_dac_vol(dev);
		}
		k_mutex_unlock(&data->lock);
		return ret;

	case AUDIO_PROPERTY_OUTPUT_MUTE:
		k_mutex_lock(&data->lock, K_FOREVER);
		for (int ch = first; ch <= last; ch++) {
			data->dac_mute[ch] = val.mute;
		}
		if (data->configured) {
			ret = aic3106_write_dac_vol(dev);
		}
		k_mutex_unlock(&data->lock);
		return ret;

	default:
		return -EINVAL;
	}
}

static int aic3106_apply_properties(const struct device *dev)
{
	/* Every property is written immediately; nothing is cached for later */
	return 0;
}

static int aic3106_configure(const struct device *dev, struct audio_codec_cfg *cfg)
{
	struct aic3106_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = aic3106_configure_locked(dev, cfg);
	k_mutex_unlock(&data->lock);

	return ret;
}

/* ---- ADC / capture (driver-specific API, see include/zephyr/audio/tlv320aic3106.h) ---- */

/*
 * Write all six ADC input routing registers. Registers 19 and 22 also hold the
 * left/right ADC power bit, so routing and power are always written together.
 * Unselected inputs are disconnected (level code 0xF), which is the reset state.
 */
static int aic3106_write_adc_route(const struct device *dev, bool power)
{
	const struct aic3106_config *cfg = dev->config;
	uint8_t mic_l = 0xFF;		/* MIC3L/R -> left ADC  (reg 17) */
	uint8_t mic_r = 0xFF;		/* MIC3L/R -> right ADC (reg 18) */
	uint8_t l1l = AIC3106_LINE_NC;	/* reg 19: LINE1L -> left ADC,  D2 = left ADC power */
	uint8_t l2l = AIC3106_LINE_NC;	/* reg 20: LINE2L -> left ADC */
	uint8_t l1r_l = AIC3106_LINE_NC;/* reg 21: LINE1R -> left ADC */
	uint8_t l1r = AIC3106_LINE_NC;	/* reg 22: LINE1R -> right ADC, D2 = right ADC power */
	uint8_t l2r = AIC3106_LINE_NC;	/* reg 23: LINE2R -> right ADC */
	uint8_t l1l_r = AIC3106_LINE_NC;/* reg 24: LINE1L -> right ADC */

	switch (cfg->input) {
	case AIC3106_INPUT_LINE1:
		l1l = AIC3106_LINE_LEVEL_0DB;
		l1r = AIC3106_LINE_LEVEL_0DB;
		break;
	case AIC3106_INPUT_LINE2:
		l2l = AIC3106_LINE_LEVEL_0DB;
		l2r = AIC3106_LINE_LEVEL_0DB;
		break;
	case AIC3106_INPUT_MIC3:
		mic_l = 0x0F;		/* MIC3L 0 dB, MIC3R not connected */
		mic_r = 0xF0;		/* MIC3L not connected, MIC3R 0 dB */
		break;
	default:
		return -EINVAL;
	}

	if (power) {
		l1l |= AIC3106_ADC_POWER;
		l1r |= AIC3106_ADC_POWER;
	}

	AIC3106_TRY(aic3106_write(dev, AIC3106_MIC3_TO_LADC, mic_l));
	AIC3106_TRY(aic3106_write(dev, AIC3106_MIC3_TO_RADC, mic_r));
	AIC3106_TRY(aic3106_write(dev, AIC3106_LINE1L_TO_LADC, l1l));
	AIC3106_TRY(aic3106_write(dev, AIC3106_LINE2L_TO_LADC, l2l));
	AIC3106_TRY(aic3106_write(dev, AIC3106_LINE1R_TO_LADC, l1r_l));
	AIC3106_TRY(aic3106_write(dev, AIC3106_LINE1R_TO_RADC, l1r));
	AIC3106_TRY(aic3106_write(dev, AIC3106_LINE2R_TO_RADC, l2r));
	return aic3106_write(dev, AIC3106_LINE1L_TO_RADC, l1l_r);
}

/* ADC PGA gain + mute; muted whenever the input is stopped */
static int aic3106_write_adc_pga(const struct device *dev)
{
	struct aic3106_data *data = dev->data;
	uint8_t val = data->adc_gain;

	if (!data->input_on || data->adc_mute) {
		val |= AIC3106_ADC_PGA_MUTE;
	}

	AIC3106_TRY(aic3106_write(dev, AIC3106_ADC_PGA_L, val));
	return aic3106_write(dev, AIC3106_ADC_PGA_R, val);
}

int tlv320aic3106_start_input(const struct device *dev)
{
	struct aic3106_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);

	if (!data->configured) {
		ret = -EPERM;
		goto out;
	}

	ret = aic3106_write_adc_route(dev, true);
	if (ret < 0) {
		goto out;
	}

	data->input_on = true;
	ret = aic3106_write_adc_pga(dev);
	if (ret < 0) {
		goto out;
	}

	ret = aic3106_wait_status(dev, AIC3106_ADC_FLAG,
				  AIC3106_ADC_FLAG_LADC_ON | AIC3106_ADC_FLAG_RADC_ON,
				  AIC3106_ADC_FLAG_LADC_ON | AIC3106_ADC_FLAG_RADC_ON);
out:
	k_mutex_unlock(&data->lock);
	return ret;
}

int tlv320aic3106_stop_input(const struct device *dev)
{
	struct aic3106_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);

	if (!data->configured) {
		ret = -EPERM;
		goto out;
	}

	/* The PGA soft-steps to mute on power-down by itself; mute explicitly anyway */
	data->input_on = false;
	ret = aic3106_write_adc_pga(dev);
	if (ret < 0) {
		goto out;
	}
	ret = aic3106_write_adc_route(dev, false);
out:
	k_mutex_unlock(&data->lock);
	return ret;
}

int tlv320aic3106_set_input_gain(const struct device *dev, int gain_half_db)
{
	struct aic3106_data *data = dev->data;
	int ret = 0;

	if (gain_half_db < 0 || gain_half_db > TLV320AIC3106_INPUT_GAIN_MAX) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	data->adc_gain = (uint8_t)gain_half_db;
	if (data->configured) {
		ret = aic3106_write_adc_pga(dev);
	}
	k_mutex_unlock(&data->lock);

	return ret;
}

int tlv320aic3106_set_input_mute(const struct device *dev, bool mute)
{
	struct aic3106_data *data = dev->data;
	int ret = 0;

	k_mutex_lock(&data->lock, K_FOREVER);
	data->adc_mute = mute;
	if (data->configured) {
		ret = aic3106_write_adc_pga(dev);
	}
	k_mutex_unlock(&data->lock);

	return ret;
}

/* ------------------------------------------------------------------------ */
/* 6. Zephyr device                                                         */
/* ------------------------------------------------------------------------ */

/*
 * POST_KERNEL init: hardware reset (mandatory after power-up, section 10.3.1)
 * and presence check. The codec has no ID register, so "present" means the
 * I2C address ACKs; a reset-default value is checked only as a warning.
 */
static int aic3106_init(const struct device *dev)
{
	const struct aic3106_config *cfg = dev->config;
	struct aic3106_data *data = dev->data;
	uint8_t val;
	int ret;

	k_mutex_init(&data->lock);
	data->page = 0;

	if (!i2c_is_ready_dt(&cfg->bus)) {
		LOG_ERR("I2C bus not ready");
		return -ENODEV;
	}

    if (cfg->reset_gpio.port != NULL) {
        if (!gpio_is_ready_dt(&cfg->reset_gpio)) {
            LOG_ERR("reset GPIO not ready");
            return -ENODEV;
        }
        ret = gpio_pin_configure_dt(&cfg->reset_gpio, GPIO_OUTPUT_ACTIVE);
        if (ret < 0) {
            return ret;
        }
        k_msleep(1);
        ret = gpio_pin_set_dt(&cfg->reset_gpio, 0);
        if (ret < 0) {
            return ret;
        }
        k_msleep(1);
    }

	ret = aic3106_read(dev, AIC3106_PLL_A, &val);
	if (ret < 0) {
		LOG_ERR("no ACK from codec at 0x%02x - check I2C wiring, address pins and that "
			"reset-gpios is GPIO_ACTIVE_LOW", cfg->bus.addr);
		return -ENODEV;
	}

	if (val != AIC3106_PLL_A_RESET) {
		LOG_WRN("PLL A register is 0x%02x after reset, expected 0x%02x", val,
			AIC3106_PLL_A_RESET);
	}

	return 0;
}

static const struct audio_codec_api aic3106_api = {
	.configure = aic3106_configure,
	.start_output = aic3106_start_output,
	.stop_output = aic3106_stop_output,
	.set_property = aic3106_set_property,
	.apply_properties = aic3106_apply_properties,
	/* clear_errors / register_error_callback: not implemented (optional in 3.7.2) */
};

#define AIC3106_OUTPUTS(inst)								\
	((DT_INST_ENUM_IDX(inst, ti_output_select) == 0) ? AIC3106_OUT_HEADPHONE :	\
	 (DT_INST_ENUM_IDX(inst, ti_output_select) == 1) ? AIC3106_OUT_LINEOUT :	\
	 (AIC3106_OUT_HEADPHONE | AIC3106_OUT_LINEOUT))

/*
 * COND_CODE_1 selects at preprocessor time between the real GPIO spec and
 * a zero struct literal.  This avoids passing {0} as a macro argument, which
 * the C preprocessor cannot handle inside a static const initialiser.
 */
#define AIC3106_RESET_GPIO(inst)						\
	COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, reset_gpios),			\
		    (GPIO_DT_SPEC_INST_GET(inst, reset_gpios)),			\
		    ({0}))

#define AIC3106_INIT(inst)								\
	static const struct aic3106_config aic3106_config_##inst = {			\
		.bus        = I2C_DT_SPEC_INST_GET(inst),				\
		.reset_gpio = AIC3106_RESET_GPIO(inst),					\
		.outputs    = AIC3106_OUTPUTS(inst),					\
		.input      = DT_INST_ENUM_IDX(inst, ti_input_select),			\
		.micbias    = DT_INST_PROP(inst, ti_micbias),				\
		.output_cm  = DT_INST_PROP(inst, ti_output_cm),				\
		.hp_ac_coupled = DT_INST_PROP(inst, ti_hp_ac_coupled),			\
	};										\
	static struct aic3106_data aic3106_data_##inst;					\
	DEVICE_DT_INST_DEFINE(inst, aic3106_init, NULL, &aic3106_data_##inst,		\
			      &aic3106_config_##inst, POST_KERNEL,			\
			      CONFIG_AUDIO_CODEC_INIT_PRIORITY, &aic3106_api);

DT_INST_FOREACH_STATUS_OKAY(AIC3106_INIT)