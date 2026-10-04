/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * src/osal/osal_spi.c - SPI OSAL implementation for the IO-Link Master,
 * Zephyr port.  Supports several CCE4511 chips: every chip has its own
 * spi_dt_spec and its own bus mutex.
 */

#include "iol-master/osal_spi.h"
#include "iol-master/osal_spi_internal.h"
#include "iol-master/osal.h"
#include "iol-master/cce4511_conf.h"
#include "iol-master/iol_log.h"

#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Name used by the legacy single-chip iol_spi_set_device() entry point. */
#define IOL_SPI_DEFAULT_NAME "cce4511_0"

/* -- Per-device state ---------------------------------------------------- */

struct iol_spi_dev {
    const char               *name;   /* registration key (static string)   */
    const struct spi_dt_spec *spec;   /* supplied by iol_spi_register_*()   */
    os_mutex_t               *mtx;    /* serialises transfers on this chip  */
    bool                      open;
};

static struct iol_spi_dev s_spi_devs[IOL_SPI_MAX_DEVICES];

static struct iol_spi_dev *spi_find(const char *name)
{
    for (size_t i = 0; i < IOL_SPI_MAX_DEVICES; i++) {
        if (s_spi_devs[i].name != NULL &&
            strcmp(s_spi_devs[i].name, name) == 0) {
            return &s_spi_devs[i];
        }
    }
    return NULL;
}

/* -- Registration -------------------------------------------------------- */

int iol_spi_register_device(const char *name, const struct spi_dt_spec *spec)
{
    if ((name == NULL) || (spec == NULL)) {
        return -EINVAL;
    }

    struct iol_spi_dev *dev = spi_find(name);

    if (dev == NULL) {
        for (size_t i = 0; i < IOL_SPI_MAX_DEVICES; i++) {
            if (s_spi_devs[i].name == NULL) {
                dev = &s_spi_devs[i];
                break;
            }
        }
        if (dev == NULL) {
            return -ENOMEM;
        }
        dev->name = name;
    } else if (dev->open) {
        return -EBUSY;
    }

    dev->spec = spec;
    return 0;
}

void iol_spi_set_device(const struct spi_dt_spec *dev)
{
    (void)iol_spi_register_device(IOL_SPI_DEFAULT_NAME, dev);
}

/* -- OSAL SPI interface -------------------------------------------------- */

/**
 * @brief Open the SPI device registered under @p spi_slave_name.
 *
 * The SPI controller itself is configured by the devicetree / Zephyr SPI
 * driver; this only validates readiness and creates the per-device mutex.
 *
 * @param spi_slave_name  Registration name (e.g. "cce4511_1").  NULL or ""
 *                        selects the legacy default ("cce4511_0").
 * @return Opaque handle for _iolink_pl_hw_spi_transfer(), or NULL on error.
 */
void *_iolink_pl_hw_spi_init(const char *spi_slave_name)
{
    const char *name = ((spi_slave_name != NULL) && (spi_slave_name[0] != '\0'))
                       ? spi_slave_name : IOL_SPI_DEFAULT_NAME;

    struct iol_spi_dev *dev = spi_find(name);

    if ((dev == NULL) || (dev->spec == NULL)) {
        IOL_LOG_ERR("osal_spi: '%s' not registered - call "
                    "iol_spi_register_device() before init", name);
        return NULL;
    }

    if (dev->open) {
        IOL_LOG_ERR("osal_spi: '%s' already open", name);
        return NULL;
    }

    if (!spi_is_ready_dt(dev->spec)) {
        IOL_LOG_ERR("osal_spi: SPI device '%s' not ready", name);
        return NULL;
    }

    dev->mtx = os_mutex_create();
    if (dev->mtx == NULL) {
        IOL_LOG_ERR("osal_spi: failed to create SPI mutex for '%s'", name);
        return NULL;
    }

    dev->open = true;

    IOL_LOG_INF("osal_spi: '%s' ready @ %u Hz", name,
                dev->spec->config.frequency);

    return dev;
}

/**
 * @brief Release a device opened with _iolink_pl_hw_spi_init().
 *
 * The registration itself is kept so the chip can be opened again after a
 * Master_deinit()/Master_init() cycle.
 */
void _iolink_pl_hw_spi_close(void *fd)
{
    struct iol_spi_dev *dev = (struct iol_spi_dev *)fd;

    if (dev == NULL) {
        return;
    }

    if (dev->mtx != NULL) {
        os_mutex_destroy(dev->mtx);
        dev->mtx = NULL;
    }

    dev->open = false;
    IOL_LOG_INF("osal_spi: '%s' closed", dev->name);
}

/**
 * @brief Full-duplex SPI transfer on one chip.
 *
 * Thread safety is per chip (os_mutex).  Two chips on different SPI
 * controllers transfer in parallel; chips sharing a controller are
 * serialised by the Zephyr SPI driver itself.
 */
void _iolink_pl_hw_spi_transfer(
    void       *fd,
    void       *data_read,
    const void *data_written,
    size_t      n_bytes_to_transfer)
{
    struct iol_spi_dev *dev = (struct iol_spi_dev *)fd;

    if ((dev == NULL) || (!dev->open) || (dev->spec == NULL)) {
        IOL_LOG_ERR("osal_spi: transfer on a closed/NULL device");
        return;
    }

    const struct spi_buf tx_buf = {
        .buf = (void *)data_written,   /* cast away const: Zephyr API quirk */
        .len = n_bytes_to_transfer,
    };
    struct spi_buf rx_buf = {
        .buf = data_read,
        .len = n_bytes_to_transfer,
    };

    const struct spi_buf_set tx_set = {
        .buffers = (data_written != NULL) ? &tx_buf : NULL,
        .count   = (data_written != NULL) ? 1U       : 0U,
    };
    const struct spi_buf_set rx_set = {
        .buffers = (data_read != NULL) ? &rx_buf : NULL,
        .count   = (data_read != NULL) ? 1U      : 0U,
    };

    os_mutex_lock(dev->mtx);

    int rc = spi_transceive_dt(dev->spec, &tx_set, &rx_set);
    if (rc != 0) {
        IOL_LOG_ERR("osal_spi: '%s' spi_transceive_dt failed (%d)",
                    dev->name, rc);
    }

    os_mutex_unlock(dev->mtx);
}
