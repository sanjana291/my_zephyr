/*
 * Copyright (c) 2026 Calixto System Pvt Ltd
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DRIVERS_RMC_BACKENDS_SUBGHZ_RMC_SUBGHZ_H_
#define DRIVERS_RMC_BACKENDS_SUBGHZ_RMC_SUBGHZ_H_

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <drivers/fsk.h>
#include "rmc_subghz_protocol.h"
#include "../rmc_backend.h"


/* -------------------------------------------------------------------- */
/* Hardware resource struct (populated by rmc.c from Devicetree)        */
/* -------------------------------------------------------------------- */

struct rmc_subghz_hw {
	const struct device *radio; /* calixto,sx127x-fsk device */
};


/* -------------------------------------------------------------------- */
/* Node addresses (pairing channel)                                      */
/* -------------------------------------------------------------------- */

/** Node address during pairing. */
#define RMC_SG_PAIRING_ADDR_SHUTTLE      0x02U

/** Remote node address during pairing. */
#define RMC_SG_PAIRING_ADDR_REMOTE 0x01U

/* -------------------------------------------------------------------- */
/* Pairing state machine states                                          */
/* -------------------------------------------------------------------- */

enum rmc_sg_state {
	RMC_SG_STATE_IDLE = 0,
	/* Pairing mode */
	RMC_SG_STATE_PAIRING_BEACON,    /* transmitting beacons every 500ms */
	RMC_SG_STATE_PAIRING_WAIT_AUTH, /* received auth req, sent conf, wait cfg */
	RMC_SG_STATE_PAIRING_WAIT_CFG,  /* sent auth conf, waiting for cfg write */

	RMC_SG_STATE_PAIRING_WAIT_STS,

	RMC_SG_STATE_PAIRING_IDLE,

	/* Working mode */
	RMC_SG_STATE_WORKING_WAIT_CONN, /* waiting for shuttle connection request */
	RMC_SG_STATE_WORKING_READY,     /* fully operational */
	/* Terminal */
	RMC_SG_STATE_STOPPED,
};

/* -------------------------------------------------------------------- */
/* Working configuration (received from Shuttle during pairing)         */
/* -------------------------------------------------------------------- */

struct rmc_sg_working_cfg {
	uint8_t  network_id[8];       /* also used directly as the radio sync
					* word (RMC_SG_SYNC_LEN bytes) - see
					* radio_configure() in rmc_subghz.c */
	uint8_t  channel_id;          /* 0–5 only */
	uint8_t  node_id;
	uint8_t  remote_id;
	uint8_t  remote_uid[RMC_SG_REMOTE_UID_LEN];
	uint16_t fw_ver;
	uint16_t hw_ver;
	bool     valid;
};


/* -------------------------------------------------------------------- */
/* Event types posted to the state-machine thread queue                 */
/* -------------------------------------------------------------------- */

enum rmc_sg_sm_evt {
	RMC_SG_EVT_RX_PACKET,
	RMC_SG_EVT_BEACON_TICK,             /* 500ms beacon timer expired          */
	RMC_SG_EVT_PAIRING_TIMEOUT,         /* 5-second beacon window expired      */
	RMC_SG_EVT_SCAN_RESP_TIMEOUT,
	RMC_SG_EVT_PAIRING_EXCHANGE_TIMEOUT,/* 20-second full exchange timeout     */
	RMC_SG_EVT_TX_REQUEST,              /* app TX without ACK                  */
	RMC_SG_EVT_DEINIT,
};

struct rmc_sg_sm_msg {
	enum rmc_sg_sm_evt evt;
	union {
		struct {
			uint8_t buf[RMC_SG_MAX_FRAME_LEN];
			uint8_t len;
			uint8_t src_addr;
		} rx;
		struct {
			uint8_t  data[RMC_SG_MAX_DATA_PLD];
			uint8_t  data_len;
		} tx;
	};
};

/* -------------------------------------------------------------------- */
/* Backend private runtime state                                         */
/* -------------------------------------------------------------------- */

struct rmc_subghz_data {
	const struct device    *dev;
	const struct rmc_subghz_hw *hw;

	struct rmc_subghz_init_cfg app_cfg;

	/* State machine. */
	enum rmc_sg_state state;
	atomic_t          ready;

	/* Device Unique ID (read from hardware at init). */
	uint8_t device_uid[RMC_SG_DEVICE_UID_LEN];

	/* UID of the REMOTE detected during pairing. */
	uint8_t remote_uid[RMC_SG_REMOTE_UID_LEN];
	bool    remote_uid_known;
	bool 	scan_resp_extended;

	/* Working configuration received from shuttle. */
	struct rmc_sg_working_cfg working_cfg;

	/* Timers. */
	struct k_work_delayable beacon_tick_work;
	struct k_work_delayable pairing_timeout_work;          /* 5-s beacon window     */
	struct k_work_delayable pairing_exchange_timeout_work; /* 20-s exchange window  */
	struct k_work_delayable scan_resp_timeout_work;
	/* Threads. */
	struct k_thread sm_thread;

	/* Event queue (state-machine thread). */
	struct k_msgq sm_msgq;
	char          sm_msgq_buf[8 * sizeof(struct rmc_sg_sm_msg)];

    struct k_sem tx_done_sem;     /* Semaphore to signal TX completion */
	int tx_result;                /* Store result of fsk_send_to() */

	rmc_callback_t callback;
};

/* Stack sizes — configured via Kconfig. */
#define RMC_SUBGHZ_SM_STACK_SIZE CONFIG_RMC_SUBGHZ_SM_STACK_SIZE
#define RMC_SUBGHZ_RX_STACK_SIZE CONFIG_RMC_SUBGHZ_RX_STACK_SIZE

/** Get the SubGHz backend vtable. */
const struct rmc_backend_api *rmc_subghz_backend_get(void);

#endif /* DRIVERS_RMC_BACKENDS_SUBGHZ_RMC_SUBGHZ_H_ */