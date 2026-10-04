/*
 * Copyright (c) 2022-2023 Circuit Valley
 * Copyright (c) 2024-2025 tinyVision.ai Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ported to Zephyr 3.7.2 video API from the latest upstream driver.
 *
 * Key changes vs. the upstream (latest) driver:
 *  - Replaced video_write_cci_*() / video_read_cci_reg() helpers with
 *    direct I2C transactions (not available in 3.7.2).
 *  - Replaced the video_ctrl framework (video_init_ctrl, video_init_menu_ctrl,
 *    video_init_int_menu_ctrl) with a plain per-driver struct; controls are
 *    now set/get through the legacy void-pointer set_ctrl/get_ctrl callbacks.
 *  - set_format / get_format / get_caps now accept the `ep` argument required
 *    by the 3.7.2 video_driver_api.
 *  - stream_start / stream_stop are two separate callbacks (no set_stream).
 *  - DEVICE_API(video, …) macro does not exist in 3.7.2; replaced with a
 *    plain static struct video_driver_api.
 *  - VIDEO_DEVICE_DEFINE does not exist in 3.7.2; removed.
 *  - set_frmival / get_frmival / enum_frmival are not part of 3.7.2's
 *    video_driver_api; fps selection is handled through the init path only.
 *  - VIDEO_PIX_FMT_SBGGR8 → VIDEO_PIX_FMT_BGGR8 (the only 8-bit Bayer
 *    format defined in 3.7.2's video.h).
 *  - VIDEO_PIX_FMT_SBGGR10P has no equivalent in 3.7.2; replaced with
 *    VIDEO_PIX_FMT_BGGR8 and a note – add your own fourcc if needed.
 *  - VIDEO_FOURCC_TO_STR not available; removed from log messages.
 */

#define DT_DRV_COMPAT sony_imx219

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/video.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#define LOG_LEVEL CONFIG_LOG_DEFAULT_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(imx219);

/* ---------------------------------------------------------------------------
 * Sensor constants
 * ---------------------------------------------------------------------------
 */
#define IMX219_FULL_WIDTH       3280
#define IMX219_FULL_HEIGHT      2464
#define IMX219_CHIP_ID          0x0219

/* Register addresses */
#define IMX219_REG_CHIP_ID_HI           0x0000
#define IMX219_REG_CHIP_ID_LO           0x0001
#define IMX219_REG_SOFTWARE_RESET       0x0103
#define IMX219_REG_MODE_SELECT          0x0100
#define IMX219_MODE_SELECT_STANDBY      0x00
#define IMX219_MODE_SELECT_STREAMING    0x01

#define IMX219_REG_ANALOG_GAIN          0x0157
#define IMX219_ANALOG_GAIN_MIN          0
#define IMX219_ANALOG_GAIN_MAX          232
#define IMX219_ANALOG_GAIN_DEFAULT      IMX219_ANALOG_GAIN_MAX

#define IMX219_REG_DIGITAL_GAIN_HI      0x0158
#define IMX219_REG_DIGITAL_GAIN_LO      0x0159
#define IMX219_DIGITAL_GAIN_DEFAULT     256

#define IMX219_REG_INTEGRATION_TIME_HI  0x015A
#define IMX219_REG_INTEGRATION_TIME_LO  0x015B
#define IMX219_INTEGRATION_TIME_DEFAULT 100

#define IMX219_REG_TESTPATTERN_HI       0x0600
#define IMX219_REG_TESTPATTERN_LO       0x0601
#define IMX219_REG_TP_WINDOW_WIDTH_HI   0x0624
#define IMX219_REG_TP_WINDOW_WIDTH_LO   0x0625
#define IMX219_REG_TP_WINDOW_HEIGHT_HI  0x0626
#define IMX219_REG_TP_WINDOW_HEIGHT_LO  0x0627

#define IMX219_REG_CSI_LANE_MODE        0x0114
#define IMX219_REG_DPHY_CTRL            0x0128
#define IMX219_REG_EXCK_FREQ_HI         0x012A
#define IMX219_REG_EXCK_FREQ_LO         0x012B

#define IMX219_REG_PREPLLCK_VT_DIV      0x0304
#define IMX219_REG_PREPLLCK_OP_DIV      0x0305
#define IMX219_REG_OPPXCK_DIV           0x0309
#define IMX219_REG_VTPXCK_DIV           0x0301
#define IMX219_REG_VTSYCK_DIV           0x0303
#define IMX219_REG_OPSYCK_DIV           0x030B
#define IMX219_REG_PLL_VT_MPY_HI        0x0306
#define IMX219_REG_PLL_VT_MPY_LO        0x0307
#define IMX219_REG_PLL_OP_MPY_HI        0x030C
#define IMX219_REG_PLL_OP_MPY_LO        0x030D

#define IMX219_REG_LINE_LENGTH_A_HI     0x0162
#define IMX219_REG_LINE_LENGTH_A_LO     0x0163
#define IMX219_REG_CSI_DATA_FORMAT_A0   0x018C
#define IMX219_REG_CSI_DATA_FORMAT_A1   0x018D

#define IMX219_REG_BINNING_MODE_H       0x0174
#define IMX219_REG_BINNING_MODE_V       0x0175
#define IMX219_REG_ORIENTATION          0x0172
#define IMX219_REG_FRM_LENGTH_A_HI      0x0160
#define IMX219_REG_FRM_LENGTH_A_LO      0x0161
#define IMX219_REG_X_ADD_STA_A_HI       0x0164
#define IMX219_REG_X_ADD_STA_A_LO       0x0165
#define IMX219_REG_X_ADD_END_A_HI       0x0166
#define IMX219_REG_X_ADD_END_A_LO       0x0167
#define IMX219_REG_Y_ADD_STA_A_HI       0x0168
#define IMX219_REG_Y_ADD_STA_A_LO       0x0169
#define IMX219_REG_Y_ADD_END_A_HI       0x016A
#define IMX219_REG_Y_ADD_END_A_LO       0x016B
#define IMX219_REG_X_OUTPUT_SIZE_HI     0x016C
#define IMX219_REG_X_OUTPUT_SIZE_LO     0x016D
#define IMX219_REG_Y_OUTPUT_SIZE_HI     0x016E
#define IMX219_REG_Y_OUTPUT_SIZE_LO     0x016F
#define IMX219_REG_X_ODD_INC_A          0x0170
#define IMX219_REG_Y_ODD_INC_A          0x0171
#define IMX219_REG_DT_PEDESTAL_HI       0xD1EA
#define IMX219_REG_DT_PEDESTAL_LO       0xD1EB
#define IMX219_DT_PEDESTAL_DEFAULT      40

/* ---------------------------------------------------------------------------
 * Pixel formats
 *
 * VIDEO_PIX_FMT_SBGGR8 was renamed from VIDEO_PIX_FMT_BGGR8 in later Zephyr
 * versions.  3.7.2 still ships it as VIDEO_PIX_FMT_BGGR8.
 *
 * There is no packed 10-bit Bayer (SBGGR10P) in 3.7.2.  If you need it,
 * define your own fourcc here and adjust the CSI data-format registers.
 * ---------------------------------------------------------------------------
 */
#ifndef VIDEO_PIX_FMT_SBGGR8
#define VIDEO_PIX_FMT_SBGGR8  VIDEO_PIX_FMT_BGGR8
#endif

/* Packed 10-bit – not in 3.7.2.  Fallback to 8-bit or define your own. */
#ifndef VIDEO_PIX_FMT_SBGGR10P
#define VIDEO_PIX_FMT_SBGGR10P \
	video_fourcc('p', 'B', 'A', 'A')  /* MIPI CSI-2 packed 10-bit BGGR */
#endif

/* ---------------------------------------------------------------------------
 * Control IDs not always present in 3.7.2 video-controls.h
 *
 * In Zephyr 3.7.2:
 *   - The camera class base is VIDEO_CTRL_CLASS_CAMERA (== 0x00010000).
 *     The upstream kernel names it VIDEO_CID_CAMERA_CLASS_BASE; that symbol
 *     does not exist in 3.7.2.
 *   - Exposure is VIDEO_CID_CAMERA_EXPOSURE, not VIDEO_CID_EXPOSURE.
 *   - Digital gain is VIDEO_CID_CAMERA_GAIN, not VIDEO_CID_GAIN.
 *   - Brightness is VIDEO_CID_CAMERA_BRIGHTNESS, not VIDEO_CID_BRIGHTNESS.
 *   - VIDEO_CID_TEST_PATTERN and VIDEO_CID_ANALOGUE_GAIN are absent entirely.
 * ---------------------------------------------------------------------------
 */

/* Map the upstream name to what 3.7.2 actually provides */
#ifndef VIDEO_CID_CAMERA_CLASS_BASE
#define VIDEO_CID_CAMERA_CLASS_BASE     VIDEO_CTRL_CLASS_CAMERA
#endif

/* Exposure: upstream VIDEO_CID_EXPOSURE → 3.7.2 VIDEO_CID_CAMERA_EXPOSURE */
#ifndef VIDEO_CID_EXPOSURE
#define VIDEO_CID_EXPOSURE              VIDEO_CID_CAMERA_EXPOSURE
#endif

/* Brightness: upstream VIDEO_CID_BRIGHTNESS → 3.7.2 VIDEO_CID_CAMERA_BRIGHTNESS */
#ifndef VIDEO_CID_BRIGHTNESS
#define VIDEO_CID_BRIGHTNESS            VIDEO_CID_CAMERA_BRIGHTNESS
#endif

/* Digital gain: upstream VIDEO_CID_GAIN → 3.7.2 VIDEO_CID_CAMERA_GAIN */
#ifndef VIDEO_CID_GAIN
#define VIDEO_CID_GAIN                  VIDEO_CID_CAMERA_GAIN
#endif

/* Analogue gain: not in 3.7.2 at all, define at slot +21 (matches V4L2) */
#ifndef VIDEO_CID_ANALOGUE_GAIN
#ifdef VIDEO_CID_ANALOG_GAIN
#define VIDEO_CID_ANALOGUE_GAIN         VIDEO_CID_ANALOG_GAIN
#else
#define VIDEO_CID_ANALOGUE_GAIN         (VIDEO_CID_CAMERA_CLASS_BASE + 21)
#endif
#endif

/* Digital gain extended: slot +22 */
#ifndef VIDEO_CID_DIGITAL_GAIN
#define VIDEO_CID_DIGITAL_GAIN          (VIDEO_CID_CAMERA_CLASS_BASE + 22)
#endif

/* Test-pattern: not in 3.7.2, define at slot +23 (no conflict) */
#ifndef VIDEO_CID_TEST_PATTERN
#define VIDEO_CID_TEST_PATTERN          (VIDEO_CID_CAMERA_CLASS_BASE + 23)
#endif

/* ---------------------------------------------------------------------------
 * Low-level I2C helpers (replace video_write_cci_* from video_common.h)
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Write a single 8-bit register (16-bit address).
 */
static int imx219_write_reg8(const struct i2c_dt_spec *i2c,
			     uint16_t addr, uint8_t val)
{
	uint8_t buf[3] = {
		(uint8_t)(addr >> 8),
		(uint8_t)(addr & 0xFF),
		val,
	};

	return i2c_write_dt(i2c, buf, sizeof(buf));
}

/**
 * @brief Write a 16-bit register (16-bit address, big-endian value).
 */
static int imx219_write_reg16(const struct i2c_dt_spec *i2c,
			      uint16_t addr, uint16_t val)
{
	uint8_t buf[4] = {
		(uint8_t)(addr >> 8),
		(uint8_t)(addr & 0xFF),
		(uint8_t)(val >> 8),
		(uint8_t)(val & 0xFF),
	};

	return i2c_write_dt(i2c, buf, sizeof(buf));
}

/**
 * @brief Read a single 8-bit register (16-bit address).
 */
static int __maybe_unused imx219_read_reg8(const struct i2c_dt_spec *i2c,
					   uint16_t addr, uint8_t *val)
{
	uint8_t addr_buf[2] = {
		(uint8_t)(addr >> 8),
		(uint8_t)(addr & 0xFF),
	};
	int ret;

	ret = i2c_write_dt(i2c, addr_buf, sizeof(addr_buf));
	if (ret < 0) {
		return ret;
	}

	return i2c_read_dt(i2c, val, 1);
}

/**
 * @brief Read a 16-bit register (16-bit address, big-endian value).
 */
static int imx219_read_reg16(const struct i2c_dt_spec *i2c,
			     uint16_t addr, uint16_t *val)
{
	uint8_t addr_buf[2] = {
		(uint8_t)(addr >> 8),
		(uint8_t)(addr & 0xFF),
	};
	uint8_t data[2];
	int ret;

	ret = i2c_write_dt(i2c, addr_buf, sizeof(addr_buf));
	if (ret < 0) {
		return ret;
	}

	ret = i2c_read_dt(i2c, data, sizeof(data));
	if (ret < 0) {
		return ret;
	}

	*val = ((uint16_t)data[0] << 8) | data[1];

	return 0;
}

/* ---------------------------------------------------------------------------
 * Structures
 * ---------------------------------------------------------------------------
 */

/** Per-driver control values (replaces the video_ctrl framework). */
struct imx219_ctrls {
	uint32_t exposure;       /* VIDEO_CID_EXPOSURE (→ VIDEO_CID_CAMERA_EXPOSURE)    */
	uint32_t brightness;     /* VIDEO_CID_BRIGHTNESS (→ VIDEO_CID_CAMERA_BRIGHTNESS) */
	uint32_t analogue_gain;  /* VIDEO_CID_ANALOGUE_GAIN                             */
	uint32_t digital_gain;   /* VIDEO_CID_GAIN (→ VIDEO_CID_CAMERA_GAIN)            */
	uint32_t test_pattern;   /* VIDEO_CID_TEST_PATTERN                              */
};

struct imx219_data {
	struct imx219_ctrls ctrls;
	struct video_format fmt;
	int fps; /* current frame-rate */
};

struct imx219_config {
	struct i2c_dt_spec i2c;
	uint32_t input_clk_hz;
};

/* ---------------------------------------------------------------------------
 * Register tables
 * ---------------------------------------------------------------------------
 */

/* Undocumented vendor registers (written as 8-bit via addr:data pairs). */
struct reg16_val8 {
	uint16_t addr;
	uint8_t  val;
};

static const struct reg16_val8 imx219_vendor_regs[] = {
	/* Enable access to registers 0x3000-0x5fff */
	{0x30eb, 0x05},
	{0x30eb, 0x0c},
	{0x300a, 0xff},
	{0x300b, 0xff},
	{0x30eb, 0x05},
	{0x30eb, 0x09},

	/* Extra undocumented registers */
	{0x455e, 0x00},
	{0x471e, 0x4b},
	{0x4767, 0x0f},
	{0x4750, 0x14},
	{0x4540, 0x00},
	{0x47b4, 0x14},
	{0x4713, 0x30},
	{0x478b, 0x10},
	{0x478f, 0x10},
	{0x4793, 0x10},
	{0x4797, 0x0e},
	{0x479b, 0x0e},
};

static int imx219_write_vendor_regs(const struct i2c_dt_spec *i2c,
				    const struct reg16_val8 *regs, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		int ret = imx219_write_reg8(i2c, regs[i].addr, regs[i].val);

		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}

/**
 * @brief Write the format-independent initialisation registers.
 */
static int imx219_write_init_regs(const struct i2c_dt_spec *i2c)
{
	int ret;

	/* MIPI: 2-lane, auto DPHY timing */
	ret = imx219_write_reg8(i2c, IMX219_REG_CSI_LANE_MODE, 0x01);
	if (ret < 0) { return ret; }

	ret = imx219_write_reg8(i2c, IMX219_REG_DPHY_CTRL, 0x00);
	if (ret < 0) { return ret; }

	/* Timing */
	ret = imx219_write_reg16(i2c, IMX219_REG_LINE_LENGTH_A_HI, 3448);
	if (ret < 0) { return ret; }

	ret = imx219_write_reg8(i2c, IMX219_REG_X_ODD_INC_A, 1);
	if (ret < 0) { return ret; }

	ret = imx219_write_reg8(i2c, IMX219_REG_Y_ODD_INC_A, 1);
	if (ret < 0) { return ret; }

	/* No binning */
	ret = imx219_write_reg8(i2c, IMX219_REG_BINNING_MODE_H, 0x00);
	if (ret < 0) { return ret; }

	ret = imx219_write_reg8(i2c, IMX219_REG_BINNING_MODE_V, 0x00);
	if (ret < 0) { return ret; }

	/* Default gains / exposure */
	ret = imx219_write_reg16(i2c, IMX219_REG_DIGITAL_GAIN_HI,
				 IMX219_DIGITAL_GAIN_DEFAULT);
	if (ret < 0) { return ret; }

	ret = imx219_write_reg8(i2c, IMX219_REG_ANALOG_GAIN,
				IMX219_ANALOG_GAIN_DEFAULT);
	if (ret < 0) { return ret; }

	return imx219_write_reg16(i2c, IMX219_REG_INTEGRATION_TIME_HI,
				  IMX219_INTEGRATION_TIME_DEFAULT);
}

/**
 * @brief Write PLL/timing registers for 30 fps.
 *
 * Tuned for 1920x1080 cropped resolution.
 */
static int imx219_write_fps30_regs(const struct i2c_dt_spec *i2c)
{
	int ret;

	ret = imx219_write_reg8(i2c, IMX219_REG_PREPLLCK_VT_DIV, 0x03);
	if (ret < 0) { return ret; }
	ret = imx219_write_reg8(i2c, IMX219_REG_PREPLLCK_OP_DIV, 0x03);
	if (ret < 0) { return ret; }
	ret = imx219_write_reg8(i2c, IMX219_REG_VTPXCK_DIV, 4);
	if (ret < 0) { return ret; }
	ret = imx219_write_reg8(i2c, IMX219_REG_VTSYCK_DIV, 1);
	if (ret < 0) { return ret; }
	ret = imx219_write_reg8(i2c, IMX219_REG_OPPXCK_DIV, 10);
	if (ret < 0) { return ret; }
	ret = imx219_write_reg8(i2c, IMX219_REG_OPSYCK_DIV, 1);
	if (ret < 0) { return ret; }
	ret = imx219_write_reg16(i2c, IMX219_REG_PLL_VT_MPY_HI, 30);
	if (ret < 0) { return ret; }
	return imx219_write_reg16(i2c, IMX219_REG_PLL_OP_MPY_HI, 50);
}

/**
 * @brief Write PLL/timing registers for 15 fps.
 *
 * Tuned for full-resolution 3280x2464 readout.
 */
static int __maybe_unused imx219_write_fps15_regs(const struct i2c_dt_spec *i2c)
{
	int ret;

	ret = imx219_write_reg8(i2c, IMX219_REG_PREPLLCK_VT_DIV, 0x03);
	if (ret < 0) { return ret; }
	ret = imx219_write_reg8(i2c, IMX219_REG_PREPLLCK_OP_DIV, 0x03);
	if (ret < 0) { return ret; }
	ret = imx219_write_reg8(i2c, IMX219_REG_VTPXCK_DIV, 4);
	if (ret < 0) { return ret; }
	ret = imx219_write_reg8(i2c, IMX219_REG_VTSYCK_DIV, 1);
	if (ret < 0) { return ret; }
	ret = imx219_write_reg8(i2c, IMX219_REG_OPPXCK_DIV, 10);
	if (ret < 0) { return ret; }
	ret = imx219_write_reg8(i2c, IMX219_REG_OPSYCK_DIV, 1);
	if (ret < 0) { return ret; }
	ret = imx219_write_reg16(i2c, IMX219_REG_PLL_VT_MPY_HI, 15);
	if (ret < 0) { return ret; }
	return imx219_write_reg16(i2c, IMX219_REG_PLL_OP_MPY_HI, 50);
}

/* ---------------------------------------------------------------------------
 * Supported format capabilities
 * ---------------------------------------------------------------------------
 */
enum {
	IMX219_RAW8_FULL_FRAME,
	IMX219_RAW10_FULL_FRAME,
};

static const struct video_format_cap imx219_fmts[] = {
	[IMX219_RAW8_FULL_FRAME] = {
		.pixelformat = VIDEO_PIX_FMT_SBGGR8,
		.width_min   = 4, .width_max  = IMX219_FULL_WIDTH,  .width_step  = 4,
		.height_min  = 4, .height_max = IMX219_FULL_HEIGHT, .height_step = 4,
	},
	[IMX219_RAW10_FULL_FRAME] = {
		.pixelformat = VIDEO_PIX_FMT_SBGGR10P,
		.width_min   = 4, .width_max  = IMX219_FULL_WIDTH,  .width_step  = 4,
		.height_min  = 4, .height_max = IMX219_FULL_HEIGHT, .height_step = 4,
	},
	{0},
};

/* ---------------------------------------------------------------------------
 * Helper: find a matching format capability index
 * ---------------------------------------------------------------------------
 */
static int imx219_find_fmt_idx(const struct video_format *fmt, size_t *out_idx)
{
	for (size_t i = 0; imx219_fmts[i].pixelformat != 0; i++) {
		const struct video_format_cap *cap = &imx219_fmts[i];

		if (fmt->pixelformat != cap->pixelformat) {
			continue;
		}
		if (fmt->width < cap->width_min || fmt->width > cap->width_max) {
			continue;
		}
		if (fmt->height < cap->height_min || fmt->height > cap->height_max) {
			continue;
		}

		/* Check step alignment (allow exact min as well) */
		if (fmt->width != cap->width_min &&
		    (fmt->width - cap->width_min) % cap->width_step != 0) {
			continue;
		}
		if (fmt->height != cap->height_min &&
		    (fmt->height - cap->height_min) % cap->height_step != 0) {
			continue;
		}

		*out_idx = i;
		return 0;
	}

	return -ENOTSUP;
}

/* ---------------------------------------------------------------------------
 * video_driver_api callbacks
 *
 * All callbacks match the 3.7.2 video_driver_api typedef signatures.
 * ---------------------------------------------------------------------------
 */

/**
 * set_format — ep argument required by 3.7.2 API (ignored; sensor has one port)
 */
static int imx219_set_fmt(const struct device *dev,
			  enum video_endpoint_id ep,
			  struct video_format *fmt)
{
	const struct imx219_config *cfg = dev->config;
	struct imx219_data *drv_data = dev->data;
	size_t idx;
	int ret;

	ARG_UNUSED(ep);

	ret = imx219_find_fmt_idx(fmt, &idx);
	if (ret < 0) {
		LOG_ERR("Format %ux%u (pixfmt 0x%08x) not supported",
			fmt->width, fmt->height, fmt->pixelformat);
		return -ENOTSUP;
	}

	/* Select RAW8 or RAW10 CSI data format */
	switch (idx) {
	case IMX219_RAW8_FULL_FRAME:
		ret = imx219_write_reg8(&cfg->i2c,
					IMX219_REG_CSI_DATA_FORMAT_A0, 8);
		if (ret < 0) { return ret; }
		ret = imx219_write_reg8(&cfg->i2c,
					IMX219_REG_CSI_DATA_FORMAT_A1, 8);
		break;
	case IMX219_RAW10_FULL_FRAME:
		ret = imx219_write_reg8(&cfg->i2c,
					IMX219_REG_CSI_DATA_FORMAT_A0, 10);
		if (ret < 0) { return ret; }
		ret = imx219_write_reg8(&cfg->i2c,
					IMX219_REG_CSI_DATA_FORMAT_A1, 10);
		break;
	default:
		return -EINVAL;
	}
	if (ret < 0) {
		return ret;
	}

	/* Centred crop window */
	uint16_t x_sta = (IMX219_FULL_WIDTH  - fmt->width)  / 2;
	uint16_t x_end = (IMX219_FULL_WIDTH  + fmt->width)  / 2 - 1;
	uint16_t y_sta = (IMX219_FULL_HEIGHT - fmt->height) / 2;
	uint16_t y_end = (IMX219_FULL_HEIGHT + fmt->height) / 2 - 1;

	ret = imx219_write_reg16(&cfg->i2c, IMX219_REG_X_ADD_STA_A_HI, x_sta);
	if (ret < 0) { return ret; }
	ret = imx219_write_reg16(&cfg->i2c, IMX219_REG_X_ADD_END_A_HI, x_end);
	if (ret < 0) { return ret; }
	ret = imx219_write_reg16(&cfg->i2c, IMX219_REG_Y_ADD_STA_A_HI, y_sta);
	if (ret < 0) { return ret; }
	ret = imx219_write_reg16(&cfg->i2c, IMX219_REG_Y_ADD_END_A_HI, y_end);
	if (ret < 0) { return ret; }

	/* Output size */
	ret = imx219_write_reg16(&cfg->i2c, IMX219_REG_X_OUTPUT_SIZE_HI, fmt->width);
	if (ret < 0) { return ret; }
	ret = imx219_write_reg16(&cfg->i2c, IMX219_REG_Y_OUTPUT_SIZE_HI, fmt->height);
	if (ret < 0) { return ret; }

	/* Frame length (with headroom) */
	ret = imx219_write_reg16(&cfg->i2c, IMX219_REG_FRM_LENGTH_A_HI,
				 fmt->height + 20);
	if (ret < 0) { return ret; }

	/* Test-pattern window */
	ret = imx219_write_reg16(&cfg->i2c, IMX219_REG_TP_WINDOW_WIDTH_HI, fmt->width);
	if (ret < 0) { return ret; }
	ret = imx219_write_reg16(&cfg->i2c, IMX219_REG_TP_WINDOW_HEIGHT_HI, fmt->height);
	if (ret < 0) { return ret; }

	/* Pitch = width × bytes-per-pixel (1 for RAW8, ~1.25 for RAW10P) */
	fmt->pitch = (idx == IMX219_RAW8_FULL_FRAME)
		     ? fmt->width
		     : (fmt->width * 10 / 8); /* packed */

	drv_data->fmt = *fmt;

	return 0;
}

/**
 * get_format — ep argument required by 3.7.2 API.
 *
 * Returns -ENODATA if the format was never successfully set (e.g. I2C
 * failure during init), giving the CSI host a clear error to propagate
 * rather than a silently zeroed struct.
 */
static int imx219_get_fmt(const struct device *dev,
			  enum video_endpoint_id ep,
			  struct video_format *fmt)
{
	struct imx219_data *drv_data = dev->data;

	ARG_UNUSED(ep);

	if (drv_data->fmt.pixelformat == 0) {
		LOG_ERR("Format not initialized");
		return -ENODATA;
	}

	*fmt = drv_data->fmt;

	return 0;
}

/**
 * get_caps — ep argument required by 3.7.2 API
 */
static int imx219_get_caps(const struct device *dev,
			   enum video_endpoint_id ep,
			   struct video_caps *caps)
{
	ARG_UNUSED(ep);

	caps->format_caps = imx219_fmts;

	return 0;
}

/**
 * stream_start — replaces set_stream(dev, true, …)
 */
static int imx219_stream_start(const struct device *dev)
{
	const struct imx219_config *cfg = dev->config;

	return imx219_write_reg8(&cfg->i2c, IMX219_REG_MODE_SELECT,
				 IMX219_MODE_SELECT_STREAMING);
}

/**
 * stream_stop — replaces set_stream(dev, false, …)
 */
static int imx219_stream_stop(const struct device *dev)
{
	const struct imx219_config *cfg = dev->config;

	return imx219_write_reg8(&cfg->i2c, IMX219_REG_MODE_SELECT,
				 IMX219_MODE_SELECT_STANDBY);
}

/**
 * set_ctrl — 3.7.2 passes the new value through `void *value`.
 *
 * The caller is responsible for casting to the correct type.  For integer
 * controls this is typically `int32_t *`.
 */
static int imx219_set_ctrl(const struct device *dev,
			   unsigned int cid,
			   void *value)
{
	const struct imx219_config *cfg = dev->config;
	struct imx219_data *drv_data = dev->data;
	struct imx219_ctrls *ctrls = &drv_data->ctrls;
	int32_t val = *(int32_t *)value;
	int ret;

	switch (cid) {
	case VIDEO_CID_EXPOSURE:
		ctrls->exposure = (uint32_t)val;
		return imx219_write_reg16(&cfg->i2c,
					  IMX219_REG_INTEGRATION_TIME_HI,
					  ctrls->exposure);

	case VIDEO_CID_ANALOGUE_GAIN:
		ctrls->analogue_gain = (uint32_t)val;
		return imx219_write_reg8(&cfg->i2c,
					 IMX219_REG_ANALOG_GAIN,
					 (uint8_t)ctrls->analogue_gain);

	case VIDEO_CID_GAIN:
		ctrls->digital_gain = (uint32_t)val;
		return imx219_write_reg16(&cfg->i2c,
					  IMX219_REG_DIGITAL_GAIN_HI,
					  (uint16_t)ctrls->digital_gain);

	case VIDEO_CID_BRIGHTNESS:
		ctrls->brightness = (uint32_t)val;
		return imx219_write_reg16(&cfg->i2c,
					  IMX219_REG_DT_PEDESTAL_HI,
					  (uint16_t)ctrls->brightness);

	case VIDEO_CID_TEST_PATTERN:
		ctrls->test_pattern = (uint32_t)val;
		return imx219_write_reg16(&cfg->i2c,
					  IMX219_REG_TESTPATTERN_HI,
					  (uint16_t)ctrls->test_pattern);

	default:
		LOG_WRN("Control 0x%x not supported", cid);
		return -ENOTSUP;
	}

	ARG_UNUSED(ret);
}

/**
 * get_ctrl — returns the cached software value (no register read-back).
 */
static int imx219_get_ctrl(const struct device *dev,
			   unsigned int cid,
			   void *value)
{
	struct imx219_data *drv_data = dev->data;
	struct imx219_ctrls *ctrls = &drv_data->ctrls;
	int32_t *out = (int32_t *)value;

	switch (cid) {
	case VIDEO_CID_EXPOSURE:
		*out = (int32_t)ctrls->exposure;
		break;
	case VIDEO_CID_ANALOGUE_GAIN:
		*out = (int32_t)ctrls->analogue_gain;
		break;
	case VIDEO_CID_GAIN:
		*out = (int32_t)ctrls->digital_gain;
		break;
	case VIDEO_CID_BRIGHTNESS:
		*out = (int32_t)ctrls->brightness;
		break;
	case VIDEO_CID_TEST_PATTERN:
		*out = (int32_t)ctrls->test_pattern;
		break;
	default:
		LOG_WRN("Control 0x%x not supported", cid);
		return -ENOTSUP;
	}

	return 0;
}

/* ---------------------------------------------------------------------------
 * Driver API table  (plain struct — DEVICE_API macro not available in 3.7.2)
 * ---------------------------------------------------------------------------
 */
static const struct video_driver_api imx219_driver_api = {
	.set_format   = imx219_set_fmt,
	.get_format   = imx219_get_fmt,
	.get_caps     = imx219_get_caps,
	.stream_start = imx219_stream_start,
	.stream_stop  = imx219_stream_stop,
	.set_ctrl     = imx219_set_ctrl,
	.get_ctrl     = imx219_get_ctrl,
	/* enqueue / dequeue / flush / set_signal — not used by a raw sensor */
};

/* ---------------------------------------------------------------------------
 * Sensor-level helpers
 * ---------------------------------------------------------------------------
 */

static int imx219_set_input_clk(const struct device *dev, uint32_t rate_hz)
{
	const struct imx219_config *cfg = dev->config;

	if (rate_hz < MHZ(6) || rate_hz > MHZ(27) || rate_hz % MHZ(1) != 0) {
		LOG_ERR("Unsupported INCK freq (%u Hz)", rate_hz);
		return -EINVAL;
	}

	/* EXCK_FREQ is in MHz with an 8-bit fractional part */
	return imx219_write_reg16(&cfg->i2c, IMX219_REG_EXCK_FREQ_HI,
				  (uint16_t)((rate_hz / MHZ(1)) << 8));
}

/* ---------------------------------------------------------------------------
 * Initialisation
 * ---------------------------------------------------------------------------
 */
static int imx219_init(const struct device *dev)
{
	const struct imx219_config *cfg = dev->config;
	struct imx219_data *drv_data = dev->data;

	/*
	 * Default format: 1920x1080 RAW8.
	 *
	 * This is the format the CSI host and main.c will see when they call
	 * video_get_format() before any explicit video_set_format() call.
	 * Using a real, usable resolution here is important: the original
	 * width_min/height_min (4x4) results in a zeroed/rejected format on
	 * the CSI side, causing "Unable to retrieve video format".
	 */
	struct video_format fmt = {
		.pixelformat = VIDEO_PIX_FMT_SBGGR8,
		.width       = 1920,
		.height      = 1080,
	};

	uint16_t chip_id;
	int ret;

	if (!device_is_ready(cfg->i2c.bus)) {
		LOG_ERR("I2C device %s is not ready", cfg->i2c.bus->name);
		return -ENODEV;
	}

	k_sleep(K_MSEC(1));

	/* Software reset */
	ret = imx219_write_reg8(&cfg->i2c, IMX219_REG_SOFTWARE_RESET, 1);
	if (ret < 0) {
		return ret;
	}

	k_sleep(K_MSEC(6)); /* t5 */

	/* Verify chip ID */
	ret = imx219_read_reg16(&cfg->i2c, IMX219_REG_CHIP_ID_HI, &chip_id);
	if (ret < 0) {
		return ret;
	}

	if (chip_id != IMX219_CHIP_ID) {
		LOG_ERR("Wrong chip ID 0x%04x (expected 0x%04x)",
			chip_id, IMX219_CHIP_ID);
		return -ENODEV;
	}

	/* Set external clock frequency */
	ret = imx219_set_input_clk(dev, cfg->input_clk_hz);
	if (ret < 0) {
		return ret;
	}

	/* Unlock vendor registers and write undocumented init sequence */
	ret = imx219_write_vendor_regs(&cfg->i2c, imx219_vendor_regs,
				       ARRAY_SIZE(imx219_vendor_regs));
	if (ret < 0) {
		return ret;
	}

	/* Common init registers */
	ret = imx219_write_init_regs(&cfg->i2c);
	if (ret < 0) {
		return ret;
	}

	/* Apply default 1920x1080 format — this populates drv_data->fmt */
	ret = imx219_set_fmt(dev, VIDEO_EP_OUT, &fmt);
	if (ret < 0) {
		LOG_ERR("Failed to set Formate: %d", ret);
		return ret;
	}

	/*
	 * Default to 30 fps, which matches the 1920x1080 crop window.
	 * (The 15 fps PLL config is tuned for full 3280x2464 readout.)
	 */
	ret = imx219_write_fps30_regs(&cfg->i2c);
	if (ret < 0) {
		LOG_ERR("Failed to set fps: %d", ret);
		return ret;
	}

	drv_data->fps = 30;

	/* Initialise software control cache */
	drv_data->ctrls.exposure      = IMX219_INTEGRATION_TIME_DEFAULT;
	drv_data->ctrls.brightness    = IMX219_DT_PEDESTAL_DEFAULT;
	drv_data->ctrls.analogue_gain = IMX219_ANALOG_GAIN_DEFAULT;
	drv_data->ctrls.digital_gain  = IMX219_DIGITAL_GAIN_DEFAULT;
	drv_data->ctrls.test_pattern  = 0; /* off */

	return 0;
}

/* ---------------------------------------------------------------------------
 * Per-instance macro
 *
 * VIDEO_DEVICE_DEFINE is not available in 3.7.2; the sensor is registered
 * solely through DEVICE_DT_INST_DEFINE.  The host controller (e.g. a CSI-2
 * bridge driver) discovers it by DT phandle.
 * ---------------------------------------------------------------------------
 */
#define IMX219_INIT(n)                                                        \
	static struct imx219_data imx219_data_##n;                            \
                                                                              \
	static const struct imx219_config imx219_cfg_##n = {                  \
		.i2c = I2C_DT_SPEC_INST_GET(n),                               \
		.input_clk_hz =                                                \
			DT_INST_PROP_BY_PHANDLE(n, clocks, clock_frequency),  \
	};                                                                    \
                                                                              \
	DEVICE_DT_INST_DEFINE(n, &imx219_init, NULL,                          \
			      &imx219_data_##n, &imx219_cfg_##n,              \
			      POST_KERNEL, CONFIG_VIDEO_INIT_PRIORITY,        \
			      &imx219_driver_api);

DT_INST_FOREACH_STATUS_OKAY(IMX219_INIT)