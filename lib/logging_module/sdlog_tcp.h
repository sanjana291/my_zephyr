/*
 * Copyright (c) 2026 Calixto System
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file sdlog_tcp.h
 * @brief Internal header for the TCP server + TFTP client integration
 *        inside the sdlog module.
 *
 * Never installed or exported. Only sdlog_tcp.c and sdlog_tftp.c
 * include this header. The application must never include it.
 */

#ifndef SDLOG_TCP_H_
#define SDLOG_TCP_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <zephyr/net/net_ip.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* MTP message type string constants                                   */
/* ------------------------------------------------------------------ */

#define SDLOG_MTP_ACK_NACK              "01"
#define SDLOG_MTP_SECURITY_SEQUENCE     "02"
#define SDLOG_MTP_LOG_FILES_INFO_REQ    "07"
#define SDLOG_MTP_LOG_FILES_INFO_RESP   "08"
#define SDLOG_MTP_LOG_READ_REQ          "09"
#define SDLOG_MTP_LOG_READ_AVAIL_RESP   "10"
#define SDLOG_MTP_LOG_TRANSFER_STATUS   "11"
#define SDLOG_MTP_LOG_READ_RESULT       "12"

/* ------------------------------------------------------------------ */
/* MTP 11  {"MTP":"11","TSA":x}  -  Transfer Status                   */
/* ------------------------------------------------------------------ */

/** TSA=0: TFTP server available and transmission started. */
#define SDLOG_TSA_AVAILABLE             0
/** TSA=1: TFTP server not available. */
#define SDLOG_TSA_NOT_AVAILABLE         1

/* ------------------------------------------------------------------ */
/* MTP 12  {"MTP":"12","STS":x,"FNC":y,"FNO":z}  -  Read Result      */
/* ------------------------------------------------------------------ */

/** STS=0: All files in the batch transferred successfully. FNO must be 0. */
#define SDLOG_RESULT_SUCCESS            0
/** STS=1: Transfer failed. FNO holds the 1-based number of the failing file. */
#define SDLOG_RESULT_FAIL               1

/* ------------------------------------------------------------------ */
/* Date array format: [DD, MM, YYYY] used in MTP 08 / 09              */
/* ------------------------------------------------------------------ */

struct sdlog_proto_date {
	uint8_t  day;    /* 1-31   */
	uint8_t  month;  /* 1-12   */
	uint16_t year;   /* 0-0xFFFF */
};

/* ------------------------------------------------------------------ */
/* sdlog_tcp.c - module-internal API                                   */
/* ------------------------------------------------------------------ */

int sdlog_tcp_server_start(void);
int sdlog_tcp_server_stop(void);

/**
 * @brief Send a NUL-terminated JSON string to the connected TCP client.
 * @retval >=0  Bytes sent.
 * @retval -ENOTCONN  No client connected.
 */
int sdlog_tcp_send(const char *json);

/* ------------------------------------------------------------------ */
/* sdlog_tftp.c - module-internal API                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief Transfer one log file to the TFTP server.
 *
 * Does NOT send any MTP packet. Returns 0 on success or a negative
 * errno on failure. The caller (sdlog_tcp.c) is responsible for all
 * MTP 11 / MTP 12 messaging.
 *
 * Special return value:
 *   -EHOSTUNREACH  TFTP server could not be reached (DNS failure or
 *                  tftp_put() returned an error). Caller must send
 *                  MTP 11 TSA=1 before MTP 12 STS=1.
 *
 * Must NOT be called while holding sdlog.lock; uses SDLOG_File_*
 * public API which acquires the lock internally.
 *
 * @param filename    Plain file name (no path prefix).
 * @param file_number 1-based index of this file in the current batch
 *                    (used only for log messages).
 * @param total_files Total files in the batch (used only for log messages).
 */
int sdlog_tftp_transfer_file(const char *filename,
			      uint32_t file_number,
			      uint32_t total_files);

#ifdef __cplusplus
}
#endif

#endif /* SDLOG_TCP_H_ */