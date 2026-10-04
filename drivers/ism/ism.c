#define DT_DRV_COMPAT calixto_ism

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>

#include "drivers/ism.h"

LOG_MODULE_REGISTER(ism_driver, CONFIG_ISM_LOG_LEVEL);

#define ISM_SAMPLE_PERIOD_MS    CONFIG_ISM_SAMPLE_PERIOD_MS
#define ISM_CONSECUTIVE_SAMPLES CONFIG_ISM_CONSECUTIVE_SAMPLES
#define ISM_THREAD_STACK_SIZE   CONFIG_ISM_THREAD_STACK_SIZE
#define ISM_THREAD_PRIORITY     CONFIG_ISM_THREAD_PRIORITY

#define ISM_MAX_CHANNELS        16U

struct ism_data {
    struct k_mutex    lock;
    struct k_thread   thread;
    k_thread_stack_t *stack;
    ism_callback_t    callback;

    uint8_t        num_channels;
    uint8_t        ism_chn_new[ISM_MAX_CHANNELS];
    uint8_t        ism_chn_old[ISM_MAX_CHANNELS];
    uint8_t        ism_count[ISM_MAX_CHANNELS];

    volatile bool     abort;
    bool              running;
};

struct ism_channel_cfg {
    struct gpio_dt_spec gpio;
};

struct ism_config {
    const struct ism_channel_cfg *channels;
    uint8_t                       num_channels;
};

void ism_status_process(const struct device *dev)
{
    struct ism_data *data = dev->data;

    k_mutex_lock(&data->lock, K_FOREVER);

    for (uint8_t ch = 0; ch < data->num_channels; ch++) {
        if (data->ism_count[ch] >= ISM_CONSECUTIVE_SAMPLES) {
            data->ism_count[ch] = 0;
            data->ism_chn_old[ch] = data->ism_chn_new[ch];

            if (data->callback) {

               data->callback(ch, data->ism_chn_new[ch]);

           }
        }
    }

    k_mutex_unlock(&data->lock);
}

/* -------------------------------------------------------------------------
 * Poll thread
 * ---------------------------------------------------------------------- */

static void ism_poll_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    const struct device     *dev  = (const struct device *)p1;
    const struct ism_config *cfg  = dev->config;
    struct ism_data         *data = dev->data;

    while (!data->abort) {

        if (data->abort) {
            break;
        }

        for (uint8_t ch = 0; ch < cfg->num_channels; ch++) {

            data->ism_chn_new[ch] = gpio_pin_get_dt(&cfg->channels[ch].gpio);


            if (data->ism_chn_old[ch] != data->ism_chn_new[ch]) {
                data->ism_count[ch]++;
            } else {
                data->ism_count[ch] = 0;
            }
        }

        ism_status_process(dev);
        // LOG_INF("ISM poll thread");
        k_msleep(ISM_SAMPLE_PERIOD_MS);
    }

    LOG_INF("ISM poll thread exiting");
}


static int _ism_init(const struct device *dev,
                     ism_callback_t       cb)
{
    const struct ism_config *cfg  = dev->config;
    struct ism_data         *data = dev->data;

    if (cb == NULL) {
        LOG_ERR("ISM_init: callback must not be NULL");
        return -EINVAL;
    }

    if (cfg->num_channels == 0U) {
        LOG_ERR("ISM_init: no channels in device-tree");
        return -ENODEV;
    }

    if (cfg->num_channels > ISM_MAX_CHANNELS) {
        LOG_ERR("ISM_init: %u channels requested, max is %u",
                cfg->num_channels, ISM_MAX_CHANNELS);
        return -EINVAL;
    }

    if (data->running) {
        LOG_WRN("ISM_init: already running - stopping first");
        data->abort = true;
        k_thread_join(&data->thread,
                      K_MSEC(2U * ISM_SAMPLE_PERIOD_MS + 50U));
        data->running = false;
        data->abort   = false;
    }

    k_mutex_lock(&data->lock, K_FOREVER);

    data->callback     = cb;
    data->abort        = false;
    data->num_channels = cfg->num_channels;

    (void)memset(data->ism_chn_new, 0, sizeof(data->ism_chn_new));
    (void)memset(data->ism_chn_old, 0, sizeof(data->ism_chn_old));
    (void)memset(data->ism_count, 0, sizeof(data->ism_count));

    for (uint8_t ch = 0; ch < cfg->num_channels; ch++) {
        const struct gpio_dt_spec *g = &cfg->channels[ch].gpio;

        if (!gpio_is_ready_dt(g)) {
            LOG_ERR("ch%u: GPIO controller not ready", ch);
            k_mutex_unlock(&data->lock);
            return -ENODEV;
        }

        int err = gpio_pin_configure_dt(g, GPIO_INPUT);

        if (err != 0) {
            LOG_ERR("ch%u: gpio_pin_configure_dt failed (err %d)", ch, err);
            k_mutex_unlock(&data->lock);
            return err;
        }

        LOG_DBG("ch%u: GPIO configured - port=%s pin=%u",
                ch, g->port->name, g->pin);
    }

    k_mutex_unlock(&data->lock);

    k_tid_t tid = k_thread_create(
        &data->thread,
        data->stack,
        ISM_THREAD_STACK_SIZE,
        ism_poll_thread,
        (void *)dev, NULL, NULL,
        ISM_THREAD_PRIORITY,
        0,
        K_NO_WAIT);

    if (tid == NULL) {
        LOG_ERR("ISM_init: k_thread_create failed");
        return -ENOMEM;
    }

    k_thread_name_set(&data->thread, "ism_poll");
    data->running = true;

    LOG_INF("ISM ready - %u ch, %u ms period, %u consec.",
            cfg->num_channels, ISM_SAMPLE_PERIOD_MS, ISM_CONSECUTIVE_SAMPLES);

    return 0;
}

static int _ism_deinit(const struct device *dev)
{
    struct ism_data *data = dev->data;

    if (!data->running) {
        LOG_WRN("ISM_deinit: not running");
        return 0;
    }

    data->abort = true;

    int err = k_thread_join(&data->thread,
                            K_MSEC(2U * ISM_SAMPLE_PERIOD_MS + 50U));
    if (err != 0) {
        LOG_WRN("ISM_deinit: thread join timed out (err %d)", err);
    }

    k_mutex_lock(&data->lock, K_FOREVER);
    data->callback = NULL;
    data->running  = false;
    k_mutex_unlock(&data->lock);

    LOG_INF("ISM deinitialized");
    return 0;
}

static int _ism_rd_chnl_state(const struct device *dev,
                               uint8_t              chnl,
                               uint16_t            *chnl_state)
{
    const struct ism_config *cfg  = dev->config;
    struct ism_data         *data = dev->data;

    if (chnl_state == NULL) {
        return -EINVAL;
    }

    if (chnl >= cfg->num_channels) {
        LOG_ERR("rd_chnl_state: ch%u out of range (max %u)",
                chnl, cfg->num_channels - 1U);
        return -ENODEV;
    }

    k_mutex_lock(&data->lock, K_FOREVER);


    if (data->ism_count[chnl] == 0U && data->ism_chn_old[chnl] == data->ism_chn_new[chnl]
        && !data->running) {
        k_mutex_unlock(&data->lock);
        return -EAGAIN;
    }

    *chnl_state = (uint16_t)data->ism_chn_old[chnl];

    k_mutex_unlock(&data->lock);

    LOG_DBG("rd_chnl_state: ch%u = %u", chnl, (unsigned)*chnl_state);
    return 0;
}


static const struct ism_driver_api ism_api = {
    .init          = _ism_init,
    .deinit        = _ism_deinit,
    .rd_chnl_state = _ism_rd_chnl_state,
};


static int ism_device_init_fn(const struct device *dev)
{
    const struct ism_config *cfg = dev->config;

    if (cfg->num_channels > ISM_MAX_CHANNELS) {
        LOG_ERR("ISM: %u channels exceeds hard limit of %u",
                cfg->num_channels, ISM_MAX_CHANNELS);
        return -EINVAL;
    }

    for (uint8_t ch = 0; ch < cfg->num_channels; ch++) {
        if (!gpio_is_ready_dt(&cfg->channels[ch].gpio)) {
            LOG_ERR("ch%u: GPIO controller not ready at boot", ch);
            return -ENODEV;
        }
    }

    LOG_DBG("ISM early-init OK (%u channels)", cfg->num_channels);
    return 0;
}


#define ISM_CHANNEL_INIT(node_id) \
    { .gpio = GPIO_DT_SPEC_GET(node_id, gpios) },

#define ISM_DEVICE_INIT(inst)                                                   \
    K_THREAD_STACK_DEFINE(ism_thread_stack_##inst,                              \
                          CONFIG_ISM_THREAD_STACK_SIZE);                        \
                                                                                \
    static const struct ism_channel_cfg ism_channels_##inst[] = {               \
        DT_INST_FOREACH_CHILD(inst, ISM_CHANNEL_INIT)                           \
    };                                                                          \
    static const struct ism_config ism_cfg_##inst = {                           \
        .channels     = ism_channels_##inst,                                    \
        .num_channels = ARRAY_SIZE(ism_channels_##inst),                        \
    };                                                                          \
    static struct ism_data ism_data_##inst = {                                  \
        .lock  = Z_MUTEX_INITIALIZER(ism_data_##inst.lock),                     \
        .stack = ism_thread_stack_##inst,                                       \
    };                                                                          \
    DEVICE_DT_INST_DEFINE(inst,                                                 \
                          ism_device_init_fn,                                   \
                          NULL,                                                 \
                          &ism_data_##inst,                                     \
                          &ism_cfg_##inst,                                      \
                          POST_KERNEL,                                          \
                          CONFIG_ISM_INIT_PRIORITY,                             \
                          &ism_api);

DT_INST_FOREACH_STATUS_OKAY(ISM_DEVICE_INIT)