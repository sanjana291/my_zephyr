/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * iol_osal/include/osal_spi.h
 *
 * IO-Link OSAL SPI interface — Zephyr port.
 */

#ifndef OSAL_PL_HW_SPI_H_INCLUDED
#define OSAL_PL_HW_SPI_H_INCLUDED

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward-declare to avoid pulling <zephyr/drivers/spi.h> into every
 * stack source that includes this header. */
struct spi_dt_spec;

/** Maximum number of SPI devices (= maximum number of CCE4511 chips). */
#define IOL_SPI_MAX_DEVICES 4

/**
 * @brief Register (or update) the SPI device used by the chip called @p name.
 *
 * One call per CCE4511. @p name must have static storage duration and is the
 * key later passed as spi_slave_name to _iolink_pl_hw_spi_init().
 * @p spec must stay valid while the chip is in use.
 *
 *   static const struct spi_dt_spec spi1 =
 *       SPI_DT_SPEC_GET(DT_NODELABEL(cce4511_1), CCE4511_SPI_OPERATION, 0);
 *   iol_spi_register_device("cce4511_1", &spi1);
 *
 * @retval 0        success
 * @retval -EINVAL  NULL argument
 * @retval -EBUSY   the named device is currently open
 * @retval -ENOMEM  table full (IOL_SPI_MAX_DEVICES)
 */
int iol_spi_register_device(const char *name, const struct spi_dt_spec *spec);

/**
 * @brief Legacy single-chip helper: same as
 *        iol_spi_register_device("cce4511_0", dev).
 */
void iol_spi_set_device(const struct spi_dt_spec *dev);

/**
 * @brief Open the SPI device registered under @p spi_slave_name.
 * @param spi_slave_name  Registration name, e.g. "cce4511_1". NULL or ""
 *                        selects the legacy default "cce4511_0".
 * @return Opaque per-device handle, or NULL on error. Every chip gets its own
 *         handle and its own bus mutex.
 */
void *_iolink_pl_hw_spi_init(const char *spi_slave_name);

/**
 * @brief Release the SPI bus resources.
 * @param fd  Handle returned by _iolink_pl_hw_spi_init().
 */
void _iolink_pl_hw_spi_close(void *fd);

/**
 * @brief Full-duplex SPI transfer.
 * @param fd                   Handle from _iolink_pl_hw_spi_init().
 * @param data_read            RX buffer (NULL for TX-only).
 * @param data_written         TX buffer (NULL for RX-only).
 * @param n_bytes_to_transfer  Byte count for the exchange.
 */
void _iolink_pl_hw_spi_transfer(
    void       *fd,
    void       *data_read,
    const void *data_written,
    size_t      n_bytes_to_transfer);

#ifdef __cplusplus
}
#endif

#endif /* OSAL_PL_HW_SPI_H_INCLUDED */