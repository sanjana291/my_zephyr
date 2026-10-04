/*
 * SX1278 FSK/GFSK Register Definitions
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Full register map for FSK/OOK mode operation.
 * Datasheet: Semtech SX1276/77/78/79 Rev 7
 */

#ifndef ZEPHYR_DRIVERS_FSK_SX1278_FSK_REGS_H_
#define ZEPHYR_DRIVERS_FSK_SX1278_FSK_REGS_H_

/*---------------------------------------------------------------------------
 * SPI access helpers
 *   Bit 7 = 1 → write, 0 → read
 *---------------------------------------------------------------------------*/
#define SX1278_SPI_WRITE_BIT   0x80U
#define SX1278_SPI_READ_MASK   0x7FU

/*===========================================================================
 * Common registers (shared FSK / LoRa)
 *===========================================================================*/
#define SX1278_REG_FIFO                  0x00
#define SX1278_REG_OP_MODE               0x01
#define SX1278_REG_BITRATE_MSB           0x02
#define SX1278_REG_BITRATE_LSB           0x03
#define SX1278_REG_FDEV_MSB              0x04
#define SX1278_REG_FDEV_LSB              0x05
#define SX1278_REG_FRF_MSB               0x06
#define SX1278_REG_FRF_MID               0x07
#define SX1278_REG_FRF_LSB               0x08
/*
 * BUG FIX: 0x09 is RegPaConfig per the SX1276/77/78/79 datasheet (Rev 4).
 * The previous definition of SX1278_REG_OSC at 0x09 was wrong - RegOsc is
 * at 0x24 and is already defined below as SX1278_REG_OSC_REG.  The
 * incorrect SX1278_REG_OSC alias is removed to prevent accidental PA_CONFIG
 * writes through a misnamed symbol.
 * Note: 0x20/0x21 are LoRa preamble registers; use 0x25/0x26 for FSK.
 */
#define SX1278_REG_PA_CONFIG             0x09
#define SX1278_REG_PREAMBLE_MSB          0x25  /* FSK preamble length MSB  */
#define SX1278_REG_PREAMBLE_LSB          0x26  /* FSK preamble length LSB  */
/* power amplifier / misc */
#define SX1278_REG_PA_RAMP               0x0A
#define SX1278_REG_OCP                   0x0B
#define SX1278_REG_LNA                   0x0C

/*===========================================================================
 * FSK / OOK specific registers
 *===========================================================================*/
/* Rx config */
#define SX1278_REG_RX_CONFIG             0x0D
#define SX1278_REG_RSSI_CONFIG           0x0E
#define SX1278_REG_RSSI_COLLISION        0x0F
#define SX1278_REG_RSSI_THRESH           0x10
#define SX1278_REG_RSSI_VALUE            0x11

/* Channel filter bandwidth */
#define SX1278_REG_RX_BW                 0x12
#define SX1278_REG_AFC_BW                0x13

/* OOK */
#define SX1278_REG_OOK_PEAK              0x14
#define SX1278_REG_OOK_FIX               0x15
#define SX1278_REG_OOK_AVG               0x16

/* AFC / FEI */
#define SX1278_REG_AFC_FEI               0x1A
#define SX1278_REG_AFC_MSB               0x1B
#define SX1278_REG_AFC_LSB               0x1C
#define SX1278_REG_FEI_MSB               0x1D
#define SX1278_REG_FEI_LSB               0x1E

/* Preamble detector */
#define SX1278_REG_PREAMBLE_DETECT       0x1F

/* Timeouts */
#define SX1278_REG_RX_TIMEOUT1           0x20
#define SX1278_REG_RX_TIMEOUT2           0x21
#define SX1278_REG_RX_TIMEOUT3           0x22
#define SX1278_REG_RX_DELAY              0x23

/* Oscillator */
#define SX1278_REG_OSC_REG               0x24

/* Preamble length (FSK) */
#define SX1278_REG_PREAMBLE_MSB_FSK      0x25
#define SX1278_REG_PREAMBLE_LSB_FSK      0x26

/* Sync word */
#define SX1278_REG_SYNC_CONFIG           0x27
#define SX1278_REG_SYNC_VALUE1           0x28
#define SX1278_REG_SYNC_VALUE2           0x29
#define SX1278_REG_SYNC_VALUE3           0x2A
#define SX1278_REG_SYNC_VALUE4           0x2B
#define SX1278_REG_SYNC_VALUE5           0x2C
#define SX1278_REG_SYNC_VALUE6           0x2D
#define SX1278_REG_SYNC_VALUE7           0x2E
#define SX1278_REG_SYNC_VALUE8           0x2F

/* Packet config */
#define SX1278_REG_PACKET_CONFIG1        0x30
#define SX1278_REG_PACKET_CONFIG2        0x31
#define SX1278_REG_PAYLOAD_LENGTH        0x32
#define SX1278_REG_NODE_ADRS             0x33
#define SX1278_REG_BROADCAST_ADRS        0x34
#define SX1278_REG_FIFO_THRESH           0x35
#define SX1278_REG_SEQ_CONFIG1           0x36
#define SX1278_REG_SEQ_CONFIG2           0x37
#define SX1278_REG_TIMER_RESOL           0x38
#define SX1278_REG_TIMER1_COEF           0x39
#define SX1278_REG_TIMER2_COEF           0x3A
#define SX1278_REG_IMAGE_CAL             0x3B
#define SX1278_REG_TEMP                  0x3C
#define SX1278_REG_LOW_BAT               0x3D

/* IRQ flags */
#define SX1278_REG_IRQ_FLAGS1            0x3E
#define SX1278_REG_IRQ_FLAGS2            0x3F

/* DIO mapping */
#define SX1278_REG_DIO_MAPPING1          0x40
#define SX1278_REG_DIO_MAPPING2          0x41

/* Version */
#define SX1278_REG_VERSION               0x42

/* TCXO, PA DAC, former temp, */
#define SX1278_REG_PLL_HOP               0x44
#define SX1278_REG_TCXO                  0x4B
#define SX1278_REG_PA_DAC                0x4D
#define SX1278_REG_FORMER_TEMP           0x5B
#define SX1278_REG_BITRATE_FRAC          0x5D
#define SX1278_REG_AGC_REF               0x61
#define SX1278_REG_AGC_THRESH1           0x62
#define SX1278_REG_AGC_THRESH2           0x63
#define SX1278_REG_AGC_THRESH3           0x64
#define SX1278_REG_PLL                   0x70

/*===========================================================================
 * RegOpMode  (0x01) bit fields
 *===========================================================================*/
#define SX1278_OPMODE_LONG_RANGE_MODE    BIT(7) /* 1=LoRa, 0=FSK/OOK        */
#define SX1278_OPMODE_MODULATION_TYPE_SHIFT 5
#define SX1278_OPMODE_MODULATION_TYPE_MSK  (0x3U << 5)
#define SX1278_OPMODE_MOD_FSK            (0x0U << 5)
#define SX1278_OPMODE_MOD_OOK            (0x1U << 5)
#define SX1278_OPMODE_LOW_FREQUENCY_MODE  BIT(3)
/* Mode bits [2:0] */
#define SX1278_OPMODE_MASK               0x07U
#define SX1278_OPMODE_SLEEP              0x00U
#define SX1278_OPMODE_STANDBY            0x01U
#define SX1278_OPMODE_FSTX               0x02U
#define SX1278_OPMODE_TX                 0x03U
#define SX1278_OPMODE_FSRX               0x04U
#define SX1278_OPMODE_RXCONTINUOUS       0x05U
// #define SX1278_OPMODE_RXSINGLE           0x06U
// #define SX1278_OPMODE_CAD                0x07U /* FSK: not used */

/*===========================================================================
 * RegPaConfig (0x09)
 *===========================================================================*/
#define SX1278_PA_SELECT_BIT             BIT(7)  /* 1=PA_BOOST, 0=RFO        */
#define SX1278_PA_MAX_POWER_SHIFT        4
#define SX1278_PA_MAX_POWER_MSK          (0x7U << 4)
#define SX1278_PA_OUTPUT_POWER_MSK       0x0FU

/*===========================================================================
 * RegPaDac (0x4D)
 *===========================================================================*/
#define SX1278_PA_DAC_DEFAULT            0x84U
#define SX1278_PA_DAC_20DBM              0x87U

/*===========================================================================
 * RegOcp (0x0B)
 *===========================================================================*/
#define SX1278_OCP_ON                    BIT(5)
#define SX1278_OCP_TRIM_MSK              0x1FU

/*===========================================================================
 * RegRxConfig (0x0D)
 *===========================================================================*/
#define SX1278_RX_RESTART                BIT(7)
#define SX1278_AFC_AUTO_ON               BIT(4)
#define SX1278_AGC_AUTO_ON               BIT(3)
#define SX1278_RX_TRIGGER_PREAMBLE_DET   0x06U
#define SX1278_RX_TRIGGER_SYNC_ADDR      0x07U

/*===========================================================================
 * RegRxBw (0x12) and RegAfcBw (0x13)
 *
 * BW = F_XOSC / (RxBwMant * 2^(RxBwExp+2))
 * RxBwMant: 0=16, 1=20, 2=24
 *
 *  Encoding:  bits [4:3] = Mant, bits [2:0] = Exp
 *===========================================================================*/
#define SX1278_RXBW_MANT_SHIFT           3
#define SX1278_RXBW_EXP_MASK             0x07U

/* Pre-computed common BW register values (F_XOSC=32 MHz) */
/* BW = 2.6 kHz  */ #define SX1278_RX_BW_2_6    0x17U
/* BW = 3.1 kHz  */ #define SX1278_RX_BW_3_1    0x0FU
/* BW = 3.9 kHz  */ #define SX1278_RX_BW_3_9    0x07U
/* BW = 5.2 kHz  */ #define SX1278_RX_BW_5_2    0x16U
/* BW = 6.3 kHz  */ #define SX1278_RX_BW_6_3    0x0EU
/* BW = 7.8 kHz  */ #define SX1278_RX_BW_7_8    0x06U
/* BW = 10.4 kHz */ #define SX1278_RX_BW_10_4   0x15U
/* BW = 12.5 kHz */ #define SX1278_RX_BW_12_5   0x0DU
/* BW = 15.6 kHz */ #define SX1278_RX_BW_15_6   0x05U
/* BW = 20.8 kHz */ #define SX1278_RX_BW_20_8   0x14U  /* ← default config  */
/* BW = 25.0 kHz */ #define SX1278_RX_BW_25_0   0x0CU
/* BW = 31.3 kHz */ #define SX1278_RX_BW_31_3   0x04U
/* BW = 41.7 kHz */ #define SX1278_RX_BW_41_7   0x13U
/* BW = 50.0 kHz */ #define SX1278_RX_BW_50_0   0x0BU
/* BW = 62.5 kHz */ #define SX1278_RX_BW_62_5   0x03U
/* BW = 83.3 kHz */ #define SX1278_RX_BW_83_3   0x12U
/* BW = 100 kHz  */ #define SX1278_RX_BW_100    0x0AU
/* BW = 125 kHz  */ #define SX1278_RX_BW_125    0x02U
/* BW = 166.7 kHz*/ #define SX1278_RX_BW_166_7  0x11U
/* BW = 200 kHz  */ #define SX1278_RX_BW_200    0x09U
/* BW = 250 kHz  */ #define SX1278_RX_BW_250    0x01U

/*===========================================================================
 * RegAfcFei (0x1A)
 *===========================================================================*/
#define SX1278_AFC_CLEAR                 BIT(1)
#define SX1278_AFC_START                 BIT(0)
#define SX1278_FEI_START                 BIT(2)

/*===========================================================================
 * RegPreambleDetect (0x1F)
 *===========================================================================*/
#define SX1278_PREAMBLE_DETECTOR_ON      BIT(7)
#define SX1278_PREAMBLE_DETECTOR_2BYTES  (0x1U << 5)
#define SX1278_PREAMBLE_DETECTOR_TOL_MSK 0x1FU

/*===========================================================================
 * RegSyncConfig (0x27)
 *===========================================================================*/
#define SX1278_SYNC_ON                   BIT(4)
#define SX1278_PREAMBLE_POLARITY_55      BIT(5)  /* 0=0xAA, 1=0x55          */
#define SX1278_SYNC_SIZE_SHIFT           0
#define SX1278_SYNC_SIZE_MSK             0x07U   /* sync_size = value+1      */

/*===========================================================================
 * RegPacketConfig1 (0x30)
 *===========================================================================*/
#define SX1278_PACKET_FORMAT_VARIABLE    BIT(7)  /* 0=fixed, 1=variable      */
#define SX1278_DC_FREE_WHITENING         (0x2U << 5)
#define SX1278_DC_FREE_MANCHESTER        (0x1U << 5)
#define SX1278_DC_FREE_NONE              (0x0U << 5)
#define SX1278_CRC_ON                    BIT(4)
#define SX1278_CRC_AUTO_CLEAR_OFF        BIT(3)
#define SX1278_ADDRESS_FILTERING_NONE    (0x0U << 1)
#define SX1278_ADDRESS_FILTERING_NODE    (0x1U << 1)
#define SX1278_ADDRESS_FILTERING_BC      (0x2U << 1)
#define SX1278_CRC_WHITENING_CCITT       BIT(0)  /* 0=CCITT, 1=IBM           */

/*===========================================================================
 * RegPacketConfig2 (0x31)
 *===========================================================================*/
#define SX1278_DATA_MODE_PACKET          BIT(6)  /* 0=continuous, 1=packet   */
#define SX1278_IO_HOME_ON                BIT(5)
#define SX1278_BEACON_ON                 BIT(3)
#define SX1278_PAYLOAD_LEN_MSB_MASK      0x07U   /* bits [2:0] = MSB of len  */

/*===========================================================================
 * RegFifoThresh (0x35)
 *
 * Bit 7 – TxStartCondition (per datasheet RegFifoThresh, 0x35):
 *   0 = start TX only when FIFO level EXCEEDS FifoThreshold ("FifoLevel")
 *   1 = start TX as soon as FIFO is NOT EMPTY ("FifoEmpty goes low")
 *       ← use this for packet mode
 *
 * NOTE: earlier revisions of this header had these two bit values swapped.
 * Semtech's own reference FSK driver defines
 * RF_FIFOTHRESH_TXSTARTCONDITION_FIFONOTEMPTY as 0x80 (bit7=1), matching the
 * datasheet text above - confirm against the datasheet before changing.
 *
 * Always use SX1278_TX_START_FIFO_NOT_EMPTY (bit7=1) for FSK packet mode.
 * This driver preloads the entire payload into the FIFO before switching to
 * TX, so in practice TX starts immediately either way (the FifoLevel
 * condition is already satisfied by the time TX mode is entered) - but the
 * correct bit value matters if FIFO-streaming for payloads >64 bytes is
 * ever implemented on top of the DIO1/FifoLevel IRQ.
 *===========================================================================*/
#define SX1278_TX_START_FIFO_NOT_EMPTY   BIT(7)  /* bit7=1: start on !Empty  */
#define SX1278_TX_START_FIFO_LEVEL       0x00U   /* bit7=0: start on Level   */
/* Deprecated alias kept for reference – do NOT use in packet mode */
#define SX1278_TX_START_FIFO_THRESH      SX1278_TX_START_FIFO_LEVEL
#define SX1278_FIFO_THRESHOLD_MSK        0x3FU

/*===========================================================================
 * RegIrqFlags1 (0x3E)
 *===========================================================================*/
#define SX1278_IRQ1_MODE_READY           BIT(7)
#define SX1278_IRQ1_RX_READY            BIT(6)
#define SX1278_IRQ1_TX_READY             BIT(5)
#define SX1278_IRQ1_PLL_LOCK             BIT(4)
#define SX1278_IRQ1_RSSI                 BIT(3)
#define SX1278_IRQ1_TIMEOUT              BIT(2)
#define SX1278_IRQ1_PREAMBLE_DETECT      BIT(1)
#define SX1278_IRQ1_SYNC_ADDR_MATCH      BIT(0)

/*===========================================================================
 * RegIrqFlags2 (0x3F)
 *===========================================================================*/
#define SX1278_IRQ2_FIFO_FULL            BIT(7)
#define SX1278_IRQ2_FIFO_EMPTY           BIT(6)
#define SX1278_IRQ2_FIFO_LEVEL           BIT(5)
#define SX1278_IRQ2_FIFO_OVERRUN         BIT(4)
#define SX1278_IRQ2_PACKET_SENT          BIT(3)
#define SX1278_IRQ2_PAYLOAD_READY        BIT(2)
#define SX1278_IRQ2_CRC_OK               BIT(1)
#define SX1278_IRQ2_LOW_BAT              BIT(0)

/*===========================================================================
 * RegDioMapping1 (0x40)
 *
 * DIO0: bits [7:6]
 * DIO1: bits [5:4]
 * DIO2: bits [3:2]
 * DIO3: bits [1:0]
 *
 * FSK Packet mode DIO0 mappings:
 *   00 = PayloadReady (RX) / PacketSent (TX)
 *   01 = CrcOk
 *   10 = (reserved)
 *   11 = TempChange/LowBat
 *===========================================================================*/
#define SX1278_DIO0_PAYLOAD_READY_PACKET_SENT  (0x00U << 6)
#define SX1278_DIO0_CRC_OK                     (0x01U << 6)
#define SX1278_DIO1_FIFO_LEVEL                 (0x00U << 4)
#define SX1278_DIO1_FIFO_EMPTY                 (0x01U << 4)
#define SX1278_DIO1_FIFO_FULL                  (0x02U << 4)
#define SX1278_DIO2_FIFO_FULL                  (0x00U << 2)
/* DIO3 – TimeoutRxStart */
#define SX1278_DIO3_TIMEOUT_RXSTART            (0x01U << 0)
#define SX1278_DIO4_PREAMBLE_DETECT            (0x00U << 6) /* DIO mapping 2 */

/*===========================================================================
 * Oscillator / crystal
 *===========================================================================*/
#define SX1278_FXOSC_HZ                  32000000UL
#define SX1278_FSTEP_HZ_X100             6103515UL  /* 61.03515625 Hz * 100   */

/*===========================================================================
 * Chip version
 *===========================================================================*/
#define SX1278_CHIP_VERSION              0x12U

/*===========================================================================
 * FIFO
 *===========================================================================*/
#define SX1278_FIFO_SIZE                 64U
#define SX1278_FIFO_STREAM_THRESHOLD     32U  /* FifoLevel flag asserts when FIFO has > 32 bytes */
#define SX1278_FIFO_STREAM_CHUNK         (SX1278_FIFO_SIZE - SX1278_FIFO_STREAM_THRESHOLD) /* 32 - safe top-up/drain size when FifoLevel is clear */
/* Deprecated alias kept for reference - previous value (1) made the shared
 * FifoLevel status flag assert almost permanently once TX/RX started,
 * which is useless as a "there's a meaningful chunk of room/data" signal
 * for FIFO streaming of payloads > SX1278_FIFO_SIZE. */
#define SX1278_FIFO_DEFAULT_THRESHOLD    SX1278_FIFO_STREAM_THRESHOLD

/*===========================================================================
 * PA config helpers
 *===========================================================================*/
/* Output power for RFO pin: Pout = Pmax - (15 - OutputPower)
 * Pmax = 10.8 + 0.6*MaxPower  [dBm] (MaxPower bits [6:4])
 * For MaxPower=4:  Pmax=13.2 dBm
 * For PA_BOOST:   Pout = 2 + OutputPower  [dBm] (with PA_DAC=default)
 *                 Pout = 5 + OutputPower  [dBm] (with PA_DAC=0x87 for 20dBm)
 */
#define SX1278_PA_CONFIG_RFO(pout_dbm) \
    ((((uint8_t)(((pout_dbm) + 2) / 1)) & 0x0FU) | (4U << 4))

/* Default PA_BOOST +17 dBm */
#define SX1278_PA_CONFIG_BOOST_17DBM     (SX1278_PA_SELECT_BIT | (4U << 4) | 15U)
/* PA_BOOST +20 dBm (requires PA_DAC=0x87) */
#define SX1278_PA_CONFIG_BOOST_20DBM     (SX1278_PA_SELECT_BIT | (7U << 4) | 15U)

#endif /* ZEPHYR_DRIVERS_FSK_SX1278_FSK_REGS_H_ */