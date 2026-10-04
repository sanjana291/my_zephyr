/*
 * SX1278 FSK/GFSK Driver Implementation
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */

#define DT_DRV_COMPAT calixto_sx127x_fsk

#include <zephyr/kernel.h>
#include <zephyr/sys_clock.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "drivers/fsk.h"
#include "sx127x_reg.h"

LOG_MODULE_REGISTER(sx1278_fsk, CONFIG_FSK_LOG_LEVEL);


/*
 * Frame layout (on-air, after the 1-byte variable-length prefix):
 *   [Dest(1)][Src(1)][payload...][Seq(1)]
 *
 * The SX1278 FIFO is 64 bytes.  In variable-length packet mode the chip
 * prepends one length byte, so the maximum on-air frame is 63 bytes:
 *   FIFO(64) - 1 length prefix = 63 on-air bytes
 *   63 - OVERHEAD(3) = 60 bytes of application payload.
 *
 * No streaming fill/drain is needed: every frame fits in a single FIFO load.
 */
#define SX1278_FRAME_HDR_LEN      2U    /* Dest + Src                          */
#define SX1278_FRAME_TRL_LEN      1U    /* Seq                                 */
#define SX1278_FRAME_OVERHEAD     (SX1278_FRAME_HDR_LEN + SX1278_FRAME_TRL_LEN) /* 3 */

/* Maximum on-air frame = FIFO(64) - 1 length prefix = 63 bytes.             */
#define SX1278_FRAME_MAX_ON_AIR   (SX1278_FIFO_SIZE - 1U)                      /* 63 */

/* Maximum application payload inside one frame.                              */
#define SX1278_FRAME_MAX_PAYLOAD  (SX1278_FRAME_MAX_ON_AIR - SX1278_FRAME_OVERHEAD) /* 60 */


#define SX1278_RX_THREAD_STACK_SIZE  4096
#define SX1278_RX_THREAD_PRIORITY    K_PRIO_COOP(7)

#define SX1278_RX_POLL_SLICE_MS       20

#define SX1278_DEDUP_TABLE_SIZE  2   /* must be a power of two */

struct sx1278_dedup_entry {
    uint8_t src;
    uint8_t seq;
    bool    valid;
};

struct sx1278_fsk_config {
    struct spi_dt_spec spi;
    struct gpio_dt_spec reset_gpio;
    struct gpio_dt_spec dio0_gpio;   /* PayloadReady / PacketSent */
    struct gpio_dt_spec dio1_gpio;   /* FifoLevel (optional)      */
};

struct sx1278_fsk_data {
    struct k_mutex lock;
    struct k_sem   irq_sem;          /* posted by DIO0 ISR            */

    struct gpio_callback dio0_cb;
    struct gpio_callback dio1_cb;

    /* Stored config for re-use */
    struct fsk_config cfg;
    bool configured;

    /* Current radio state */
    uint8_t op_mode;                 /* last written [2:0] mode bits  */

    /*-----------------------------------------------------------------
     * Reliable-delivery / callback-RX layer (see framing block above)
     *-----------------------------------------------------------------*/
    fsk_rx_callback_t rx_callback;
    void *rx_user_data;

    /* Internal always-on receive thread */
    struct k_thread rx_thread;
    K_KERNEL_STACK_MEMBER(rx_thread_stack, SX1278_RX_THREAD_STACK_SIZE);

    atomic_t tx_yield_request;

    uint8_t tx_seq;                  /* next outgoing DATA frame Seq (uint8_t, wraps 0xFF->0x00) */

    struct sx1278_dedup_entry dedup_table[SX1278_DEDUP_TABLE_SIZE];
    uint8_t                   dedup_next;
};

/*---------------------------------------------------------------------------
 * Low-level SPI helpers
 *---------------------------------------------------------------------------*/

static int sx1278_read_reg(const struct device *dev, uint8_t reg,
                           uint8_t *val)
{
    const struct sx1278_fsk_config *cfg = dev->config;
    uint8_t tx_buf[2] = { reg & SX1278_SPI_READ_MASK, 0x00 };
    uint8_t rx_buf[2] = { 0 };

    struct spi_buf tx = { .buf = tx_buf, .len = 2 };
    struct spi_buf rx = { .buf = rx_buf, .len = 2 };
    struct spi_buf_set tx_set = { .buffers = &tx, .count = 1 };
    struct spi_buf_set rx_set = { .buffers = &rx, .count = 1 };

    int ret = spi_transceive_dt(&cfg->spi, &tx_set, &rx_set);
    if (ret == 0) {
        *val = rx_buf[1];
    }

    return ret;
}

static int sx1278_write_reg(const struct device *dev, uint8_t reg,
                            uint8_t val)
{
    const struct sx1278_fsk_config *cfg = dev->config;
    uint8_t buf[2] = { reg | SX1278_SPI_WRITE_BIT, val };

    struct spi_buf tx = { .buf = buf, .len = 2 };
    struct spi_buf_set tx_set = { .buffers = &tx, .count = 1 };

    int ret = spi_write_dt(&cfg->spi, &tx_set);

    return ret;
}

static int sx1278_write_fifo(const struct device *dev,
                             const uint8_t *data, uint8_t len)
{
    const struct sx1278_fsk_config *cfg = dev->config;

    if (len > SX1278_FIFO_SIZE) {
        return -EINVAL;
    }

    uint8_t tx_buf[SX1278_FIFO_SIZE + 1U];
    tx_buf[0] = SX1278_REG_FIFO | SX1278_SPI_WRITE_BIT;
    memcpy(&tx_buf[1], data, len);

    struct spi_buf tx = { .buf = tx_buf, .len = (size_t)(len + 1U) };
    struct spi_buf_set tx_set = { .buffers = &tx, .count = 1 };

    int ret = spi_write_dt(&cfg->spi, &tx_set);

    return ret;
}

static int sx1278_read_fifo(const struct device *dev,
                            uint8_t *buf, uint8_t len)
{
    const struct sx1278_fsk_config *cfg = dev->config;

    if (len == 0 || len > SX1278_FIFO_SIZE) {
        return -EINVAL;
    }

    uint8_t tx_buf[SX1278_FIFO_SIZE + 1U] = { 0 };
    uint8_t rx_buf[SX1278_FIFO_SIZE + 1U] = { 0 };

    tx_buf[0] = SX1278_REG_FIFO & SX1278_SPI_READ_MASK;

    struct spi_buf tx = { .buf = tx_buf, .len = (size_t)(len + 1U) };
    struct spi_buf rx = { .buf = rx_buf, .len = (size_t)(len + 1U) };
    struct spi_buf_set tx_set = { .buffers = &tx, .count = 1 };
    struct spi_buf_set rx_set = { .buffers = &rx, .count = 1 };

    int ret = spi_transceive_dt(&cfg->spi, &tx_set, &rx_set);

    if (ret == 0) {
        memcpy(buf, &rx_buf[1], len);
    }
    return ret;
}

/*---------------------------------------------------------------------------
 * Mode helpers
 *---------------------------------------------------------------------------*/

static int sx1278_set_mode(const struct device *dev, uint8_t mode)
{
    struct sx1278_fsk_data *data = dev->data;
    /* Keep LongRangeMode=0, ModulationType=FSK */
    uint8_t val = SX1278_OPMODE_MOD_FSK | (mode & SX1278_OPMODE_MASK) | SX1278_OPMODE_LOW_FREQUENCY_MODE;
    int ret = sx1278_write_reg(dev, SX1278_REG_OP_MODE, val);
    if (ret == 0) {
        data->op_mode = mode & SX1278_OPMODE_MASK;
    }
    return ret;
}

/**
 * Wait until IRQ1 has the ModeReady bit set (max ~10 ms).
 */
static int sx1278_wait_mode_ready(const struct device *dev)
{
    for (int i = 0; i < 100; i++) {
        uint8_t flags1;
        int ret = sx1278_read_reg(dev, SX1278_REG_IRQ_FLAGS1, &flags1);
        if (ret) {
            return ret;
        }
        if (flags1 & SX1278_IRQ1_MODE_READY) {
            return 0;
        }
        k_sleep(K_MSEC(1));
    }
    LOG_ERR("Timeout waiting for ModeReady");
    return -ETIMEDOUT;
}

/*---------------------------------------------------------------------------
 * Frequency / bitrate / deviation calculations
 *---------------------------------------------------------------------------*/

static int sx1278_program_frequency(const struct device *dev, uint32_t freq_hz)
{

    uint32_t frf = (uint32_t)(((uint64_t)freq_hz << 19) / SX1278_FXOSC_HZ);
    int ret;

    ret  = sx1278_write_reg(dev, SX1278_REG_FRF_MSB, (uint8_t)(frf >> 16));
    ret |= sx1278_write_reg(dev, SX1278_REG_FRF_MID, (uint8_t)(frf >> 8));
    ret |= sx1278_write_reg(dev, SX1278_REG_FRF_LSB, (uint8_t)(frf));

    LOG_DBG("Frequency %u Hz -> RegFrf=0x%06X", freq_hz, (unsigned)frf);
    return ret;
}

static int sx1278_program_bitrate(const struct device *dev, uint16_t bps)
{
    /* RegBitrate = F_XOSC / BitRate  (fractional part ignored) */
    uint16_t br = SX1278_FXOSC_HZ / bps;
    int ret;

    ret  = sx1278_write_reg(dev, SX1278_REG_BITRATE_MSB, (uint8_t)(br >> 8));
    ret |= sx1278_write_reg(dev, SX1278_REG_BITRATE_LSB, (uint8_t)(br));

    ret |= sx1278_write_reg(dev, SX1278_REG_BITRATE_FRAC, 0x00);

    LOG_DBG("BitRate %u bps -> RegBitrate=0x%04X", bps, br);
    return ret;
}

static int sx1278_program_fdev(const struct device *dev, uint32_t fdev_hz)
{
    uint32_t fdev = ((uint64_t)fdev_hz << 19) / SX1278_FXOSC_HZ;

    fdev &= 0x3FFFU;

    int ret;
    ret  = sx1278_write_reg(dev, SX1278_REG_FDEV_MSB, (uint8_t)(fdev >> 8));
    ret |= sx1278_write_reg(dev, SX1278_REG_FDEV_LSB, (uint8_t)(fdev));

    LOG_DBG("Fdev %u Hz -> RegFdev=0x%04X", fdev_hz, fdev);
    return ret;
}

/*---------------------------------------------------------------------------
 * PA configuration
 *---------------------------------------------------------------------------*/

static int sx1278_program_pa(const struct device *dev, int8_t power_dbm,
                             bool pa_boost)
{
    uint8_t pa_cfg;
    uint8_t pa_dac = SX1278_PA_DAC_DEFAULT;

    if (pa_boost) {
        if (power_dbm >= 20) {
            /* High-power mode: up to +20 dBm */
            pa_cfg = SX1278_PA_SELECT_BIT | (7U << 4) | 15U;
            pa_dac = SX1278_PA_DAC_20DBM;
        } else {
            /* +2 to +17 dBm: Pout = 2 + OutputPower */
            uint8_t output_power = (uint8_t)CLAMP((int)power_dbm - 2, 0, 15);
            pa_cfg = SX1278_PA_SELECT_BIT | (4U << 4) | output_power;
        }
    } else {
        /* RFO: Pout = Pmax - (15 - OutputPower)
         *       Pmax = 10.8 + 0.6*MaxPower  MaxPower=4 -> Pmax~13.2 dBm */
        uint8_t output_power = (uint8_t)CLAMP((int)power_dbm + 1, 0, 15);
        pa_cfg = (4U << 4) | output_power;
    }

    int ret;
    ret  = sx1278_write_reg(dev, SX1278_REG_PA_CONFIG, pa_cfg);
    ret |= sx1278_write_reg(dev, SX1278_REG_PA_DAC, pa_dac);

    // ret  = sx1278_write_reg(dev, SX1278_REG_PA_CONFIG, 0x8F);
    // ret = sx1278_write_reg(dev, SX1278_REG_PA_DAC, 0x87);

    LOG_DBG("PA cfg=0x%02X dac=0x%02X (%s %ddBm)",
            pa_cfg, pa_dac, pa_boost ? "BOOST" : "RFO", power_dbm);
    return ret;
}

/*---------------------------------------------------------------------------
 * DIO interrupt service routines
 *---------------------------------------------------------------------------*/

static void sx1278_dio0_isr(const struct device *gpio_dev,
                            struct gpio_callback *cb,
                            gpio_port_pins_t pins)
{
    ARG_UNUSED(gpio_dev);
    ARG_UNUSED(pins);

    struct sx1278_fsk_data *data =
        CONTAINER_OF(cb, struct sx1278_fsk_data, dio0_cb);

    k_sem_give(&data->irq_sem);
}

static void sx1278_dio1_isr(const struct device *gpio_dev,
                            struct gpio_callback *cb,
                            gpio_port_pins_t pins)
{
    ARG_UNUSED(gpio_dev);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);
}

/*---------------------------------------------------------------------------
 * Full chip configuration
 *---------------------------------------------------------------------------*/

static int sx1278_apply_config(const struct device *dev,
                               const struct fsk_config *cfg)
{
    int ret;

    /* ----------------------------------------------------------------
     * Step 1: Guarantee we are in FSK mode by writing SLEEP with
     *         LongRangeMode=0 first, then STANDBY.
     * ---------------------------------------------------------------- */
    ret = sx1278_write_reg(dev, SX1278_REG_OP_MODE,
                           SX1278_OPMODE_MOD_FSK | SX1278_OPMODE_SLEEP | SX1278_OPMODE_LOW_FREQUENCY_MODE);
    if (ret) return ret;
    k_sleep(K_MSEC(1));

    ret = sx1278_set_mode(dev, SX1278_OPMODE_STANDBY);
    if (ret) return ret;
    ret = sx1278_wait_mode_ready(dev);
    if (ret) return ret;

    /* ----------------------------------------------------------------
     * Step 2: RF parameters
     * ---------------------------------------------------------------- */
    ret = sx1278_program_frequency(dev, cfg->frequency_hz);
    if (ret) return ret;

    ret = sx1278_program_bitrate(dev, cfg->bitrate_bps);
    if (ret) return ret;

    ret = sx1278_program_fdev(dev, cfg->fdev_hz);
    if (ret) return ret;

    /* ----------------------------------------------------------------
     * Step 3: PA
     * ---------------------------------------------------------------- */
    ret = sx1278_program_pa(dev, cfg->tx_power_dbm, cfg->pa_boost);
    if (ret) return ret;

    /* PA ramp: 40 us (bits [3:0] = 9 -> 40 us default; keep shaping) */
    uint8_t pa_ramp = (uint8_t)(cfg->shaping << 5) | 0x09U;
    ret = sx1278_write_reg(dev, SX1278_REG_PA_RAMP, pa_ramp);
    if (ret) return ret;

    /* ----------------------------------------------------------------
     * Step 4: LNA - max gain, no boost
     * ---------------------------------------------------------------- */

    /* ----------------------------------------------------------------
     * Step 5: RX config
     * ---------------------------------------------------------------- */
    uint8_t rx_cfg = SX1278_RX_TRIGGER_SYNC_ADDR | SX1278_AGC_AUTO_ON;
    if (cfg->afc_on) {
        rx_cfg |= SX1278_AFC_AUTO_ON;
    }
    ret = sx1278_write_reg(dev, SX1278_REG_RX_CONFIG, rx_cfg);
    if (ret) return ret;

    /* RSSI smoothing: 8 samples */
    ret = sx1278_write_reg(dev, SX1278_REG_RSSI_CONFIG, 0x02U);
    if (ret) return ret;

    // 3. Increase OCP to 240 mA — required for +20 dBm
    ret = sx1278_write_reg(dev, SX1278_REG_OCP, 0x3BU);
    if (ret) return ret;

    /* RX and AFC bandwidths */
    ret = sx1278_write_reg(dev, SX1278_REG_RX_BW, cfg->rx_bw_reg);
    if (ret) return ret;
    ret = sx1278_write_reg(dev, SX1278_REG_AFC_BW, cfg->afc_bw_reg);
    if (ret) return ret;

    /* ----------------------------------------------------------------
     * Step 6: Preamble detector
     * ---------------------------------------------------------------- */
    ret = sx1278_write_reg(dev, SX1278_REG_PREAMBLE_DETECT,
                           SX1278_PREAMBLE_DETECTOR_ON |
                           SX1278_PREAMBLE_DETECTOR_2BYTES |
                           0x0AU /* tolerance = 10 chips */);
    if (ret) return ret;

    /* Preamble length */
    ret  = sx1278_write_reg(dev, SX1278_REG_PREAMBLE_MSB_FSK,
                            (uint8_t)(cfg->preamble_len >> 8));
    ret |= sx1278_write_reg(dev, SX1278_REG_PREAMBLE_LSB_FSK,
                            (uint8_t)(cfg->preamble_len));
    if (ret) return ret;

    /* ----------------------------------------------------------------
     * Step 7: Sync word
     * ---------------------------------------------------------------- */
    if (cfg->sync_len > 0 && cfg->sync_len <= 8) {
        uint8_t sync_cfg = SX1278_SYNC_ON |
                           ((cfg->sync_len - 1) & SX1278_SYNC_SIZE_MSK);
        ret = sx1278_write_reg(dev, SX1278_REG_SYNC_CONFIG, sync_cfg);
        if (ret) return ret;

        for (uint8_t i = 0; i < cfg->sync_len; i++) {
            ret = sx1278_write_reg(dev, SX1278_REG_SYNC_VALUE1 + i,
                                   cfg->sync_word[i]);
            if (ret) return ret;
        }
    } else {
        ret = sx1278_write_reg(dev, SX1278_REG_SYNC_CONFIG, 0x00);
        if (ret) return ret;
    }

    /* ----------------------------------------------------------------
     * Step 8: Packet config
     * ---------------------------------------------------------------- */
    uint8_t pkt1 = 0;

    if (cfg->packet_format == FSK_PACKET_VARIABLE) {
        pkt1 |= SX1278_PACKET_FORMAT_VARIABLE;
    }
    if (cfg->crc_on) {
        pkt1 |= SX1278_CRC_ON;
    }
    switch (cfg->dc_free) {
    case FSK_DC_FREE_MANCHESTER:
        pkt1 |= SX1278_DC_FREE_MANCHESTER;
        break;
    case FSK_DC_FREE_WHITENING:
        pkt1 |= SX1278_DC_FREE_WHITENING;
        break;
    default:
        break;
    }

    switch (cfg->addr_filter) {
    case FSK_ADDR_FILTER_NODE:
        pkt1 |= SX1278_ADDRESS_FILTERING_NODE;
        break;
    case FSK_ADDR_FILTER_NODE_BROADCAST:
        pkt1 |= SX1278_ADDRESS_FILTERING_BC;
        break;
    case FSK_ADDR_FILTER_NONE:
    default:
        pkt1 |= SX1278_ADDRESS_FILTERING_NONE;
        break;
    }
    ret = sx1278_write_reg(dev, SX1278_REG_PACKET_CONFIG1, pkt1);
    if (ret) return ret;

    ret = sx1278_write_reg(dev, SX1278_REG_NODE_ADRS, cfg->node_addr);
    if (ret) return ret;
    ret = sx1278_write_reg(dev, SX1278_REG_BROADCAST_ADRS, cfg->broadcast_addr);
    if (ret) return ret;

    /* Packet mode, data mode = packet */
    ret = sx1278_write_reg(dev, SX1278_REG_PACKET_CONFIG2,
                           SX1278_DATA_MODE_PACKET);
    if (ret) return ret;

    /* Payload length */
    ret = sx1278_write_reg(dev, SX1278_REG_PAYLOAD_LENGTH, cfg->payload_len);
    if (ret) return ret;

    ret = sx1278_write_reg(dev, SX1278_REG_FIFO_THRESH,
                           SX1278_TX_START_FIFO_NOT_EMPTY |
                           SX1278_FIFO_STREAM_THRESHOLD);
    if (ret) return ret;

    /* ----------------------------------------------------------------
     * Step 10: Timeouts
     * ---------------------------------------------------------------- */
    if (cfg->rx_timeout > 0) {
        ret = sx1278_write_reg(dev, SX1278_REG_RX_TIMEOUT2, cfg->rx_timeout);
    } else {
        ret = sx1278_write_reg(dev, SX1278_REG_RX_TIMEOUT2, 0x00);
    }
    if (ret) return ret;

    /* ----------------------------------------------------------------
     * Step 11: DIO mapping defaults
     * DIO0 = PayloadReady (RX) or PacketSent (TX) - same mapping 0x00
     * DIO1 = FifoLevel
     * ---------------------------------------------------------------- */
    ret = sx1278_write_reg(dev, SX1278_REG_DIO_MAPPING1,
                           SX1278_DIO0_PAYLOAD_READY_PACKET_SENT |
                           SX1278_DIO1_FIFO_LEVEL);
    if (ret) return ret;
    ret = sx1278_write_reg(dev, SX1278_REG_DIO_MAPPING2, 0x00);
    if (ret) return ret;

    /* ----------------------------------------------------------------
     * Step 12: Image calibration (run before first use)
     * ---------------------------------------------------------------- */
    ret = sx1278_write_reg(dev, SX1278_REG_IMAGE_CAL, 0x02U);
    if (ret) return ret;

    return 0;
}

/*---------------------------------------------------------------------------
 * API: configure
 *---------------------------------------------------------------------------*/

static int sx1278_fsk_configure(const struct device *dev,
                                const struct fsk_config *cfg)
{
    struct sx1278_fsk_data *data = dev->data;
    int ret;

    if (!cfg || cfg->bitrate_bps == 0 || cfg->frequency_hz == 0) {
        return -EINVAL;
    }

    if (cfg->packet_format == FSK_PACKET_FIXED) {
        if (cfg->payload_len == 0) {
            LOG_ERR("Fixed packet format requires non-zero payload_len");
            return -EINVAL;
        }
    }

    if (cfg->addr_filter != FSK_ADDR_FILTER_NONE && cfg->sync_len == 0) {
        LOG_ERR("Address filtering requires sync word to be enabled (sync_len > 0)");
        return -EINVAL;
    }

    k_mutex_lock(&data->lock, K_FOREVER);

    ret = sx1278_apply_config(dev, cfg);
    if (ret == 0) {
        memcpy(&data->cfg, cfg, sizeof(*cfg));
        data->configured = true;
        LOG_INF("SX1278 FSK configured: %u Hz, %u bps, Fdev=%u Hz",
                cfg->frequency_hz, cfg->bitrate_bps, cfg->fdev_hz);

        if (IS_ENABLED(CONFIG_LOG) && CONFIG_FSK_LOG_LEVEL >= 4) {
            uint8_t r_opmode, r_br_msb, r_br_lsb, r_fdev_msb, r_fdev_lsb;
            uint8_t r_frf_msb, r_frf_mid, r_frf_lsb;
            uint8_t r_pkt1, r_pkt2, r_fifo_thr, r_sync_cfg;
            uint8_t r_pa, r_pa_dac, r_dio1, t_thresh;

            sx1278_read_reg(dev, SX1278_REG_OP_MODE,        &r_opmode);
            sx1278_read_reg(dev, SX1278_REG_BITRATE_MSB,    &r_br_msb);
            sx1278_read_reg(dev, SX1278_REG_BITRATE_LSB,    &r_br_lsb);
            sx1278_read_reg(dev, SX1278_REG_FDEV_MSB,       &r_fdev_msb);
            sx1278_read_reg(dev, SX1278_REG_FDEV_LSB,       &r_fdev_lsb);
            sx1278_read_reg(dev, SX1278_REG_FRF_MSB,        &r_frf_msb);
            sx1278_read_reg(dev, SX1278_REG_FRF_MID,        &r_frf_mid);
            sx1278_read_reg(dev, SX1278_REG_FRF_LSB,        &r_frf_lsb);
            sx1278_read_reg(dev, SX1278_REG_PACKET_CONFIG1, &r_pkt1);
            sx1278_read_reg(dev, SX1278_REG_PACKET_CONFIG2, &r_pkt2);
            sx1278_read_reg(dev, SX1278_REG_FIFO_THRESH,    &r_fifo_thr);
            sx1278_read_reg(dev, SX1278_REG_SYNC_CONFIG,    &r_sync_cfg);
            sx1278_read_reg(dev, SX1278_REG_PA_CONFIG,      &r_pa);
            sx1278_read_reg(dev, SX1278_REG_PA_DAC,         &r_pa_dac);
            sx1278_read_reg(dev, SX1278_REG_DIO_MAPPING1,   &r_dio1);
            sx1278_read_reg(dev, SX1278_REG_RSSI_THRESH,    &t_thresh);

            LOG_DBG("--- Register readback after configure ---");
            LOG_DBG("  RegOpMode    = 0x%02X (want 0x01=STBY, bit7=0=FSK)", r_opmode);
            LOG_DBG("  RegBitrate   = 0x%02X%02X (want 0x1A0A @ 4800bps)", r_br_msb, r_br_lsb);
            LOG_DBG("  RegFdev      = 0x%02X%02X (want 0x0051 @ 5kHz)", r_fdev_msb, r_fdev_lsb);
            LOG_DBG("  RegFrf       = 0x%02X%02X%02X (want 0x6C8000 @ 433MHz)", r_frf_msb, r_frf_mid, r_frf_lsb);
            LOG_DBG("  RegPktCfg1   = 0x%02X (bit7=var, bit4=crc, bits[6:5]=dc)", r_pkt1);
            LOG_DBG("  RegPktCfg2   = 0x%02X (bit6 MUST=1 for pkt mode)", r_pkt2);
            LOG_DBG("  RegFifoThr   = 0x%02X (bit7 MUST=1; 0x20=thresh32)", r_fifo_thr);
            LOG_DBG("  RegSyncCfg   = 0x%02X (bit4=SyncOn, bits[2:0]=size-1)", r_sync_cfg);
            LOG_DBG("  RegPA        = 0x%02X (bit7=1 PA_BOOST / 0 RFO)", r_pa);
            LOG_DBG("  RegPA_DAC    = 0x%02X (0x84=default, 0x87=20dBm)", r_pa_dac);
            LOG_DBG("  RegDioMap1   = 0x%02X (bits[7:6]=00 -> PktSent/PldRdy)", r_dio1);
            LOG_DBG("  RegRssiThr   = 0x%02X ", t_thresh);

            if (r_opmode & SX1278_OPMODE_LONG_RANGE_MODE) {
                LOG_ERR("  !! LongRangeMode=1 - chip is in LoRa mode !! "
                        "SLEEP+write 0x00 did not clear it.");
            }
            if (!(r_fifo_thr & SX1278_TX_START_FIFO_NOT_EMPTY)) {
                LOG_ERR("  !! FifoThresh bit7=0 - TxStartCondition=FifoLevel:"
                        " TX may hang for short packets. Must be 1=FifoNotEmpty!!");
            }
            if (!(r_pkt2 & SX1278_DATA_MODE_PACKET)) {
                LOG_ERR("  !! DataMode bit6=0 - chip is in continuous mode !!");
            }
            if (r_br_msb == 0x00 && r_br_lsb == 0x00) {
                LOG_ERR("  !! BitRate registers = 0x0000 - SPI write likely failed !!");
            }
        }
    } else {
        LOG_ERR("Configuration failed: %d", ret);
    }

    k_mutex_unlock(&data->lock);
    return ret;
}

/*---------------------------------------------------------------------------
 * API: set_frequency
 *---------------------------------------------------------------------------*/

static int sx1278_fsk_set_frequency(const struct device *dev, uint32_t freq_hz)
{
    struct sx1278_fsk_data *data = dev->data;
    int ret;

    k_mutex_lock(&data->lock, K_FOREVER);
    ret = sx1278_set_mode(dev, SX1278_OPMODE_STANDBY);
    if (ret == 0) {
        ret = sx1278_program_frequency(dev, freq_hz);
        if (ret == 0) {
            data->cfg.frequency_hz = freq_hz;
        }
    }
    k_mutex_unlock(&data->lock);
    return ret;
}

/*---------------------------------------------------------------------------
 * API: set_bitrate
 *---------------------------------------------------------------------------*/

static int sx1278_fsk_set_bitrate(const struct device *dev, uint32_t bps)
{
    struct sx1278_fsk_data *data = dev->data;
    int ret;

    k_mutex_lock(&data->lock, K_FOREVER);
    ret = sx1278_set_mode(dev, SX1278_OPMODE_STANDBY);
    if (ret == 0) {
        ret = sx1278_program_bitrate(dev, bps);
        if (ret == 0) {
            data->cfg.bitrate_bps = bps;
        }
    }
    k_mutex_unlock(&data->lock);
    return ret;
}

/*---------------------------------------------------------------------------
 * API: set_frequency_deviation
 *---------------------------------------------------------------------------*/

static int sx1278_fsk_set_frequency_deviation(const struct device *dev,
                                              uint32_t fdev_hz)
{
    struct sx1278_fsk_data *data = dev->data;
    int ret;

    k_mutex_lock(&data->lock, K_FOREVER);
    ret = sx1278_set_mode(dev, SX1278_OPMODE_STANDBY);
    if (ret == 0) {
        ret = sx1278_program_fdev(dev, fdev_hz);
        if (ret == 0) {
            data->cfg.fdev_hz = fdev_hz;
        }
    }
    k_mutex_unlock(&data->lock);
    return ret;
}

/*---------------------------------------------------------------------------
 * API: set_rx_bandwidth
 *---------------------------------------------------------------------------*/

static int sx1278_fsk_set_rx_bandwidth(const struct device *dev,
                                       uint8_t rx_bw_reg)
{
    struct sx1278_fsk_data *data = dev->data;
    int ret;

    k_mutex_lock(&data->lock, K_FOREVER);
    ret = sx1278_set_mode(dev, SX1278_OPMODE_STANDBY);
    if (ret == 0) {
        ret = sx1278_write_reg(dev, SX1278_REG_RX_BW, rx_bw_reg);
        if (ret == 0) {
            data->cfg.rx_bw_reg = rx_bw_reg;
        }
    }
    k_mutex_unlock(&data->lock);
    return ret;
}

/*---------------------------------------------------------------------------
 * API: standby / sleep
 *---------------------------------------------------------------------------*/

static int sx1278_fsk_standby(const struct device *dev)
{
    struct sx1278_fsk_data *data = dev->data;
    int ret;

    k_mutex_lock(&data->lock, K_FOREVER);
    ret = sx1278_set_mode(dev, SX1278_OPMODE_STANDBY);
    k_mutex_unlock(&data->lock);
    return ret;
}

static int sx1278_fsk_sleep(const struct device *dev)
{
    struct sx1278_fsk_data *data = dev->data;
    int ret;

    k_mutex_lock(&data->lock, K_FOREVER);
    ret = sx1278_set_mode(dev, SX1278_OPMODE_SLEEP);
    k_mutex_unlock(&data->lock);
    return ret;
}

/*---------------------------------------------------------------------------
 * RX helper: (re-)enter RX_CONTINUOUS if not already there
 *
 * Idempotent by design: if the radio is already in RX_CONTINUOUS (checked
 * via data->op_mode, not a fresh register read) this is a no-op.
 *
 * Caller must hold data->lock.
 *---------------------------------------------------------------------------*/
static int sx1278_rx_enter(const struct device *dev)
{
    struct sx1278_fsk_data *data = dev->data;
    int ret;

    if (data->op_mode == SX1278_OPMODE_RXCONTINUOUS) {
        return 0;
    }

    ret = sx1278_set_mode(dev, SX1278_OPMODE_STANDBY);
    if (ret) return ret;
    ret = sx1278_wait_mode_ready(dev);
    if (ret) return ret;

    /* Clear FIFO overrun */
    ret = sx1278_write_reg(dev, SX1278_REG_IRQ_FLAGS2, SX1278_IRQ2_FIFO_OVERRUN);
    if (ret) return ret;

    /* Restart AFC */
    if (data->cfg.afc_on) {
        ret = sx1278_write_reg(dev, SX1278_REG_AFC_FEI,
                               SX1278_AFC_CLEAR | SX1278_AFC_START);
        if (ret) return ret;
    }

    /* DIO0 mapping: PayloadReady */
    ret = sx1278_write_reg(dev, SX1278_REG_DIO_MAPPING1,
                           SX1278_DIO0_PAYLOAD_READY_PACKET_SENT);
    if (ret) return ret;

    k_sem_reset(&data->irq_sem);

    return sx1278_set_mode(dev, SX1278_OPMODE_RXCONTINUOUS);
}

/*---------------------------------------------------------------------------
 * TX helper: load frame into FIFO in one burst and wait for PacketSent.
 *
 * @param on_air      Frame bytes to transmit (after the length prefix).
 * @param on_air_len  Number of bytes; must be <= SX1278_FRAME_MAX_ON_AIR (63).
 *
 * Because every frame fits inside the 64-byte FIFO (1 length prefix +
 * up to 63 on-air bytes) there is no streaming fill loop.  The full
 * frame is written in one SPI burst before TX mode is entered, so
 * PacketSent fires as soon as the radio has clocked out the last byte.
 *
 * Caller must hold data->lock.  Leaves the radio in STANDBY on return.
 *---------------------------------------------------------------------------*/
static int sx1278_tx_fifo_and_wait(const struct device *dev,
                                   const uint8_t *on_air, uint8_t on_air_len,
                                   k_timeout_t timeout)
{
    struct sx1278_fsk_data *data = dev->data;
    int ret;

    /* on_air_len must fit in the FIFO minus the 1-byte length prefix. */
    if (on_air_len == 0U || on_air_len > SX1278_FRAME_MAX_ON_AIR) {
        return -EINVAL;
    }

    /* Discard any stale PayloadReady/PacketSent IRQ left from a prior op. */
    k_sem_reset(&data->irq_sem);

    ret = sx1278_set_mode(dev, SX1278_OPMODE_STANDBY);
    if (ret) goto out;
    ret = sx1278_wait_mode_ready(dev);
    if (ret) goto out;

    /*
     * Build the FIFO burst: [length_prefix | on_air bytes].
     * Total = 1 + on_air_len <= 64 bytes — always fits in one write.
     */
    {
        uint8_t stage[SX1278_FIFO_SIZE];

        stage[0] = on_air_len;                  /* variable-length prefix */
        memcpy(&stage[1], on_air, on_air_len);

        ret = sx1278_write_fifo(dev, stage, (uint8_t)(1U + on_air_len));
        if (ret) goto out;

        LOG_DBG("TX FIFO initial burst: %u bytes (%u of %u on-air bytes queued)",
                (unsigned)(1U + on_air_len), (unsigned)on_air_len,
                (unsigned)on_air_len);
    }

    /* Map DIO0 → PacketSent and enter TX. */
    ret = sx1278_write_reg(dev, SX1278_REG_DIO_MAPPING1,
                           SX1278_DIO0_PAYLOAD_READY_PACKET_SENT);
    if (ret) goto out;

    ret = sx1278_set_mode(dev, SX1278_OPMODE_TX);
    if (ret) goto out_standby;

    /* Wait for PacketSent via DIO0 IRQ, with polling fallback. */
    ret = k_sem_take(&data->irq_sem, timeout);
    if (ret == -EAGAIN) {
        bool sent_by_poll = false;

        for (int poll_i = 0; poll_i < 100; poll_i++) {
            uint8_t flags2_poll = 0;

            k_sleep(K_MSEC(2));
            sx1278_read_reg(dev, SX1278_REG_IRQ_FLAGS2, &flags2_poll);
            if (flags2_poll & SX1278_IRQ2_PACKET_SENT) {
                sent_by_poll = true;
                break;
            }
        }

        if (sent_by_poll) {
            LOG_WRN("PacketSent confirmed by POLL - DIO0 interrupt not "
                    "working; check wiring/overlay pin");
            ret = 0;
        } else {
            LOG_ERR("TX hard timeout (on_air_len=%u)", (unsigned)on_air_len);
            ret = -ETIMEDOUT;
        }
    }

out_standby:
    sx1278_set_mode(dev, SX1278_OPMODE_STANDBY);
out:
    return ret;
}

/*---------------------------------------------------------------------------
 * RX helper: wait for one packet and unload it from the FIFO.
 *
 * @param rxbuf    Caller buffer; must be at least SX1278_FRAME_MAX_ON_AIR
 *                 (63) bytes.
 * @param out_len  On success, set to the number of on-air payload bytes.
 * @param meta     Filled with RSSI / AFC / CRC status.
 *
 * Because every frame fits inside the 64-byte FIFO there is no streaming
 * drain loop.  After PayloadReady fires we do two small SPI reads:
 *   1. Read 1 byte  → the variable-length prefix (actual_len).
 *   2. Read actual_len bytes → the on-air payload.
 * This avoids over-reading an empty FIFO, which was the root cause of the
 * all-zeros / repeated-byte garbage frames.
 *
 * Caller must hold data->lock and must already have entered RX_CONTINUOUS.
 *---------------------------------------------------------------------------*/
static int sx1278_rx_wait_and_unload(const struct device *dev,
                                     uint8_t *rxbuf, uint8_t *out_len,
                                     struct fsk_rx_metadata *meta,
                                     k_timeout_t timeout)
{
    struct sx1278_fsk_data *data = dev->data;
    int ret;

    k_timepoint_t deadline = sys_timepoint_calc(timeout);

    /* ---- Wait for PayloadReady ---- */
    while (1) {
        ret = k_sem_take(&data->irq_sem, K_MSEC(2));
        if (ret == 0) {
            /* DIO0 fired → PayloadReady */
            break;
        }

        /* Poll fallback (handles missing DIO0 edge). */
        uint8_t flags2_poll = 0;
        sx1278_read_reg(dev, SX1278_REG_IRQ_FLAGS2, &flags2_poll);

        if (flags2_poll & SX1278_IRQ2_FIFO_OVERRUN) {
            LOG_ERR("RX: FIFO overrun - packet lost");
            /* Clear the overrun flag. */
            sx1278_write_reg(dev, SX1278_REG_IRQ_FLAGS2,
                             SX1278_IRQ2_FIFO_OVERRUN);
            return -EIO;
        }

        if (flags2_poll & SX1278_IRQ2_PAYLOAD_READY) {
            ret = 0;
            break;
        }

        if (sys_timepoint_expired(deadline)) {
            return -EAGAIN;
        }
    }

    /* ---- Packet is in the FIFO: read metadata registers first ---- */

    uint8_t rssi_raw = 0;
    sx1278_read_reg(dev, SX1278_REG_RSSI_VALUE, &rssi_raw);
    int16_t rssi_dbm = -(int16_t)rssi_raw / 2;

    int32_t afc_hz = 0;
    if (data->cfg.afc_on) {
        uint8_t afc_msb = 0, afc_lsb = 0;
        sx1278_read_reg(dev, SX1278_REG_AFC_MSB, &afc_msb);
        sx1278_read_reg(dev, SX1278_REG_AFC_LSB, &afc_lsb);
        int16_t afc_raw = (int16_t)((uint16_t)afc_msb << 8 | afc_lsb);
        afc_hz = (int32_t)afc_raw * 61;
    }

    uint8_t flags2 = 0;
    sx1278_read_reg(dev, SX1278_REG_IRQ_FLAGS2, &flags2);

    bool crc_ok = true;
    if (data->cfg.crc_on) {
        crc_ok = (flags2 & SX1278_IRQ2_CRC_OK) != 0;
        if (!crc_ok) {
            LOG_WRN("RX: CRC failure (flags2=0x%02X) - discarding", flags2);
        }
    }

    /* ---- Step 1: read the 1-byte variable-length prefix ---- */
    uint8_t actual_len = 0;
    ret = sx1278_read_fifo(dev, &actual_len, 1U);
    if (ret) {
        return ret;
    }

    if (actual_len == 0U || actual_len > SX1278_FRAME_MAX_ON_AIR) {
        LOG_WRN("RX: bad length prefix 0x%02X - discarding", actual_len);
        /* Flush whatever is left in the FIFO via a SLEEP cycle. */
        sx1278_set_mode(dev, SX1278_OPMODE_SLEEP);
        k_sleep(K_MSEC(1));
        sx1278_set_mode(dev, SX1278_OPMODE_STANDBY);
        return -EBADMSG;
    }

    /* ---- Step 2: read exactly actual_len payload bytes ---- */
    ret = sx1278_read_fifo(dev, rxbuf, actual_len);
    if (ret) {
        return ret;
    }

    LOG_DBG("RX drain: payload_have=%u want=%u chunk=%u",
            (unsigned)actual_len, (unsigned)actual_len, (unsigned)actual_len);

    /* ---- Verify FIFO is now empty (sanity check) ---- */
    {
        uint8_t flags2_check = 0;
        sx1278_read_reg(dev, SX1278_REG_IRQ_FLAGS2, &flags2_check);
        if (!(flags2_check & SX1278_IRQ2_FIFO_EMPTY)) {
            LOG_WRN("RX: FIFO not empty after exact read (len=%u) - "
                    "flushing via SLEEP", (unsigned)actual_len);
            sx1278_set_mode(dev, SX1278_OPMODE_SLEEP);
            k_sleep(K_MSEC(1));
            sx1278_set_mode(dev, SX1278_OPMODE_STANDBY);
        }
    }

    if (meta) {
        meta->rssi        = rssi_dbm;
        meta->afc_hz      = afc_hz;
        meta->crc_ok      = crc_ok;
        meta->payload_len = 0;   /* filled in by sx1278_process_rx_frame */
        meta->src_addr    = 0;
    }

    *out_len = actual_len;
    return 0;
}

/*---------------------------------------------------------------------------
 * Deduplication helper
 *
 * Returns true  if (src, seq) was already seen recently -> duplicate, drop.
 * Returns false if it is new -> record it and allow processing.
 *
 * Uses a fixed-size circular table with round-robin eviction; the oldest
 * entry is overwritten when the table is full.  With SX1278_DEDUP_TABLE_SIZE
 * == 8 and max_retries == 3, the table can track 2-3 concurrent senders
 * simultaneously without any false positives.
 *
 * Caller must hold data->lock.
 *---------------------------------------------------------------------------*/
static bool sx1278_dedup_is_duplicate(struct sx1278_fsk_data *data,
                                      uint8_t src, uint8_t seq)
{
    for (int i = 0; i < SX1278_DEDUP_TABLE_SIZE; i++) {
        if (data->dedup_table[i].valid &&
            data->dedup_table[i].src == src &&
            data->dedup_table[i].seq == seq) {
            LOG_DBG("Dedup: dropping duplicate frame src=0x%02X seq=%u",
                    src, (unsigned)seq);
            return true;
        }
    }

    /* New pair - record in the next eviction slot */
    uint8_t slot = data->dedup_next;
    data->dedup_table[slot].src   = src;
    data->dedup_table[slot].seq   = seq;
    data->dedup_table[slot].valid = true;
    data->dedup_next = (uint8_t)((slot + 1U) & (SX1278_DEDUP_TABLE_SIZE - 1U));

    return false;
}

/*---------------------------------------------------------------------------
 * Parse one received on-air frame.
 *
 * Frame layout (no type byte):
 *   [Dest][Src][Content ... payload_len bytes][Seq]
 *
 * ACK vs DATA is distinguished by payload_len == 0, not by any type field.
 * Duplicate DATA frames (same src + seq already in dedup table) are silently
 * dropped before the user callback is invoked.
 *
 * Caller must hold data->lock; released/re-acquired around user callback.
 *---------------------------------------------------------------------------*/
static void sx1278_process_rx_frame(const struct device *dev,
                                    const uint8_t *raw, uint8_t on_air_len,
                                    const struct fsk_rx_metadata *meta_in)
{
    struct sx1278_fsk_data *data = dev->data;

    if (!meta_in->crc_ok) {
        LOG_WRN("Dropping frame with bad CRC");
        return;
    }

    if (on_air_len < SX1278_FRAME_OVERHEAD) {
        LOG_WRN("Dropping runt frame (%u bytes, need >= %u)",
                on_air_len, SX1278_FRAME_OVERHEAD);
        return;
    }

    uint8_t src         = raw[1];
    uint8_t payload_len = on_air_len - SX1278_FRAME_OVERHEAD;
    uint8_t trl_off     = SX1278_FRAME_HDR_LEN + payload_len;
    uint8_t seq         = raw[trl_off];

    const uint8_t *payload = &raw[SX1278_FRAME_HDR_LEN];

    if (sx1278_dedup_is_duplicate(data, src, seq)) {
        return;
    }

    LOG_DBG("DATA from 0x%02X seq=%u payload_len=%u",
            src, (unsigned)seq, payload_len);

    {
        int rx_ret = sx1278_rx_enter(dev);
        if (rx_ret) {
            LOG_ERR("Failed to re-enter RX after ACK TX: %d", rx_ret);
        }
    }

    if (data->rx_callback) {
        fsk_rx_callback_t cb = data->rx_callback;
        void *user_data = data->rx_user_data;
        struct fsk_rx_metadata cb_meta = *meta_in;
        cb_meta.payload_len = payload_len;
        cb_meta.src_addr    = src;

        uint8_t payload_copy[SX1278_FRAME_MAX_PAYLOAD]; /* 60 bytes max */
        if (payload_len > 0) {
            memcpy(payload_copy, payload, payload_len);
        }

        k_mutex_unlock(&data->lock);
        cb(dev, payload_copy, payload_len, &cb_meta, user_data);
        k_mutex_lock(&data->lock, K_FOREVER);
    }
}

/*---------------------------------------------------------------------------
 * Always-on internal RX thread.
 *---------------------------------------------------------------------------*/
static void sx1278_rx_thread_fn(void *p1, void *p2, void *p3)
{
    const struct device *dev = p1;
    struct sx1278_fsk_data *data = dev->data;

    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    while (1) {
        if (!data->configured || atomic_get(&data->tx_yield_request)) {
            k_sleep(K_MSEC(1));
            continue;
        }

        k_mutex_lock(&data->lock, K_FOREVER);

        if (!data->configured || atomic_get(&data->tx_yield_request)) {
            k_mutex_unlock(&data->lock);
            k_sleep(K_MSEC(1));
            continue;
        }

        int ret = sx1278_rx_enter(dev);
        if (ret) {
            k_mutex_unlock(&data->lock);
            k_sleep(K_MSEC(10));
            continue;
        }

        /*
         * rxbuf must hold a complete on-air frame including the trailer.
         * Max on-air frame = SX1278_FRAME_OVERHEAD + SX1278_FRAME_MAX_PAYLOAD
         *                  = 3 + 252 = 255 bytes.
         */
        uint8_t rxbuf[SX1278_FRAME_MAX_ON_AIR]; /* 63 bytes — fits in one FIFO */
        uint8_t on_air_len = 0;
        struct fsk_rx_metadata meta = {0};

        ret = sx1278_rx_wait_and_unload(dev, rxbuf, &on_air_len, &meta,
                                        K_MSEC(SX1278_RX_POLL_SLICE_MS));
        if (ret == 0) {
            sx1278_process_rx_frame(dev, rxbuf, on_air_len, &meta);
        } else if (ret != -EAGAIN) {
            LOG_WRN("RX thread: receive error %d, retrying", ret);
        }

        k_mutex_unlock(&data->lock);
    }
}

/*---------------------------------------------------------------------------
 * API: send_to
 *
 * Builds a framed DATA packet and transmits it up to max_retries times.
 * The receiver's dedup table ensures only the first copy is processed.
 *
 * Frame layout written to air:
 *   [Dest][Src][payload...len bytes][Seq]
 *
 * Returns 0 on success, negative errno on failure.
 *---------------------------------------------------------------------------*/
static int sx1278_fsk_send_to(const struct device *dev, uint8_t dest_addr,
                              const uint8_t *payload, uint8_t len,
                              k_timeout_t timeout)
{
    struct sx1278_fsk_data *data = dev->data;

    if (!data->configured) {
        return -ENODEV;
    }
    if (data->cfg.packet_format != FSK_PACKET_VARIABLE) {
        LOG_ERR("fsk_send_to() requires FSK_PACKET_VARIABLE mode");
        return -ENOTSUP;
    }
    if (len == 0) {
        LOG_ERR("fsk_send_to(): len must be >= 1");
        return -EINVAL;
    }
    if (len > SX1278_FRAME_MAX_PAYLOAD) {
        LOG_ERR("fsk_send_to(): len %u exceeds max payload %u",
                (unsigned)len, (unsigned)SX1278_FRAME_MAX_PAYLOAD);
        return -EINVAL;
    }
    if (payload == NULL) {
        return -EINVAL;
    }

    atomic_set(&data->tx_yield_request, 1);
    k_mutex_lock(&data->lock, K_FOREVER);
    atomic_set(&data->tx_yield_request, 0);

    uint8_t seq = ++data->tx_seq;
    if(!seq)  seq = data->tx_seq = 1;

    uint8_t frame[SX1278_FRAME_OVERHEAD + SX1278_FRAME_MAX_PAYLOAD];

    /* Header */
    frame[0] = dest_addr;
    frame[1] = data->cfg.node_addr;

    memcpy(&frame[SX1278_FRAME_HDR_LEN], payload, len);

    uint8_t trl_off = SX1278_FRAME_HDR_LEN + len;
    frame[trl_off]  = seq;

    uint8_t on_air_len = (uint8_t)(SX1278_FRAME_OVERHEAD + len);

    uint8_t max_attempts = (uint8_t)(data->cfg.max_retries);

    int ret = -ETIMEDOUT;

    for (uint8_t attempt = 0; attempt < max_attempts; attempt++) {

        int tx_ret = sx1278_tx_fifo_and_wait(dev, frame, on_air_len, timeout);
        if (tx_ret) {
            ret = tx_ret;
            LOG_WRN("send_to(0x%02X): TX failed (attempt %u/%u): %d",
                    dest_addr, attempt + 1, max_attempts, tx_ret);
            continue;
        }

        LOG_DBG("send_to(0x%02X): attempt %u/%u sent (seq=%u)",
                dest_addr, attempt + 1, max_attempts, (unsigned)seq);

        ret = 0;

        k_sleep(K_MSEC(10));
    }

    sx1278_set_mode(dev, SX1278_OPMODE_STANDBY);
    k_mutex_unlock(&data->lock);
    return ret;
}

static int sx1278_fsk_send(const struct device *dev,
                           const uint8_t *payload, uint8_t len,
                           k_timeout_t timeout)
{
    struct sx1278_fsk_data *data = dev->data;

    if (!data->configured) {
        return -ENODEV;
    }
    if (data->cfg.packet_format != FSK_PACKET_VARIABLE) {
        LOG_ERR("fsk_send() requires FSK_PACKET_VARIABLE mode");
        return -ENOTSUP;
    }
    if (len == 0 || len > SX1278_FRAME_MAX_ON_AIR) {
        return -EINVAL;
    }
    if (payload == NULL) {
        return -EINVAL;
    }

    atomic_set(&data->tx_yield_request, 1);
    k_mutex_lock(&data->lock, K_FOREVER);
    atomic_set(&data->tx_yield_request, 0);

    int ret = sx1278_tx_fifo_and_wait(dev, payload, len, timeout);

    if (ret == 0) {
        LOG_DBG("fsk_send(): %u bytes sent", len);
    } else {
        LOG_WRN("fsk_send(): TX failed: %d", ret);
    }

    sx1278_set_mode(dev, SX1278_OPMODE_STANDBY);
    k_mutex_unlock(&data->lock);
    return ret;
}

/*---------------------------------------------------------------------------
 * API: set_rx_callback
 *---------------------------------------------------------------------------*/

static int sx1278_fsk_set_rx_callback(const struct device *dev,
                                      fsk_rx_callback_t cb, void *user_data)
{
    struct sx1278_fsk_data *data = dev->data;

    if (!data->configured) {
        return -ENODEV;
    }
    if (data->cfg.packet_format != FSK_PACKET_VARIABLE) {
        LOG_ERR("fsk_set_rx_callback() requires FSK_PACKET_VARIABLE mode");
        return -ENOTSUP;
    }

    k_mutex_lock(&data->lock, K_FOREVER);
    data->rx_callback = cb;
    data->rx_user_data = user_data;
    k_mutex_unlock(&data->lock);

    LOG_INF("RX callback %s", cb ? "registered" : "cleared");
    return 0;
}

/*---------------------------------------------------------------------------
 * Hardware reset
 *---------------------------------------------------------------------------*/

static int sx1278_hw_reset(const struct device *dev)
{
    const struct sx1278_fsk_config *cfg = dev->config;

    if (cfg->reset_gpio.port == NULL) {
        LOG_INF("No reset-gpios in devicetree - using software reset "
                "(single-GPIO mode)");
        sx1278_write_reg(dev, SX1278_REG_OP_MODE,
                         SX1278_OPMODE_MOD_FSK | SX1278_OPMODE_SLEEP);
        k_sleep(K_MSEC(10));
        return 0;
    }

    if (!device_is_ready(cfg->reset_gpio.port)) {
        LOG_ERR("Reset GPIO not ready");
        return -ENODEV;
    }

    gpio_pin_configure_dt(&cfg->reset_gpio, GPIO_OUTPUT_ACTIVE);
    k_sleep(K_MSEC(1));
    gpio_pin_set_dt(&cfg->reset_gpio, 0);
    k_sleep(K_MSEC(10));

    return 0;
}

/*---------------------------------------------------------------------------
 * Driver init
 *---------------------------------------------------------------------------*/

static int sx1278_fsk_init(const struct device *dev)
{
    const struct sx1278_fsk_config *cfg = dev->config;
    struct sx1278_fsk_data *data = dev->data;
    int ret;

    k_mutex_init(&data->lock);
    k_sem_init(&data->irq_sem, 0, 1);
    atomic_set(&data->tx_yield_request, 0);
    data->tx_seq = 0;
    data->rx_callback = NULL;
    data->rx_user_data = NULL;

    /* Initialise deduplication table */
    memset(data->dedup_table, 0, sizeof(data->dedup_table));
    data->dedup_next = 0;

    if (!spi_is_ready_dt(&cfg->spi)) {
        LOG_ERR("SPI device not ready");
        return -ENODEV;
    }

    ret = sx1278_hw_reset(dev);
    if (ret) return ret;

    uint8_t version = 0;
    ret = sx1278_read_reg(dev, SX1278_REG_VERSION, &version);
    if (ret) {
        LOG_ERR("SPI read failed during version check");
        return ret;
    }
    if (version != SX1278_CHIP_VERSION) {
        LOG_ERR("Bad chip version: 0x%02X (expected 0x%02X)",
                version, SX1278_CHIP_VERSION);
        return -ENODEV;
    }
    LOG_INF("SX1278 detected (version=0x%02X)", version);

    ret = sx1278_write_reg(dev, SX1278_REG_OP_MODE,
                           SX1278_OPMODE_MOD_FSK | SX1278_OPMODE_SLEEP);
    if (ret) return ret;
    k_sleep(K_MSEC(2));

    ret = sx1278_set_mode(dev, SX1278_OPMODE_STANDBY);
    if (ret) return ret;

    if (!device_is_ready(cfg->dio0_gpio.port)) {
        LOG_ERR("DIO0 GPIO not ready");
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&cfg->dio0_gpio, GPIO_INPUT);
    if (ret) return ret;

    gpio_init_callback(&data->dio0_cb, sx1278_dio0_isr,
                       BIT(cfg->dio0_gpio.pin));
    ret = gpio_add_callback(cfg->dio0_gpio.port, &data->dio0_cb);
    if (ret) return ret;

    ret = gpio_pin_interrupt_configure_dt(&cfg->dio0_gpio,
                                          GPIO_INT_EDGE_RISING);
    if (ret) return ret;

    if (device_is_ready(cfg->dio1_gpio.port)) {
        ret = gpio_pin_configure_dt(&cfg->dio1_gpio, GPIO_INPUT);
        if (ret == 0) {
            gpio_init_callback(&data->dio1_cb, sx1278_dio1_isr,
                               BIT(cfg->dio1_gpio.pin));
            gpio_add_callback(cfg->dio1_gpio.port, &data->dio1_cb);
            gpio_pin_interrupt_configure_dt(&cfg->dio1_gpio,
                                            GPIO_INT_EDGE_RISING);
        }
    }

    k_thread_create(&data->rx_thread, data->rx_thread_stack,
                    K_KERNEL_STACK_SIZEOF(data->rx_thread_stack),
                    sx1278_rx_thread_fn, (void *)dev, NULL, NULL,
                    SX1278_RX_THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&data->rx_thread, "sx1278_fsk_rx");

    LOG_INF("SX1278 FSK driver initialised");
    return 0;
}

/*---------------------------------------------------------------------------
 * API vtable
 *---------------------------------------------------------------------------*/

static const struct fsk_driver_api sx1278_fsk_api = {
    .configure               = sx1278_fsk_configure,
    .send_to                 = sx1278_fsk_send_to,
    .send                    = sx1278_fsk_send,
    .set_rx_callback         = sx1278_fsk_set_rx_callback,
    .set_frequency           = sx1278_fsk_set_frequency,
    .set_bitrate             = sx1278_fsk_set_bitrate,
    .set_frequency_deviation = sx1278_fsk_set_frequency_deviation,
    .set_rx_bandwidth        = sx1278_fsk_set_rx_bandwidth,
    .standby                 = sx1278_fsk_standby,
    .sleep                   = sx1278_fsk_sleep,
};

/*---------------------------------------------------------------------------
 * Device instantiation macro
 *---------------------------------------------------------------------------*/

#define SX1278_FSK_DEFINE(inst)                                               \
    static struct sx1278_fsk_data sx1278_fsk_data_##inst;                     \
                                                                              \
    static const struct sx1278_fsk_config sx1278_fsk_config_##inst = {        \
        .spi       = SPI_DT_SPEC_INST_GET(inst,                               \
                         SPI_WORD_SET(8) | SPI_TRANSFER_MSB |                 \
                         SPI_OP_MODE_MASTER, 0),                              \
        .reset_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, reset_gpios,             \
                          {.port = NULL}),                                    \
        .dio0_gpio  = GPIO_DT_SPEC_INST_GET(inst, dio0_gpios),                \
        .dio1_gpio  = GPIO_DT_SPEC_INST_GET_OR(inst, dio1_gpios,              \
                          {.port = NULL}),                                    \
    };                                                                        \
                                                                              \
    DEVICE_DT_INST_DEFINE(inst,                                               \
                          sx1278_fsk_init,                                    \
                          NULL,                                               \
                          &sx1278_fsk_data_##inst,                            \
                          &sx1278_fsk_config_##inst,                          \
                          POST_KERNEL,                                        \
                          CONFIG_FSK_INIT_PRIORITY,                           \
                          &sx1278_fsk_api);

DT_INST_FOREACH_STATUS_OKAY(SX1278_FSK_DEFINE)