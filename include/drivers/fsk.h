/*
 * SX1278 FSK/GFSK Driver Public API
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_FSK_H_
#define ZEPHYR_INCLUDE_DRIVERS_FSK_H_

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief FSK modulation shaping options.
 */
enum fsk_mod_shaping {
    FSK_SHAPING_NONE        = 0, /**< No shaping (FSK)               */
    FSK_SHAPING_GAUSSIAN_BT1_0 = 1, /**< Gaussian BT=1.0 (GFSK)     */
    FSK_SHAPING_GAUSSIAN_BT0_5 = 2, /**< Gaussian BT=0.5 (GFSK)     */
    FSK_SHAPING_GAUSSIAN_BT0_3 = 3, /**< Gaussian BT=0.3 (GFSK)     */
};

/**
 * @brief FSK packet format.
 */
enum fsk_packet_format {
    FSK_PACKET_FIXED    = 0, /**< Fixed-length packets            */
    FSK_PACKET_VARIABLE = 1, /**< Variable-length (length byte)   */
};

/**
 * @brief DC-free encoding.
 */
enum fsk_dc_free {
    FSK_DC_FREE_NONE       = 0, /**< No encoding                   */
    FSK_DC_FREE_MANCHESTER = 1, /**< Manchester encoding            */
    FSK_DC_FREE_WHITENING  = 2, /**< Whitening (recommended)        */
};

/**
 * @brief RX address-based filtering mode.
 *
 * Address filtering is an additional check performed AFTER the sync word
 * has matched (it shares the SyncAddressMatch IRQ flag with sync
 * detection). This hardware filter is independent of, and coarser than,
 * the driver's own destination-address check that fsk_set_rx_callback()
 * relies on (see fsk_rx_callback_t) - the hardware filter is checked
 * first and applies to the raw address byte at a fixed position, before
 * the driver's own [Dest][Src][Type][Seq] header is even parsed.
 */
enum fsk_addr_filter {
    /** No address filtering - all packets that pass sync-word match are accepted. */
    FSK_ADDR_FILTER_NONE            = 0,
    /** Accept only packets whose address byte matches @ref fsk_config.node_addr. */
    FSK_ADDR_FILTER_NODE            = 1,
    /**
     * Accept packets whose address byte matches @ref fsk_config.node_addr
     * OR @ref fsk_config.broadcast_addr.
     */
    FSK_ADDR_FILTER_NODE_BROADCAST  = 2,
};

/**
 * @brief FSK configuration structure.
 *
 * Register calculation formulas (F_XOSC = 32 MHz):
 *
 *   BitRate   = F_XOSC / (RegBitrateMsb:Lsb + RegBitrateFrac/16)
 *             → RegBitrate = F_XOSC / BitRate
 *
 *   Fdev      = F_XOSC * RegFdevMsb:Lsb / 2^19
 *             → RegFdev = Fdev * 2^19 / F_XOSC
 *
 *   Frequency = F_XOSC * RegFrf / 2^19
 *             → RegFrf = Frequency * 2^19 / F_XOSC
 *
 *   RxBw      = F_XOSC / (Mant * 2^(Exp+2))
 *             See sx1278_fsk_regs.h for pre-computed values.
 *
 *   Carson rule (minimum BW): BW >= 2*(Fdev + BitRate/2)
 *                               = 2*5000 + 4800 = 14800 Hz → use 20.8 kHz
 */
struct fsk_config {
    /** Centre frequency in Hz (e.g. 433000000) */
    uint32_t frequency_hz;

    /** Bit rate in bps (e.g. 4800) */
    uint32_t bitrate_bps;

    /** Frequency deviation in Hz (e.g. 5000) */
    uint32_t fdev_hz;

    /**
     * RX channel filter bandwidth register value.
     * Use the SX1278_RX_BW_xxx constants from sx1278_fsk_regs.h.
     * E.g. SX1278_RX_BW_20_8 for 20.8 kHz.
     */
    uint8_t rx_bw_reg;

    /**
     * AFC channel filter bandwidth register value.
     * Typically 1.5–2x wider than rx_bw_reg.
     */
    uint8_t afc_bw_reg;

    /** Preamble length in bytes */
    uint16_t preamble_len;

    /** Sync word bytes. sync_len must be 1–8. */
    uint8_t sync_word[8];

    /** Number of sync word bytes (1–8). 0 = sync disabled. */
    uint8_t sync_len;

    /** Packet format: fixed or variable */
    enum fsk_packet_format packet_format;

    /** Payload length for fixed packets; max payload for variable */
    uint8_t payload_len;

    /** DC-free encoding */
    enum fsk_dc_free dc_free;

    /** Enable hardware CRC (CRC-CCITT appended/checked automatically) */
    bool crc_on;

    /** Enable AFC (Automatic Frequency Correction) */
    bool afc_on;

    /**
     * RX address-based filtering. NONE by default (no filtering).
     * Requires sync word matching to also be enabled (sync_len > 0) -
     * address filtering is checked after, and shares the same IRQ flag
     * (SyncAddressMatch) as, sync-word detection.
     */
    enum fsk_addr_filter addr_filter;

    /**
     * This device's node address.
     *
     * Used as the RegNodeAdrs value for hardware address filtering when
     * addr_filter != FSK_ADDR_FILTER_NONE, AND (regardless of addr_filter)
     * as the source address the driver stamps on every frame it
     * transmits via fsk_send_to() and as the destination address it
     * matches incoming frames against before delivering them to the RX
     * callback or accepting a driver-level ACK. Required for
     * fsk_send_to()/fsk_set_rx_callback() to work correctly.
     */
    uint8_t node_addr;

    /**
     * Broadcast address. Only used when
     * addr_filter == FSK_ADDR_FILTER_NODE_BROADCAST.
     */
    uint8_t broadcast_addr;


    /**
     * Number of retransmissions fsk_send_to() will attempt after the
     * first send if no driver-level ACK arrives in time. 0 = send once,
     * no retries. Total number of over-the-air attempts is
     * (max_retries + 1).
     */
    uint8_t max_retries;

    /** Modulation shaping */
    enum fsk_mod_shaping shaping;

    /**
     * TX output power in dBm.
     * Range: +2 to +17 dBm (PA_BOOST), or -1 to +14 dBm (RFO).
     * If >= 20, PA_DAC high-power mode is enabled (PA_BOOST only).
     */
    int8_t tx_power_dbm;

    /**
     * Use PA_BOOST pin instead of RFO.
     * Required for output power > 14 dBm on SX1278.
     */
    bool pa_boost;

    /** RX timeout in symbol periods (0 = disabled) */
    uint8_t rx_timeout;
};

/**
 * @brief FSK receive result metadata.
 */
struct fsk_rx_metadata {
    /** RSSI in dBm at time of packet reception (-RSSI/2) */
    int16_t rssi;

    /** AFC frequency correction in Hz */
    int32_t afc_hz;

    /** CRC was valid (only meaningful if crc_on=true) */
    bool crc_ok;

    /**
     * Actual number of application payload bytes received. For packets
     * delivered through the fsk_rx_callback_t path, the driver's own
     * addressing/ACK header (see fsk_rx_callback_t) has already been
     * stripped out, so this is purely the application's payload length.
     */
    uint8_t payload_len;

    /**
     * Source node address the packet was sent from. Only valid for
     * packets delivered through the fsk_rx_callback_t path (see below);
     * set to 0 otherwise.
     */
    uint8_t src_addr;
};

/**
 * @brief Callback invoked when a valid application packet is received.
 *
 * Registered via fsk_set_rx_callback(). The driver keeps the radio in
 * receive mode by default and calls this callback for every packet that:
 *   - passes the hardware sync word (and address filter, if enabled),
 *   - passes CRC (if crc_on),
 *   - is addressed to this node's fsk_config.node_addr (or the configured
 *     broadcast address), and
 *   - is an ordinary application packet, NOT a driver-level ACK.
 *
 * Driver-level ACKs are consumed entirely internally (to satisfy an
 * in-progress fsk_send_to() retry/wait, or discarded if none is
 * outstanding) and are never passed to this callback. The driver also
 * transmits its own driver-level ACK back to the sender before invoking
 * this callback, so by the time the application sees the packet the
 * sender has (almost certainly) already been acknowledged.
 *
 * @param dev        FSK device handle.
 * @param payload    Application payload bytes (driver header stripped).
 * @param len        Number of payload bytes.
 * @param meta       RSSI/AFC/CRC/source-address metadata for this packet.
 * @param user_data  Opaque pointer passed to fsk_set_rx_callback().
 *
 * @note Called from the driver's internal receive thread. Keep it short;
 *       do not block for long periods. It is safe to call fsk_send_to()
 *       from within the callback.
 */
typedef void (*fsk_rx_callback_t)(const struct device *dev,
                                  const uint8_t *payload, uint8_t len,
                                  const struct fsk_rx_metadata *meta,
                                  void *user_data);

/**
 * @brief FSK driver API.
 *
 * Accessed via the fsk_xxx() inline wrappers below.
 */
__subsystem struct fsk_driver_api {
    int (*configure)(const struct device *dev, const struct fsk_config *cfg);
    int (*send_to)(const struct device *dev, uint8_t dest_addr,
                   const uint8_t *data, uint8_t len, k_timeout_t timeout);
    int (*send)(const struct device *dev,
                   const uint8_t *data, uint8_t len, k_timeout_t timeout);
    int (*set_rx_callback)(const struct device *dev, fsk_rx_callback_t cb,
                           void *user_data);
    int (*set_frequency)(const struct device *dev, uint32_t freq_hz);
    int (*set_bitrate)(const struct device *dev, uint32_t bitrate_bps);
    int (*set_frequency_deviation)(const struct device *dev, uint32_t fdev_hz);
    int (*set_rx_bandwidth)(const struct device *dev, uint8_t rx_bw_reg);
    int (*standby)(const struct device *dev);
    int (*sleep)(const struct device *dev);
};

/**
 * @brief Configure the FSK radio.
 *
 * Must be called before any TX/RX operation. Places the device in STANDBY
 * after configuration.
 *
 * @param dev    FSK device handle.
 * @param cfg    Pointer to configuration structure.
 * @return 0 on success, negative errno on failure.
 */
__syscall int fsk_configure(const struct device *dev, const struct fsk_config *cfg);

static inline int z_impl_fsk_configure(const struct device *dev,
                                const struct fsk_config *cfg)
{
    const struct fsk_driver_api *api =
        (const struct fsk_driver_api *)dev->api;
    return api->configure(dev, cfg);
}

/**
 * @brief Transmit a packet to a specific node address, reliably.
 *
 * The driver is normally sitting in receive mode (see fsk_set_rx_callback()).
 * This call briefly takes the radio away from RX, builds a variable-length
 * frame of the form:
 *
 *     [Length][Dest][Src][Type][Seq][ application payload... ]
 *
 * (Dest = @p dest_addr, Src = this device's fsk_config.node_addr, Type
 * marks it as a data frame, Seq is an internally-managed sequence number
 * used only to match the returning ACK to this specific attempt), sends
 * it, and waits for the peer's driver-level ACK. If no ACK arrives within
 * fsk_config.ack_timeout_ms, the frame is retransmitted (same Seq) up to
 * fsk_config.max_retries times before giving up. The radio returns to
 * receive mode afterwards either way.
 *
 * Only supported in FSK_PACKET_VARIABLE mode.
 *
 * @param dev        FSK device handle.
 * @param dest_addr  Destination node address.
 * @param data       Application payload bytes.
 * @param len        Application payload length. Limited to 255 minus the
 *                   4-byte driver header (see fsk_rx_callback_t).
 * @param timeout    Per-attempt over-the-air TX timeout (not the ACK wait -
 *                   see fsk_config.ack_timeout_ms for that).
 * @return 0 if the packet was sent and driver-level ACK'd, -ETIMEDOUT if
 *         all attempts went unacknowledged, -ENOTSUP if not in
 *         FSK_PACKET_VARIABLE mode, or another negative errno.
 */
__syscall int fsk_send_to(const struct device *dev, uint8_t dest_addr,
                          const uint8_t *data, uint8_t len,
                          k_timeout_t timeout);

static inline int z_impl_fsk_send_to(const struct device *dev, uint8_t dest_addr,
                                     const uint8_t *data, uint8_t len,
                                     k_timeout_t timeout)
{
    const struct fsk_driver_api *api =
        (const struct fsk_driver_api *)dev->api;
    return api->send_to(dev, dest_addr, data, len, timeout);
}

__syscall int fsk_send(const struct device *dev,
                          const uint8_t *data, uint8_t len,
                          k_timeout_t timeout);

static inline int z_impl_fsk_send(const struct device *dev,
                                     const uint8_t *data, uint8_t len,
                                     k_timeout_t timeout)
{
    const struct fsk_driver_api *api =
        (const struct fsk_driver_api *)dev->api;
    return api->send(dev, data, len, timeout);
}

/**
 * @brief Register (or clear, with cb == NULL) the receive callback.
 *
 * Once configured (fsk_configure()), the driver runs its own internal
 * thread that keeps the radio in receive mode at all times, invoking
 * @p cb for every valid, addressed-to-us application packet it receives
 * (see fsk_rx_callback_t for exactly what that excludes). There is no
 * separate blocking or polling receive call - this callback is the only
 * way the application gets received data.
 *
 * @note This is a plain kernel-mode API, NOT a Zephyr syscall: a raw
 *       function pointer plus an opaque user_data pointer cannot be safely
 *       validated/marshaled across the user/kernel syscall boundary, so
 *       (unlike the other calls in this header) it must be called from
 *       supervisor mode.
 *
 * @param dev        FSK device handle.
 * @param cb         Callback to invoke on receive, or NULL to disable.
 * @param user_data  Opaque pointer passed back to @p cb unchanged.
 * @return 0 on success, -ENOTSUP if not in FSK_PACKET_VARIABLE mode, or
 *         another negative errno.
 */
static inline int fsk_set_rx_callback(const struct device *dev,
                                      fsk_rx_callback_t cb, void *user_data)
{
    const struct fsk_driver_api *api =
        (const struct fsk_driver_api *)dev->api;
    return api->set_rx_callback(dev, cb, user_data);
}

/**
 * @brief Set the centre frequency.
 * @param dev      FSK device handle.
 * @param freq_hz  Frequency in Hz.
 * @return 0 on success.
 */
__syscall int fsk_set_frequency(const struct device *dev, uint32_t freq_hz);

static inline int z_impl_fsk_set_frequency(const struct device *dev, uint32_t freq_hz)
{
    const struct fsk_driver_api *api =
        (const struct fsk_driver_api *)dev->api;
    return api->set_frequency(dev, freq_hz);
}

/**
 * @brief Set the bit rate.
 * @param dev         FSK device handle.
 * @param bitrate_bps Bit rate in bps.
 * @return 0 on success.
 */
__syscall int fsk_set_bitrate(const struct device *dev, uint32_t bitrate_bps);

static inline int z_impl_fsk_set_bitrate(const struct device *dev, uint32_t bitrate_bps)
{
    const struct fsk_driver_api *api =
        (const struct fsk_driver_api *)dev->api;
    return api->set_bitrate(dev, bitrate_bps);
}

/**
 * @brief Set the frequency deviation.
 * @param dev     FSK device handle.
 * @param fdev_hz Frequency deviation in Hz.
 * @return 0 on success.
 */
__syscall int fsk_set_frequency_deviation(const struct device *dev,
                                          uint32_t fdev_hz);

static inline int z_impl_fsk_set_frequency_deviation(const struct device *dev,
                                                     uint32_t fdev_hz)
{
    const struct fsk_driver_api *api =
        (const struct fsk_driver_api *)dev->api;
    return api->set_frequency_deviation(dev, fdev_hz);
}

/**
 * @brief Set the RX channel filter bandwidth.
 * @param dev       FSK device handle.
 * @param rx_bw_reg Register value (use SX1278_RX_BW_xxx constants).
 * @return 0 on success.
 */
__syscall int fsk_set_rx_bandwidth(const struct device *dev,
                                   uint8_t rx_bw_reg);

static inline int z_impl_fsk_set_rx_bandwidth(const struct device *dev,
                                             uint8_t rx_bw_reg)
{
    const struct fsk_driver_api *api =
        (const struct fsk_driver_api *)dev->api;
    return api->set_rx_bandwidth(dev, rx_bw_reg);
}

__syscall int fsk_standby(const struct device *dev);

static inline int z_impl_fsk_standby(const struct device *dev)
{
    const struct fsk_driver_api *api =
        (const struct fsk_driver_api *)dev->api;
    return api->standby(dev);
}

__syscall int fsk_sleep(const struct device *dev);

static inline int z_impl_fsk_sleep(const struct device *dev)
{
    const struct fsk_driver_api *api =
        (const struct fsk_driver_api *)dev->api;
    return api->sleep(dev);
}

#include <syscalls/fsk.h>

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_FSK_H_ */