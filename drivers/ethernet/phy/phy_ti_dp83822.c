/*
 * Copyright (c) 2026 Calixto Systems Pvt Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Driver for the Texas Instruments DP83822 10/100 Mbps Ethernet PHY.
 *
 * Reference: DP83822 datasheet (SNLS505G, Rev G, August 2023)
 *            Texas Instruments, https://www.ti.com/lit/ds/symlink/dp83822i.pdf
 */

#define DT_DRV_COMPAT ti_dp83822

#include <zephyr/kernel.h>
#include <zephyr/net/phy.h>
#include <zephyr/net/mii.h>
#include <zephyr/drivers/mdio.h>
#include <string.h>
#include <zephyr/sys/util_macro.h>

#define LOG_MODULE_NAME phy_ti_dp83822
#define LOG_LEVEL CONFIG_PHY_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

/* -----------------------------------------------------------------------
 * DP83822 register addresses (IEEE Clause 22, direct MDIO access)
 * ----------------------------------------------------------------------- */

/* Standard MII registers (re-use Zephyr's <zephyr/net/mii.h> defines):
 *   MII_BMCR   0x00  Basic Mode Control Register
 *   MII_BMSR   0x01  Basic Mode Status Register
 *   MII_ANAR   0x04  Auto-Negotiation Advertisement Register
 *   MII_ANLPAR 0x05  Auto-Negotiation Link Partner Ability Register
 */

/* DP83822 vendor-specific registers (Clause 22 direct space, addr 0x00-0x1F) */
#define DP83822_PHYSTS_REG          0x10  /* PHY Status Register            */
#define DP83822_RCSR_REG            0x17  /* RMII and Clock Select Register  */
#define DP83822_PHYRCR_REG          0x1F  /* PHY Reset Control Register     */

/* -----------------------------------------------------------------------
 * PHYSTS (0x10) – PHY Status Register bit definitions
 * Used for fast single-register link, speed, and duplex detection.
 * ----------------------------------------------------------------------- */
#define DP83822_PHYSTS_LINK_STATUS      BIT(0)  /* 1 = link up                */
#define DP83822_PHYSTS_SPEED_10         BIT(1)  /* 1 = 10 Mbps, 0 = 100 Mbps */
#define DP83822_PHYSTS_DUPLEX           BIT(2)  /* 1 = full duplex            */
#define DP83822_PHYSTS_AUTONEG_DONE     BIT(4)  /* 1 = auto-neg complete      */

/* -----------------------------------------------------------------------
 * RCSR (0x17) – RMII and Clock Select Register bit definitions
 * ----------------------------------------------------------------------- */
#define DP83822_RCSR_RMII_MODE_EN       BIT(5)  /* 1 = RMII mode enabled      */
#define DP83822_RCSR_RMII_MODE_SEL      BIT(7)  /* 0 = Master, 1 = Slave      */

/* -----------------------------------------------------------------------
 * PHYRCR (0x1F) – PHY Reset Control Register bit definitions
 * ----------------------------------------------------------------------- */
#define DP83822_PHYRCR_SW_RESET         BIT(15) /* Software reset (self-clear)*/
#define DP83822_PHYRCR_DIG_RESTART      BIT(14) /* Digital restart (self-clr) */

/* -----------------------------------------------------------------------
 * PHY identifier (OUI + model), used for sanity-checking at init.
 * PHYIDR1 (0x02) = 0x2000, PHYIDR2 (0x03) bits[15:4] = 0xA240 >> 4
 * Full 32-bit ID: 0x2000A240 (family mask: 0xFFFFFFF0)
 * ----------------------------------------------------------------------- */
#define DP83822_PHY_ID              0x2000A240U
#define DP83822_PHY_ID_MASK         0xFFFFFFF0U

/* -----------------------------------------------------------------------
 * Software reset completion timeout
 * Datasheet §7.8: post-reset MDC preamble must wait ≥ 2 ms (T2).
 * We poll up to ~500 ms in 1 ms steps for the self-clearing reset bit.
 * ----------------------------------------------------------------------- */
#define DP83822_SW_RESET_TIMEOUT_MS     500U
#define DP83822_SW_RESET_POLL_MS        1U

/* Post-reset stabilisation: ≥ 2 ms before first MDIO transaction (§7.8 T2) */
#define DP83822_POST_RESET_DELAY_US     2500U

/* -----------------------------------------------------------------------
 * RMII mode enum
 * ----------------------------------------------------------------------- */
enum dp83822_rmii_mode {
	DP83822_RMII_MASTER = 0, /* PHY supplies 50 MHz clock from GPIO3 */
	DP83822_RMII_SLAVE  = 1, /* PHY consumes 50 MHz clock from XI    */
};

/* -----------------------------------------------------------------------
 * Driver configuration (from Device Tree)
 * ----------------------------------------------------------------------- */
struct dp83822_config {
	uint8_t addr;
	const struct device *mdio_dev;
	enum dp83822_rmii_mode rmii_mode;
};

/* -----------------------------------------------------------------------
 * Driver runtime data
 * ----------------------------------------------------------------------- */
struct dp83822_data {
	const struct device *dev;
	struct phy_link_state state;
	phy_callback_t cb;
	void *cb_data;
	struct k_mutex mutex;
	struct k_work_delayable phy_monitor_work;
};

static int phy_dp83822_read(const struct device *dev,
			    uint16_t reg_addr, uint32_t *data)
{
	const struct dp83822_config *config = dev->config;
	int ret;

	*data = 0U;

	ret = mdio_read(config->mdio_dev, config->addr, reg_addr, (uint16_t *)data);
	if (ret) {
		return ret;
	}

	return 0;
}

static int phy_dp83822_write(const struct device *dev,
			     uint16_t reg_addr, uint32_t data)
{
	const struct dp83822_config *config = dev->config;
	int ret;

	ret = mdio_write(config->mdio_dev, config->addr, reg_addr, (uint16_t)data);
	if (ret) {
		return ret;
	}

	return 0;
}

static int phy_dp83822_autonegotiate(const struct device *dev)
{
	const struct dp83822_config *config = dev->config;
	uint32_t bmcr = 0;
	uint32_t bmsr = 0;
	uint16_t timeout = CONFIG_PHY_AUTONEG_TIMEOUT_MS / 100;
	int ret;

	ret = phy_dp83822_read(dev, MII_BMCR, &bmcr);
	if (ret) {
		LOG_ERR("PHY (%d) BMCR read failed: %d", config->addr, ret);
		return ret;
	}

	/* (Re)start auto-negotiation; clear isolate bit */
	LOG_DBG("PHY (%d) starting auto-negotiation", config->addr);
	bmcr |= MII_BMCR_AUTONEG_ENABLE | MII_BMCR_AUTONEG_RESTART;
	bmcr &= ~MII_BMCR_ISOLATE;

	ret = phy_dp83822_write(dev, MII_BMCR, bmcr);
	if (ret) {
		LOG_ERR("PHY (%d) BMCR write failed: %d", config->addr, ret);
		return ret;
	}

	/* Poll BMSR[5] (Auto-Negotiation Complete) */
	do {
		if (timeout-- == 0) {
			LOG_DBG("PHY (%d) auto-negotiation timed out",
				config->addr);
			/*
			 * Return -ENETDOWN (not -ETIMEDOUT) to signal a link
			 * timeout rather than an MDIO communication error,
			 * matching the convention used in phy_microchip_ksz8081.
			 */
			return -ENETDOWN;
		}
		k_msleep(100);

		ret = phy_dp83822_read(dev, MII_BMSR, &bmsr);
		if (ret) {
			LOG_ERR("PHY (%d) BMSR read failed: %d",
				config->addr, ret);
			return ret;
		}
	} while (!(bmsr & MII_BMSR_AUTONEG_COMPLETE));

	LOG_DBG("PHY (%d) auto-negotiation complete", config->addr);
	return 0;
}

static int phy_dp83822_get_link(const struct device *dev,
				struct phy_link_state *state)
{
	const struct dp83822_config *config = dev->config;
	struct dp83822_data *data = dev->data;
	struct phy_link_state old_state = data->state;
	uint32_t bmsr = 0;
	uint32_t anar = 0;
	uint32_t anlpar = 0;
	int ret;

	static uint8_t pr_counter = 0;


	/* Lock mutex */
	ret = k_mutex_lock(&data->mutex, K_FOREVER);
	if (ret) {
		LOG_ERR("PHY mutex lock error");
		return ret;
	}

	/* Read link state */
	ret = phy_dp83822_read(dev, MII_BMSR, &bmsr);
	if (ret) {
		LOG_ERR("Error reading phy (%d) basic status register", config->addr);
		k_mutex_unlock(&data->mutex);
		return ret;
	}
	state->is_up = bmsr & MII_BMSR_LINK_STATUS;

	if (!state->is_up) {
		/* BMSR bit 2 is a latching-low bit: one momentary blip since the
		 * last read shows as "down" here even if the link is fine again
		 * by now. Re-read BMSR (a second read returns the live value) and
		 * read the DP83822's own real-time PHYSTS register to tell a
		 * sustained drop from a latched blip, and correct state->is_up so
		 * a latched blip is never reported to the MAC driver as a real
		 * link loss. */
		uint32_t bmsr2 = 0;
		uint32_t physts = 0;

		(void)phy_dp83822_read(dev, MII_BMSR, &bmsr2);
		(void)phy_dp83822_read(dev, DP83822_PHYSTS_REG, &physts);

		if (physts & DP83822_PHYSTS_LINK_STATUS) {
			/* Latched blip: the link never actually dropped, so the
			 * negotiated speed/duplex did not change either. */
			state->is_up = true;
			state->speed = old_state.speed;
		}

		LOG_INF("PHY (%d) link-down diag: bmsr=0x%04X bmsr_reread=0x%04X "
			"physts=0x%04X -> live link is %s",
			config->addr, bmsr, bmsr2, physts,
			(physts & DP83822_PHYSTS_LINK_STATUS) ? "UP (latched blip)"
							     : "DOWN (real drop)");
		k_mutex_unlock(&data->mutex);
		goto result;
	}

	/* Read currently configured advertising options */
	ret = phy_dp83822_read(dev, MII_ANAR, &anar);
	if (ret) {
		LOG_ERR("Error reading phy (%d) advertising register", config->addr);
		k_mutex_unlock(&data->mutex);
		return ret;
	}else{
		if(pr_counter % 4 == 0){
			LOG_DBG("PHY (%d) ANAR 0x%04X", config->addr, anar);
		}
	}

	/* Read link partner capability */
	ret = phy_dp83822_read(dev, MII_ANLPAR, &anlpar);
	if (ret) {
		LOG_ERR("Error reading phy (%d) link partner register", config->addr);
		k_mutex_unlock(&data->mutex);
		return ret;
	}else{
		if(pr_counter % 4 == 0){
			LOG_DBG("PHY (%d) ANLPAR 0x%04X", config->addr, anlpar );
		}
	}

	pr_counter++;
	/* Unlock mutex */
	k_mutex_unlock(&data->mutex);

	uint32_t mutual_capabilities = anar & anlpar;

	if (mutual_capabilities & MII_ADVERTISE_100_FULL) {
		state->speed = LINK_FULL_100BASE_T;
	} else if (mutual_capabilities & MII_ADVERTISE_100_HALF) {
		state->speed = LINK_HALF_100BASE_T;
	} else if (mutual_capabilities & MII_ADVERTISE_10_FULL) {
		state->speed = LINK_FULL_10BASE_T;
	} else if (mutual_capabilities & MII_ADVERTISE_10_HALF) {
		state->speed = LINK_HALF_10BASE_T;
	} else {
		ret = -EIO;
	}

result:
	if (memcmp(&old_state, state, sizeof(struct phy_link_state)) != 0) {
		LOG_DBG("PHY %d is %s", config->addr,
			state->is_up ? "up" : "down");
		if (state->is_up) {
			LOG_DBG("PHY (%d) link %s Mb %s duplex", config->addr,
				PHY_LINK_IS_SPEED_100M(state->speed) ? "100" : "10",
				PHY_LINK_IS_FULL_DUPLEX(state->speed)? "full" : "half");
		}
	}

	return ret;
}

static int phy_dp83822_sw_reset(const struct device *dev)
{
	const struct dp83822_config *config = dev->config;
	struct dp83822_data *data = dev->data;
	uint32_t bmcr = 0;
	uint16_t timeout = DP83822_SW_RESET_TIMEOUT_MS / DP83822_SW_RESET_POLL_MS;
	int ret;

	/* Lock mutex */
	ret = k_mutex_lock(&data->mutex, K_FOREVER);
	if (ret) {
		LOG_ERR("PHY mutex lock error");
		return ret;
	}

	ret = phy_dp83822_write(dev, MII_BMCR, MII_BMCR_RESET);
	if (ret) {
		LOG_ERR("PHY (%d) software reset write failed: %d",
			config->addr, ret);
		return ret;
	}

	do {
		if (timeout-- == 0) {
			LOG_ERR("PHY (%d) software reset timed out",
				config->addr);
			return -ETIMEDOUT;
		}
		k_msleep(DP83822_SW_RESET_POLL_MS);

		ret = phy_dp83822_read(dev, MII_BMCR, &bmcr);
		if (ret) {
			LOG_ERR("PHY (%d) BMCR read during reset failed: %d",
				config->addr, ret);
			return ret;
		}
	} while (bmcr & MII_BMCR_RESET);

	k_busy_wait(DP83822_POST_RESET_DELAY_US);

	LOG_DBG("PHY (%d) software reset complete", config->addr);
	/* Unlock mutex */
	k_mutex_unlock(&data->mutex);
	return 0;
}


static int phy_dp83822_cfg_link(const struct device *dev,
				enum phy_link_speed speeds)
{
	const struct dp83822_config *config = dev->config;
	struct dp83822_data *data = dev->data;
	struct phy_link_state state = {};
	uint32_t anar = 0;
	int ret;

	ret = k_mutex_lock(&data->mutex, K_FOREVER);
	if (ret) {
		LOG_ERR("PHY (%d) mutex lock failed: %d", config->addr, ret);
		goto done;
	}

	k_work_cancel_delayable(&data->phy_monitor_work);

	ret = phy_dp83822_read(dev, MII_ANAR, &anar);
	if (ret) {
		LOG_ERR("PHY (%d) ANAR read failed: %d", config->addr, ret);
		goto done;
	}

	if (speeds & LINK_FULL_100BASE_T) {
		anar |= MII_ADVERTISE_100_FULL;
	} else {
		anar &= ~MII_ADVERTISE_100_FULL;
	}
	if (speeds & LINK_HALF_100BASE_T) {
		anar |= MII_ADVERTISE_100_HALF;
	} else {
		anar &= ~MII_ADVERTISE_100_HALF;
	}
	if (speeds & LINK_FULL_10BASE_T) {
		anar |= MII_ADVERTISE_10_FULL;
	} else {
		anar &= ~MII_ADVERTISE_10_FULL;
	}
	if (speeds & LINK_HALF_10BASE_T) {
		anar |= MII_ADVERTISE_10_HALF;
	} else {
		anar &= ~MII_ADVERTISE_10_HALF;
	}

	ret = phy_dp83822_write(dev, MII_ANAR, anar);
	if (ret) {
		LOG_ERR("PHY (%d) ANAR write failed: %d", config->addr, ret);
		goto done;
	}

	ret = phy_dp83822_autonegotiate(dev);
	if (ret && (ret != -ENETDOWN)) {
		LOG_ERR("PHY (%d) auto-negotiation error: %d",
			config->addr, ret);
		goto done;
	}

	ret = phy_dp83822_get_link(dev, &state);

	if (ret == 0 &&
	    memcmp(&state, &data->state, sizeof(struct phy_link_state)) != 0) {
		memcpy(&data->state, &state, sizeof(struct phy_link_state));
		if (data->cb) {
			data->cb(dev, &data->state, data->cb_data);
		}
	}

	LOG_INF("PHY %d is %s", config->addr,
		data->state.is_up ? "up" : "down");
	if (data->state.is_up) {
		LOG_INF("PHY (%d) link %s Mb %s duplex",
			config->addr,
			PHY_LINK_IS_SPEED_100M(data->state.speed)
				? "100" : "10",
			PHY_LINK_IS_FULL_DUPLEX(data->state.speed)
				? "full" : "half");
	}

done:
	/* Unlock mutex */
	k_mutex_unlock(&data->mutex);

	/* Start monitoring */
	k_work_reschedule(&data->phy_monitor_work,
			  K_MSEC(CONFIG_PHY_MONITOR_PERIOD));
	return ret;
}

static int phy_dp83822_link_cb_set(const struct device *dev,
				   phy_callback_t cb, void *user_data)
{
	struct dp83822_data *data = dev->data;

	data->cb = cb;
	data->cb_data = user_data;

	phy_dp83822_get_link(dev, &data->state);
	data->cb(dev, &data->state, data->cb_data);

	return 0;
}

static void phy_dp83822_monitor_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct dp83822_data *data =
		CONTAINER_OF(dwork, struct dp83822_data, phy_monitor_work);
	const struct device *dev = data->dev;
	struct phy_link_state state = {};
	int rc;

	rc = phy_dp83822_get_link(dev, &state);

	if (rc == 0 &&
	    memcmp(&state, &data->state, sizeof(struct phy_link_state)) != 0) {
		memcpy(&data->state, &state, sizeof(struct phy_link_state));
		if (data->cb) {
			data->cb(dev, &data->state, data->cb_data);
		}
	}

	/* TODO: replace polling with GPIO interrupt when hardware allows */
	k_work_reschedule(&data->phy_monitor_work,
			  K_MSEC(CONFIG_PHY_MONITOR_PERIOD));
}

/* -----------------------------------------------------------------------
 * PHY initialisation (driver probe)
 * ----------------------------------------------------------------------- */
static int phy_dp83822_init(const struct device *dev)
{
	const struct dp83822_config *config = dev->config;
	struct dp83822_data *data = dev->data;
	int ret;

	data->dev = dev;

	ret = k_mutex_init(&data->mutex);
	if (ret) {
		return ret;
	}

	mdio_bus_enable(config->mdio_dev);

	// /* Software reset clears all IEEE registers to default state */
	// ret = phy_dp83822_sw_reset(dev);
	// if (ret) {
	// 	LOG_ERR("PHY (%d) software reset failed: %d",
	// 		config->addr, ret);
	// 	return ret;
	// }

	uint16_t sor1= 0x0467;
	uint32_t sor1_data = 0;
	ret = phy_dp83822_read(dev, sor1, &sor1_data);
	LOG_INF("Before write SOR1:%d",sor1_data);
	sor1_data &= 0xff3f;
	sor1_data |= 0x0080;
	LOG_INF("After write SOR1:%d",sor1_data);

	ret = phy_dp83822_write(dev, sor1, sor1_data);

	k_work_init_delayable(&data->phy_monitor_work,
			      phy_dp83822_monitor_work_handler);

	return 0;
}


/* -----------------------------------------------------------------------
 * Ethernet PHY driver API table
 * ----------------------------------------------------------------------- */
static const struct ethphy_driver_api dp83822_phy_api = {
	.get_link    = phy_dp83822_get_link,
	.cfg_link    = phy_dp83822_cfg_link,
	.link_cb_set = phy_dp83822_link_cb_set,
	.read        = phy_dp83822_read,
	.write       = phy_dp83822_write,
};

/* -----------------------------------------------------------------------
 * Device instantiation macros
 * ----------------------------------------------------------------------- */

#define TI_DP83822_INIT(n)                                                    \
	static const struct dp83822_config dp83822_##n##_config = {           \
		.addr      = DT_INST_REG_ADDR(n),                             \
		.mdio_dev  = DEVICE_DT_GET(DT_INST_PARENT(n)),               \
		.rmii_mode = DT_INST_ENUM_IDX(n, ti_rmii_mode),              \
	};                                                                    \
                                                                              \
	static struct dp83822_data dp83822_##n##_data;                        \
                                                                              \
	DEVICE_DT_INST_DEFINE(n,                                              \
			      &phy_dp83822_init,                              \
			      NULL,                                           \
			      &dp83822_##n##_data,                            \
			      &dp83822_##n##_config,                          \
			      POST_KERNEL,                                    \
			      CONFIG_PHY_INIT_PRIORITY,                       \
			      &dp83822_phy_api);

DT_INST_FOREACH_STATUS_OKAY(TI_DP83822_INIT)