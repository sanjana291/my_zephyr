/**
 * @file    config_params.h
 * @brief   Shared configuration-parameter structures for the 2-Way Shuttle.
 *
 * ===========================================================================
 * ARCHITECTURE OVERVIEW
 * ===========================================================================
 *
 *  ┌─────────────────────────────────────────────────────────────────────┐
 *  │  TCP CLIENT (Studio)                                                │
 *  │    JSON  -> Config Module  -> config_sec_<X>_t  (user view)      │
 *  │                  │                                                  │
 *  │                  │  validates ranges, calls storage API             │
 *  │                  V                                                  │
 *  │            Storage Module  -> config_nvm_sec_<X>_t  (NVM view)    │
 *  │                                  adds/checks is_configured byte    │
 *  │                                  serialises to EEPROM              │
 *  └─────────────────────────────────────────────────────────────────────┘
 *
 *  config_sec_<X>_t        - User-facing struct: parameter fields only.
 *                            Used by: config module, application, TCP parser.
 *                            The is_configured byte is NOT present here.
 *
 *  config_nvm_sec_<X>_t    - Storage-module-private struct: is_configured
 *                            prepended, then parameter fields, all packed
 *                            to match EEPROM wire format exactly.
 *                            Used by: storage module ONLY.
 *
 *  config_params_t          - All user-facing sections aggregated.
 *                            Also carries user-only fields that
 *                            are never written to EEPROM.
 *
 *  config_nvm_params_t      - All NVM sections aggregated; the exact binary
 *                            image written to / read from EEPROM (after the
 *                            record header). Storage module ONLY.
 *
 * ===========================================================================
 * USER-ONLY vs. EEPROM FIELDS
 * ===========================================================================
 *
 *  fwv (Firmware Version) - Present in config_sec_o_t (user struct) so the
 *  config module can include it in read-responses over TCP. NOT stored in
 *  EEPROM; the value is #defined in the firmware build system and populated
 *  at run-time before the struct is sent to the client.
 *
 *  stm (System Time) - Present in config_sec_o_t (user struct) so the
 *  config module can read/write the RTC over TCP. NOT stored in EEPROM;
 *  the storage module ignores it.  Format: [DD, MM, YYYY, HR, MN, SC, WD]
 *  packed into 7 × uint8_t.
 *
 * ===========================================================================
 * EEPROM LAYOUT  (overhead + 15 sections, each prefixed by is_configured)
 * ===========================================================================
 *
 *  Offset   Section              Param B   NVM B (is_cfg + param)
 *  ──────   ──────────────────   ───────   ──────────────────────
 *       0   Record overhead            -       6  (magic+ver+len+CRC16)
 *       6   A  Store Mission           8       9
 *      15   B  Retrieve/Cont           10     11
 *      26   C  Rack & Pallet Geo       6       7
 *      33   D  Shuttle Speed           6       7
 *      40   E  Accel/Decel Ramps      24      25
 *      65   F  Safety Distances        8       9
 *      74   G  Alignment & Recovery    4       5
 *      79   H  Lift                    2       3
 *      82   I  Parking & Power         8       9
 *      91   J  Sensor Cal (factory)   10      11
 *     102   K  Machine Geo (factory)   4       5
 *     107   L  Wi-Fi Settings         80      81
 *     188   M  RF Settings             6       7
 *     195   N  General Settings        8       9
 *     204   O  Statistics             36      37  (separate EEPROM region)
 *  ──────                           ───     ───
 *     241   Total record bytes
 *     482   Two-bank total  (safe against power-loss mid-write)
 *     512   Recommended EEPROM allocation
 *
 * ===========================================================================
 * NAMING CONVENTIONS
 * ===========================================================================
 *
 *  config_sec_<X>_t                  User-facing struct (no is_configured).
 *  config_nvm_sec_<X>_t              NVM struct (is_configured + fields,
 *                                    packed). Storage module only.
 *  CFG_<X>_<FIELD>_{MIN,MAX,DEFAULT} Per-parameter range / default macros.
 *  CONFIG_SECTION_CONFIGURED         Sentinel: section has valid data.
 *  CONFIG_SECTION_CLEARED            Sentinel: section has no valid data.
 */

#ifndef CONFIG_PARAMS_H_
#define CONFIG_PARAMS_H_

#include <stdint.h>
#include <zephyr/drivers/rtc.h>

#ifdef __cplusplus
extern "C" {
#endif


/* ===========================================================================
 * Record-level size constants
 * =========================================================================== */

/** Overhead: magic(1) + version(1) + length(2) + CRC-16(2). */
#define CONFIG_RECORD_OVERHEAD_BYTES    (6U)

/** Parameter payload bytes (all sections, no is_configured bytes). */
#define CONFIG_PARAM_BYTES              (220U)

/** is_configured byte count (one per section × 15 sections). */
#define CONFIG_SECTION_STATUS_BYTES     (15U)

/** Full NVM payload = param bytes + is_configured bytes. */
#define CONFIG_NVM_PAYLOAD_BYTES        (CONFIG_PARAM_BYTES + CONFIG_SECTION_STATUS_BYTES)

/** Full NVM record = overhead + payload. */
#define CONFIG_TOTAL_RECORD_BYTES       (CONFIG_RECORD_OVERHEAD_BYTES + CONFIG_NVM_PAYLOAD_BYTES)

/** Recommended EEPROM allocation. */
#define CONFIG_EEPROM_ALLOCATION_BYTES  (512U)


/* ===========================================================================
 * Section configuration-status sentinels
 *
 * The is_configured byte is the first byte of every NVM section struct.
 * It is written and read EXCLUSIVELY by the storage module.
 * The config module and application never see or use it.
 * =========================================================================== */

#define CONFIG_SECTION_CONFIGURED   (0xA5U) /**< Section holds valid, user-written data. */
#define CONFIG_SECTION_CLEARED      (0x00U) /**< Section empty / fresh chip default.      */


/* ===========================================================================
 * Section index enumeration
 * =========================================================================== */

typedef enum {
    CONFIG_SEC_A = 0,   /**< Store Mission                       */
    CONFIG_SEC_B,       /**< Retrieve / Continuous Retrieve      */
    CONFIG_SEC_C,       /**< Rack & Pallet Geometry              */
    CONFIG_SEC_D,       /**< Shuttle Speed                       */
    CONFIG_SEC_E,       /**< Acceleration & Deceleration Ramps  */
    CONFIG_SEC_F,       /**< Safety Stopping Distances           */
    CONFIG_SEC_G,       /**< Alignment & Recovery                */
    CONFIG_SEC_H,       /**< Lift                                */
    CONFIG_SEC_I,       /**< Parking & Power Saving              */
    CONFIG_SEC_J,       /**< Sensor Calibration (factory only)   */
    CONFIG_SEC_K,       /**< Machine Geometry (factory only)     */
    CONFIG_SEC_L,       /**< Wi-Fi Settings                      */
    CONFIG_SEC_M,       /**< RF Settings                         */
    CONFIG_SEC_N,       /**< General Settings                    */
    CONFIG_SEC_O,       /**< Statistics (separate EEPROM region) */
    CONFIG_SEC_MAX
} config_param_sec_t;


/* ===========================================================================
 * Section A - Store Mission
 * Param bytes: GFF(2) + LWS(4) + PIB(2) = 8 B  |  NVM: 9 B
 * =========================================================================== */

#define CFG_A_GFF_MIN      (200U)
#define CFG_A_GFF_MAX      (800U)
#define CFG_A_GFF_DEFAULT  (200U)     /**< mm - FIFO first-pallet gap, must be >= RNP */

#define CFG_A_LWS_MIN      (5000UL)
#define CFG_A_LWS_MAX      (300000UL)
#define CFG_A_LWS_DEFAULT  (20000UL)  /**< ms - shuttle stationary the whole wait */

#define CFG_A_PIB_MIN      (50U)
#define CFG_A_PIB_MAX      (400U)
#define CFG_A_PIB_DEFAULT  (200U)     /**< mm */

/**
 * @brief User-facing Section A.
 *
 * @note  __attribute__((packed)) is required because lws (uint32_t) follows
 *        gff (uint16_t).  Without packing the compiler inserts 2 pad bytes
 *        before lws, growing the struct from 8 to 12 bytes.  Access each
 *        member by name; do not take a misaligned pointer to lws on targets
 *        where unaligned 32-bit loads raise a bus fault.
 */
typedef struct __attribute__((packed)) {
    uint16_t gff;   /**< First Pallet Gap FIFO          [mm]  STORE_FIRST_OFFSET_FIFO_MM       */
    uint32_t lws;   /**< Pallet Waiting Time (storing)  [ms]  P2_TIMEOUT_WAIT_PALLET_STORE_MS  */
    uint16_t pib;   /**< Reposition Distance            [mm]  P2_REPOSITION_DIST_DEFAULT_MM    */
} config_sec_a_t;

/** NVM Section A - storage module only. */
typedef struct __attribute__((packed)) {
    uint8_t  is_configured;
    uint16_t gff;
    uint32_t lws;
    uint16_t pib;
} config_nvm_sec_a_t;


/* ===========================================================================
 * Section B - Retrieve / Continuous Retrieve Mission
 * Param bytes: PWS(4) + PWC(4) + WGO(2) = 10 B  |  NVM: 11 B
 * =========================================================================== */

#define CFG_B_PWS_MIN      (30000UL)
#define CFG_B_PWS_MAX      (1800000UL)
#define CFG_B_PWS_DEFAULT  (180000UL)  /**< ms - min 30 s; shuttle needs ~10 s to position sensors */

#define CFG_B_PWC_MIN      (60000UL)
#define CFG_B_PWC_MAX      (600000UL)
#define CFG_B_PWC_DEFAULT  (300000UL)  /**< ms - shuttle holds pallet raised this whole time */

#define CFG_B_WGO_MIN      (200U)
#define CFG_B_WGO_MAX      (1000U)
#define CFG_B_WGO_DEFAULT  (800U)      /**< mm - measured from pallet ahead, not rack end */

/**
 * @brief User-facing Section B.
 *
 * @note  __attribute__((packed)) prevents 2 bytes of tail padding that the
 *        compiler would otherwise append after wgo (uint16_t) to re-align the
 *        struct to 4 bytes, growing it from 10 to 12 bytes.
 */
typedef struct __attribute__((packed)) {
    uint32_t pws;   /**< Pickup Wait - Single Retrieve      [ms]  P2_TIMEOUT_WAIT_PICKUP_RETRIEVE_MS   */
    uint32_t pwc;   /**< Pickup Wait - Continuous Retrieve  [ms]  P2_TIMEOUT_WAIT_PICKUP_CONTINUOUS_MS */
    uint16_t wgo;   /**< Waiting Gap - Slot Occupied        [mm]  CONTINUOUS_HOLD_STANDOFF_MM          */
} config_sec_b_t;

/** NVM Section B - storage module only. */
typedef struct __attribute__((packed)) {
    uint8_t  is_configured;
    uint32_t pws;
    uint32_t pwc;
    uint16_t wgo;
} config_nvm_sec_b_t;


/* ===========================================================================
 * Section C - Rack & Pallet Geometry
 * Param bytes: GDP(2) + PLD(2) + EFC(2) = 6 B  |  NVM: 7 B
 * =========================================================================== */

#define CFG_C_GDP_MIN      (20U)
#define CFG_C_GDP_MAX      (400U)
#define CFG_C_GDP_DEFAULT  (50U)     /**< mm - spacing between consecutively stored pallets */

#define CFG_C_PLD_MIN      (800U)
#define CFG_C_PLD_MAX      (2000U)
#define CFG_C_PLD_DEFAULT  (1000U)   /**< mm - depth of pallets used at this site */

#define CFG_C_EFC_MIN      (100U)
#define CFG_C_EFC_MAX      (400U)
#define CFG_C_EFC_DEFAULT  (200U)    /**< mm - extra room needed to drive back out past last pallet */

/** User-facing Section C. */
typedef struct {
    uint16_t gdp;   /**< Pallet Gap               [mm]  P2_PALLET_PITCH_DEFAULT_MM */
    uint16_t pld;   /**< Pallet Length             [mm]  PALLET_DEPTH_MM            */
    uint16_t efc;   /**< Rack Full Clearance       [mm]  RACK_FULL_OFFSET_MM        */
} config_sec_c_t;

/** NVM Section C - storage module only. */
typedef struct __attribute__((packed)) {
    uint8_t  is_configured;
    uint16_t gdp; uint16_t pld; uint16_t efc;
} config_nvm_sec_c_t;


/* ===========================================================================
 * Section D - Shuttle Speed
 * Param bytes: SWL(2) + SLD(2) + LSP(2) = 6 B  |  NVM: 7 B
 * =========================================================================== */

#define CFG_D_SWL_MIN      (500U)
#define CFG_D_SWL_MAX      (2630U)   /**< rpm ≈ 1.10 m/s; raising also needs SSD raised */
#define CFG_D_SWL_DEFAULT  (2630U)

#define CFG_D_SLD_MIN      (500U)
#define CFG_D_SLD_MAX      (1920U)   /**< rpm ≈ 0.80 m/s; must never exceed SWL */
#define CFG_D_SLD_DEFAULT  (1920U)

#define CFG_D_LSP_MIN      (500U)
#define CFG_D_LSP_MAX      (800U)    /**< rpm */
#define CFG_D_LSP_DEFAULT  (620U)    /**< Single speed for raise and lower, mission and manual */

/** User-facing Section D. */
typedef struct {
    uint16_t swl;   /**< Speed Unloaded   [rpm]  TRACTION_CRUISE_UNLOADED_RPM */
    uint16_t sld;   /**< Speed Loaded     [rpm]  TRACTION_CRUISE_LOADED_RPM   */
    uint16_t lsp;   /**< Lift Speed       [rpm]  LIFT_STROKE_RPM              */
} config_sec_d_t;

/** NVM Section D - storage module only. */
typedef struct __attribute__((packed)) {
    uint8_t  is_configured;
    uint16_t swl; uint16_t sld; uint16_t lsp;
} config_nvm_sec_d_t;


/* ===========================================================================
 * Section E - Acceleration & Deceleration Ramps
 * Param bytes: 8 ramps × (speed:uint16_t + time:uint8_t) = 8×3 = 24 B  |  NVM: 25 B
 *
 * Wire format interleaves uint16_t and uint8_t fields. Both structs are
 * __attribute__((packed)) to suppress compiler padding.
 * Access members by name; do not take misaligned pointers on targets where
 * UNALIGN_TRP is set.
 * =========================================================================== */

#define CFG_E_RPM_MIN   (500U)
#define CFG_E_RPM_MAX   (6000U)
#define CFG_E_TIME_MIN  (1U)     /**< s - whole seconds; 0 rejected by motor drive */
#define CFG_E_TIME_MAX  (10U)

#define CFG_E_AUR_DEFAULT  (5254U)  /**< rpm - accel unloaded */
#define CFG_E_AUT_DEFAULT  (1U)
#define CFG_E_ALR_DEFAULT  (2000U)  /**< rpm - gentler than unloaded; pallet is unclamped */
#define CFG_E_ALT_DEFAULT  (2U)
#define CFG_E_MAS_DEFAULT  (800U)   /**< rpm - manual accel loaded (held jog button) */
#define CFG_E_MAT_DEFAULT  (1U)
#define CFG_E_MUS_DEFAULT  (800U)   /**< rpm - manual accel unloaded (+PLUS jog button) */
#define CFG_E_MUT_DEFAULT  (1U)
#define CFG_E_DUD_DEFAULT  (2540U)  /**< rpm - softening lengthens stopping distance */
#define CFG_E_DUT_DEFAULT  (2U)
#define CFG_E_DLD_DEFAULT  (1320U)
#define CFG_E_DLT_DEFAULT  (2U)
#define CFG_E_LAS_DEFAULT  (900U)   /**< rpm - lift accel */
#define CFG_E_LAT_DEFAULT  (1U)
#define CFG_E_LDS_DEFAULT  (900U)   /**< rpm - lift decel */
#define CFG_E_LDT_DEFAULT  (2U)

/** User-facing Section E. */
typedef struct __attribute__((packed)) {
    uint16_t aur;   /**< Accel Unloaded RPM          [rpm]  RAMP_ACCEL_UNLOADED.delta_speed        */
    uint8_t  aut;   /**< Accel Unloaded Time         [s]    RAMP_ACCEL_UNLOADED.delta_time         */
    uint16_t alr;   /**< Accel Loaded RPM            [rpm]  RAMP_ACCEL_LOADED.delta_speed          */
    uint8_t  alt;   /**< Accel Loaded Time           [s]    RAMP_ACCEL_LOADED.delta_time           */
    uint16_t mas;   /**< Manual Accel Loaded RPM     [rpm]  RAMP_MANUAL_ACCEL_LOADED.delta_speed   */
    uint8_t  mat;   /**< Manual Accel Loaded Time    [s]    RAMP_MANUAL_ACCEL_LOADED.delta_time    */
    uint16_t mus;   /**< Manual Accel Unloaded RPM   [rpm]  RAMP_MANUAL_ACCEL_UNLOADED.delta_speed */
    uint8_t  mut;   /**< Manual Accel Unloaded Time  [s]    RAMP_MANUAL_ACCEL_UNLOADED.delta_time  */
    uint16_t dud;   /**< Decel Unloaded RPM          [rpm]  RAMP_DECEL_UNLOADED.delta_speed        */
    uint8_t  dut;   /**< Decel Unloaded Time         [s]    RAMP_DECEL_UNLOADED.delta_time         */
    uint16_t dld;   /**< Decel Loaded RPM            [rpm]  RAMP_DECEL_LOADED.delta_speed          */
    uint8_t  dlt;   /**< Decel Loaded Time           [s]    RAMP_DECEL_LOADED.delta_time           */
    uint16_t las;   /**< Lift Accel RPM              [rpm]  RAMP_LIFT_ACCEL.delta_speed            */
    uint8_t  lat;   /**< Lift Accel Time             [s]    RAMP_LIFT_ACCEL.delta_time             */
    uint16_t lds;   /**< Lift Decel RPM              [rpm]  RAMP_LIFT_DECEL.delta_speed            */
    uint8_t  ldt;   /**< Lift Decel Time             [s]    RAMP_LIFT_DECEL.delta_time             */
} config_sec_e_t;

/** NVM Section E - storage module only. */
typedef struct __attribute__((packed)) {
    uint8_t  is_configured;
    uint16_t aur; uint8_t aut;
    uint16_t alr; uint8_t alt;
    uint16_t mas; uint8_t mat;
    uint16_t mus; uint8_t mut;
    uint16_t dud; uint8_t dut;
    uint16_t dld; uint8_t dlt;
    uint16_t las; uint8_t lat;
    uint16_t lds; uint8_t ldt;
} config_nvm_sec_e_t;


/* ===========================================================================
 * Section F - Safety Stopping Distances
 * Param bytes: SSD(2) + RNW(2) + RNP(2) + SDP(2) = 8 B  |  NVM: 9 B
 * =========================================================================== */

#define CFG_F_SSD_MIN      (1400U)
#define CFG_F_SSD_MAX      (2000U)
#define CFG_F_SSD_DEFAULT  (1400U)   /**< mm - must cover travel speed + braking distance */

#define CFG_F_RNW_MIN      (50U)
#define CFG_F_RNW_MAX      (300U)
#define CFG_F_RNW_DEFAULT  (50U)     /**< mm - applies to every movement, auto and manual */

#define CFG_F_RNP_MIN      (100U)
#define CFG_F_RNP_MAX      (300U)
#define CFG_F_RNP_DEFAULT  (100U)    /**< mm - raised pallet overhangs; must be >= RNW */

#define CFG_F_SDP_MIN      (1200U)
#define CFG_F_SDP_MAX      (2000U)
#define CFG_F_SDP_DEFAULT  (1200U)   /**< mm - hard floor; 1200 mm is minimum */

/** User-facing Section F. */
typedef struct {
    uint16_t ssd;   /**< Slow-Down Distance              [mm]  APPROACH_D_START_MM    */
    uint16_t rnw;   /**< Rack End Gap - Empty            [mm]  HOME_TRUE_UNLOADED_MM  */
    uint16_t rnp;   /**< Rack End Gap - Loaded           [mm]  HOME_TRUE_LOADED_MM    */
    uint16_t sdp;   /**< Pallet Ahead Slow-Down Distance [mm]  PALLET_PROTECT_DIST_MM */
} config_sec_f_t;

/** NVM Section F - storage module only. */
typedef struct __attribute__((packed)) {
    uint8_t  is_configured;
    uint16_t ssd; uint16_t rnw; uint16_t rnp; uint16_t sdp;
} config_nvm_sec_f_t;


/* ===========================================================================
 * Section G - Alignment & Recovery
 * Param bytes: ASR(2) + ATL(2) = 4 B  |  NVM: 5 B
 * =========================================================================== */

#define CFG_G_ASR_MIN      (200U)
#define CFG_G_ASR_MAX      (1500U)
#define CFG_G_ASR_DEFAULT  (500U)    /**< mm - increasing ASR REQUIRES increasing ATL */

/** Minimum ATL formula: ATL_min_ms = (ASR_mm × 25) + 2600 */
#define CFG_G_ATL_MIN      (15100U)
#define CFG_G_ATL_MAX      (60000U)
#define CFG_G_ATL_DEFAULT  (20000U)  /**< ms */

/** User-facing Section G. */
typedef struct {
    uint16_t asr;   /**< Alignment Range      [mm]  SHUTTLE_ALIGN_SWEEP_MM      */
    uint16_t atl;   /**< Alignment Time Limit [ms]  P2_TIMEOUT_SHUTTLE_ALIGN_MS */
} config_sec_g_t;

/** NVM Section G - storage module only. */
typedef struct __attribute__((packed)) {
    uint8_t  is_configured;
    uint16_t asr; uint16_t atl;
} config_nvm_sec_g_t;


/* ===========================================================================
 * Section H - Lift
 * Param bytes: LFT(2) = 2 B  |  NVM: 3 B
 * =========================================================================== */

#define CFG_H_LFT_MIN      (3250U)
#define CFG_H_LFT_MAX      (3500U)
#define CFG_H_LFT_DEFAULT  (3250U)  /**< ms - FAULT LIMIT, not a speed target */

/** User-facing Section H. */
typedef struct {
    uint16_t lft;   /**< Lift Fault Timeout  [ms]  LIFT_STROKE_WATCHDOG_MS */
} config_sec_h_t;

/** NVM Section H - storage module only. */
typedef struct __attribute__((packed)) {
    uint8_t  is_configured;
    uint16_t lft;
} config_nvm_sec_h_t;


/* ===========================================================================
 * Section I - Parking & Power Saving
 * Param bytes: PKD(2) + API(4) + MSI(2) = 8 B  |  NVM: 9 B
 *
 * NOTE: api (uint32_t) is declared before pkd (uint16_t) in the user struct
 * so that 4-byte alignment is satisfied without padding, keeping
 * sizeof(config_sec_i_t) == 8.  The NVM struct uses __attribute__((packed))
 * and preserves wire order: is_cfg, pkd, api, msi.
 * =========================================================================== */

#define CFG_I_PKD_MIN      (200U)
#define CFG_I_PKD_MAX      (1400U)
#define CFG_I_PKD_DEFAULT  (500U)         /**< mm - above 1400 mm shuttle stops short silently */

#define CFG_I_API_MIN      (300000UL)
#define CFG_I_API_MAX      (43200000UL)
#define CFG_I_API_DEFAULT  (3600000UL)    /**< ms - any command resets the countdown */

#define CFG_I_MSI_MIN      (10000U)
#define CFG_I_MSI_MAX      (60000U)
#define CFG_I_MSI_DEFAULT  (30000U)       /**< ms - motors wake automatically on next command */

/** User-facing Section I. */
typedef struct {
    uint32_t api;   /**< Auto Power-Off After Idle  [ms]  MAIN_OFF_TIMEOUT_MS       */
    uint16_t pkd;   /**< Parking Distance           [mm]  PARK_DISTANCE_MM          */
    uint16_t msi;   /**< Motor Sleep Time           [ms]  MOTOR_CAN_IDLE_DISABLE_MS */
} config_sec_i_t;

/** NVM Section I - storage module only. Wire order: is_cfg, pkd, api, msi. */
typedef struct __attribute__((packed)) {
    uint8_t  is_configured;
    uint16_t pkd; uint32_t api; uint16_t msi;
} config_nvm_sec_i_t;


/* ===========================================================================
 * Section J - Sensor Calibration  *** FACTORY ONLY ***
 * Param bytes: FSO(2)+RSO(2)+FPO(2)+RPO(2)+PTC(2) = 10 B  |  NVM: 11 B
 * =========================================================================== */

#define CFG_J_FSO_MIN      ( 20)
#define CFG_J_FSO_MAX      (120)
#define CFG_J_FSO_DEFAULT  ( 50)    /**< mm - wrong value shifts every forward stop point */

#define CFG_J_RSO_MIN      ( 20)
#define CFG_J_RSO_MAX      (120)
#define CFG_J_RSO_DEFAULT  ( 50)    /**< mm - as FSO, for reverse travel */

#define CFG_J_FPO_MIN      ( 20)
#define CFG_J_FPO_MAX      (120)
#define CFG_J_FPO_DEFAULT  ( 50)    /**< mm - not interchangeable with collision offsets */

#define CFG_J_RPO_MIN      ( 20)
#define CFG_J_RPO_MAX      (120)
#define CFG_J_RPO_DEFAULT  ( 50)

/** 9925 = cos(~7°) × 10000. Shared by both pallet sensors. */
#define CFG_J_PTC_MIN      (9600U)
#define CFG_J_PTC_MAX      (10000U)
#define CFG_J_PTC_DEFAULT  (9925U)

/** User-facing Section J. */
typedef struct {
    int16_t  fso;   /**< Front Collision Sensor Offset       [mm]       COLLISION_FRONT_SENSOR_OFFSET_MM       */
    int16_t  rso;   /**< Rear Collision Sensor Offset        [mm]       COLLISION_BACK_SENSOR_OFFSET_MM        */
    int16_t  fpo;   /**< Front Pallet Distance Sensor Offset [mm]       PALLET_DISTANCE_FRONT_SENSOR_OFFSET_MM */
    int16_t  rpo;   /**< Rear Pallet Distance Sensor Offset  [mm]       PALLET_DISTANCE_BACK_SENSOR_OFFSET_MM  */
    uint16_t ptc;   /**< Pallet Sensor Tilt Correction [×0.0001 ≈ cos7°] PALLET_SENSOR_COS_SCALED              */
} config_sec_j_t;

/** NVM Section J - storage module only. */
typedef struct __attribute__((packed)) {
    uint8_t  is_configured;
    int16_t  fso; int16_t rso; int16_t fpo; int16_t rpo; uint16_t ptc;
} config_nvm_sec_j_t;


/* ===========================================================================
 * Section K - Machine Geometry  *** FACTORY ONLY ***
 * Param bytes: WHD(2) + GBR(2) = 4 B  |  NVM: 5 B
 * =========================================================================== */

/** Unit: ×0.1 mm  ->  1200 means 120.0 mm.  Every measured distance depends on this. */
#define CFG_K_WHD_MIN      (100U)
#define CFG_K_WHD_MAX      (2000U)
#define CFG_K_WHD_DEFAULT  (120U)

/** Unit: ×0.1 : 1  ->  150 means 15.0 : 1.  Works together with WHD. */
#define CFG_K_GBR_MIN      (0U)
#define CFG_K_GBR_MAX      (500U)
#define CFG_K_GBR_DEFAULT  (15U)

/** User-facing Section K. */
typedef struct {
    uint16_t whd;   /**< Wheel Diameter  [×0.1 mm]   ODO_WHEEL_DIAMETER_MM */
    uint16_t gbr;   /**< Gearbox Ratio   [×0.1 : 1]  ODO_GEARBOX_RATIO     */
} config_sec_k_t;

/** NVM Section K - storage module only. */
typedef struct __attribute__((packed)) {
    uint8_t  is_configured;
    uint16_t whd; uint16_t gbr;
} config_nvm_sec_k_t;


/* ===========================================================================
 * Section L - Wi-Fi Settings
 * Param bytes: SSZ(1)+SID(25)+PSZ(1)+WPS(25)+WAT(1)+WST(1)+WIP(4)+WSM(4)+WGI(4)+RDN(12)+RCP(2) = 80 B
 * NVM: 81 B
 *
 * String fields carry an explicit length prefix (ssz / psz) so the storage
 * module can serialise them without relying on null-termination in EEPROM.
 * wip/wsm/wgi are uint32_t preceded by two uint8_t fields; the struct is
 * packed to suppress the 2-byte alignment padding the compiler would otherwise
 * insert before wip.
 * =========================================================================== */

#define CFG_L_SSID_MAX_LEN  (25U)   /**< bytes; 802.11 SSID limit is 32 chars */
#define CFG_L_PWD_MAX_LEN   (25U)
#define CFG_L_RDN_LEN       (12U)   /**< Remote controller DNS name / MAC hex string */

#define CFG_L_WAT_MIN       (0U)
#define CFG_L_WAT_MAX       (1U)    /**< 0 = static IP, 1 = DHCP */

#define CFG_L_WST_MIN       (0U)
#define CFG_L_WST_MAX       (8U)

#define CFG_L_RCP_MIN       (1U)
#define CFG_L_RCP_MAX       (65535U)

/** User-facing Section L. */
typedef struct __attribute__((packed)) {
    uint8_t  ssz;                        /**< Wi-Fi SSID length (bytes)               */
    char     sid[CFG_L_SSID_MAX_LEN];   /**< Wi-Fi SSID (not null-terminated in NVM) */
    uint8_t  psz;                        /**< Wi-Fi Password length (bytes)            */
    char     wps[CFG_L_PWD_MAX_LEN];    /**< Wi-Fi Password                           */
    uint8_t  wat;                        /**< Address Mode  (0 = static, 1 = DHCP)    */
    uint8_t  wst;                        /**< Security Type (0-8)                      */
    uint32_t wip;                        /**< Shuttle IP Address   (big-endian IPv4)   */
    uint32_t wsm;                        /**< Subnet Mask                              */
    uint32_t wgi;                        /**< Gateway IP                               */
    char     rdn[CFG_L_RDN_LEN];        /**< Remote Controller DNS Name / MAC string  */
    uint16_t rcp;                        /**< Remote Controller Port                   */
} config_sec_l_t;

/** NVM Section L - storage module only. */
typedef struct __attribute__((packed)) {
    uint8_t  is_configured;
    uint8_t  ssz; char sid[CFG_L_SSID_MAX_LEN];
    uint8_t  psz; char wps[CFG_L_PWD_MAX_LEN];
    uint8_t  wat; uint8_t wst;
    uint32_t wip; uint32_t wsm; uint32_t wgi;
    char     rdn[CFG_L_RDN_LEN];
    uint16_t rcp;
} config_nvm_sec_l_t;


/* ===========================================================================
 * Section M - RF Settings
 * Param bytes: RAD(1)+SYW(1)+CID(1)+SND(1)+PMT(2) = 6 B  |  NVM: 7 B
 *
 * NOTE: SYW is now a single uint8_t LoRa sync byte (0x01-0xFF, exclude 0x34).
 * =========================================================================== */

#define CFG_M_RAD_MIN      (1U)
#define CFG_M_RAD_MAX      (254U)   /**< 0x01-0xFE; must match remote's firmware */

#define CFG_M_SYW_MIN      (1U)
#define CFG_M_SYW_MAX      (255U)   /**< exclude 0x34 (reserved for public LoRaWAN) */
#define CFG_M_SYW_DEFAULT  (0x12U)  /**< current hardcoded value; assign per remote+shuttle group */

#define CFG_M_CID_MIN      (0U)
#define CFG_M_CID_MAX      (20U)    /**< channel index on 21-channel grid; pairing always uses 20 */
#define CFG_M_CID_DEFAULT  (4U)     /**< channel 4 = 433.4244 MHz */

#define CFG_M_SND_MIN      (1U)
#define CFG_M_SND_MAX      (254U)

#define CFG_M_PMT_MIN      (5000U)
#define CFG_M_PMT_MAX      (30000U)
#define CFG_M_PMT_DEFAULT  (10000U) /**< ms - pairing window after boot */

/** User-facing Section M. */
typedef struct {
    uint8_t  rad;   /**< Remote Node ID         [0x01-0xFE]  SUBGHZ_REMOTE_ADDR                */
    uint8_t  syw;   /**< LoRa Network Sync Byte [0x01-0xFF]  LORA_LINK_SYNC_WORD               */
    uint8_t  cid;   /**< Channel ID             [0-20]       SUBGHZ_CHANNEL_ID                 */
    uint8_t  snd;   /**< Shuttle Node ID        [0x01-0xFE]  SUBGHZ_SHUTTLE_ADDR               */
    uint16_t pmt;   /**< Pairing Mode Timeout   [ms]         CONFIG_RMC_SUBGHZ_PAIRING_TIMEOUT_MS */
} config_sec_m_t;

/** NVM Section M - storage module only. */
typedef struct __attribute__((packed)) {
    uint8_t  is_configured;
    uint8_t  rad; uint8_t syw; uint8_t cid; uint8_t snd; uint16_t pmt;
} config_nvm_sec_m_t;


/* ===========================================================================
 * Section N - General Settings
 * Param bytes: COM(1)+MPH(1)+PLM(2)+MNO(2)+MPL(2) = 8 B  |  NVM: 9 B
 * =========================================================================== */

/** Communication link used by the handheld remote. */
typedef enum {
    CONFIG_COMM_MODE_WIFI    = 0,   /**< Wi-Fi        */
    CONFIG_COMM_MODE_SUB_GHZ = 1    /**< Sub-GHz RF   */
} config_comm_mode_t;

#define CFG_N_COM_MIN      (0U)
#define CFG_N_COM_MAX      (1U)

#define CFG_N_MPH_MIN      (0U)
#define CFG_N_MPH_MAX      (1U)
#define CFG_N_MPH_DEFAULT  (0U)     /**< 0 = OFF, 1 = ON */

#define CFG_N_PLM_MIN      (0U)
#define CFG_N_PLM_MAX      (1200U)  /**< kg */
#define CFG_N_PLM_DEFAULT  (1000U)

#define CFG_N_MNO_MIN      (1U)
#define CFG_N_MNO_MAX      (9999U)

#define CFG_N_MPL_MIN      (0U)
#define CFG_N_MPL_MAX      (1200U)  /**< kg */
#define CFG_N_MPL_DEFAULT  (1000U)

/** User-facing Section N. */
typedef struct {
    uint8_t  com;   /**< Communication Mode  (config_comm_mode_t)                         */
    uint8_t  mph;   /**< Multi-Pallet Mode   (0 = OFF, 1 = ON)                            */
    uint16_t plm;   /**< Payload Limit       [kg]  - overweight alarm threshold            */
    uint16_t mno;   /**< Machine Number      [1-9999]  - appears in logs only             */
    uint16_t mpl;   /**< Max Payload         [kg]  - rated capacity of this shuttle       */
} config_sec_n_t;

/** NVM Section N - storage module only. */
typedef struct __attribute__((packed)) {
    uint8_t  is_configured;
    uint8_t  com; uint8_t mph;
    uint16_t plm; uint16_t mno; uint16_t mpl;
} config_nvm_sec_n_t;


/* ===========================================================================
 * Section O - Statistics  (separate EEPROM region; read-only from user side)
 * Param bytes: HWV(2)+TOD(4)+SCT(4)+RCT(4)+PSC(4)+PLC(4)+LCT(4)+M4C(1)+reserved(9) = 36 B
 * NVM: 37 B
 *
 * fwv (Firmware Version) - user struct only; NOT stored in EEPROM.
 *   Populated from the build-system constant before sending to TCP client.
 *
 * stm (System / RTC Time) - user struct only; NOT stored in EEPROM.
 *   Config module reads/writes RTC directly. Format: [DD,MM,YYYY,HR,MN,SC,WD].
 *
 * hwv (Hardware Version) - stored in EEPROM (statistics region); also present
 *   in user struct so the client can read it without querying the running image.
 * =========================================================================== */

/** Number of uint8_t elements in the stm (system time) array. */
#define CFG_O_STM_LEN   (6U)   /**< [DD, MM, YYYY, HR, MN, SC, WD] -> 6 elements */

/** Number of reserved bytes at the end of the statistics EEPROM section. */
#define CFG_O_RESERVED  (9U - 1U - 4U) // - 1U for plt (1 byte) -4U for tmn

/**
 * @brief User-facing Section O - Statistics.
 *
 * @note  fwv and stm are NEVER written to EEPROM.
 *        The storage module copies all other fields to/from the statistics
 *        EEPROM region.
 */
typedef struct {
    /* ---- User-only (never written to EEPROM) ---- */
    uint16_t fwv;               /**< Firmware Version  (read-only; defined in build system)    */
    // uint8_t  stm[CFG_O_STM_LEN]; /**< System/RTC Time [DD,MM,YYYY,HR,MN,SC] (R/W RTC) */
    struct rtc_time stm;
    uint16_t hwv;               /**< Hardware Version  (read-only; set at manufacture)         */
    uint32_t tod;               /**< Total Odometry    [m]                                     */
    uint32_t sct;               /**< Store Count       (lift(up) -> traction -> lift(down) = 1)          */
    uint32_t rct;               /**< Retrieved Count   (lift(up) -> traction -> lift(down) = 1)          */
    uint32_t psc;               /**< Compact Push Count                                        */
    uint32_t plc;               /**< Compact Pull Count                                        */
    uint32_t lct;               /**< Lift Count        (lift(up) -> lift(down) = 1)                     */
    uint8_t  m4c;               /**< M4 Not-Responding Event Count                             */
    uint8_t  plt;
    uint32_t tmn;
} config_sec_o_t;

/**
 * @brief NVM Section O - statistics EEPROM region, storage module only.
 *
 * fwv and stm are intentionally absent. reserved[] pads to 36 param bytes
 * so the section total matches the memory map (36 param + 1 is_cfg = 37 B).
 */
typedef struct __attribute__((packed)) {
    uint8_t  is_configured;
    uint16_t hwv;
    uint32_t tod;
    uint32_t sct;
    uint32_t rct;
    uint32_t psc;
    uint32_t plc;
    uint32_t lct;
    uint8_t  m4c;
    uint8_t  plt;
    uint32_t tmn;
    uint8_t  reserved[CFG_O_RESERVED]; /**< Pad to 36 param bytes; initialise to 0x00. */
} config_nvm_sec_o_t;


/* ===========================================================================
 * Aggregate parameter sets
 * =========================================================================== */

/**
 * @brief Complete user-facing configuration - all 15 sections.
 *
 * Exchanged between the config module (TCP layer) and the application.
 * No is_configured bytes are present.  Members are in section A->O order.
 *
 * @warning Do not reorder members without bumping CONFIG_NVM_FORMAT_VERSION
 *          and providing a migration routine in the storage module.
 */
typedef struct {
    config_sec_a_t sec_a;   /**< A - Store Mission                       */
    config_sec_b_t sec_b;   /**< B - Retrieve / Continuous Retrieve      */
    config_sec_c_t sec_c;   /**< C - Rack & Pallet Geometry              */
    config_sec_d_t sec_d;   /**< D - Shuttle Speed                       */
    config_sec_e_t sec_e;   /**< E - Acceleration & Deceleration Ramps  */
    config_sec_f_t sec_f;   /**< F - Safety Stopping Distances           */
    config_sec_g_t sec_g;   /**< G - Alignment & Recovery                */
    config_sec_h_t sec_h;   /**< H - Lift                                */
    config_sec_i_t sec_i;   /**< I - Parking & Power Saving              */
    config_sec_j_t sec_j;   /**< J - Sensor Calibration (factory only)   */
    config_sec_k_t sec_k;   /**< K - Machine Geometry (factory only)     */
    config_sec_l_t sec_l;   /**< L - Wi-Fi Settings                      */
    config_sec_m_t sec_m;   /**< M - RF Settings                         */
    config_sec_n_t sec_n;   /**< N - General Settings                    */
    config_sec_o_t sec_o;   /**< O - Statistics (+ user-only fwv, stm)  */
} config_params_t;

/**
 * @brief Complete NVM parameter image - storage module only.
 *
 * Binary-identical to the EEPROM payload that follows the record header.
 * Each section's first byte is is_configured; the rest are parameter data.
 *
 * @warning Do NOT expose this type or its members to the config module,
 *          the application, or the TCP parser.
 */
typedef struct __attribute__((packed)) {
    config_nvm_sec_a_t sec_a;
    config_nvm_sec_b_t sec_b;
    config_nvm_sec_c_t sec_c;
    config_nvm_sec_d_t sec_d;
    config_nvm_sec_e_t sec_e;
    config_nvm_sec_f_t sec_f;
    config_nvm_sec_g_t sec_g;
    config_nvm_sec_h_t sec_h;
    config_nvm_sec_i_t sec_i;
    config_nvm_sec_j_t sec_j;
    config_nvm_sec_k_t sec_k;
    config_nvm_sec_l_t sec_l;
    config_nvm_sec_m_t sec_m;
    config_nvm_sec_n_t sec_n;
    config_nvm_sec_o_t sec_o;
} config_nvm_params_t;


/* ===========================================================================
 * NVM record wrapper  -  storage module only
 * =========================================================================== */

/** Magic byte at the start of every valid NVM record. */
#define CONFIG_NVM_MAGIC           (0xA5U)

/**
 * Increment whenever config_nvm_params_t layout changes.
 * The storage module must refuse a record with a non-matching version
 * and trigger a factory-defaults restore.
 */
#define CONFIG_NVM_FORMAT_VERSION  (1U)

/**
 * @brief NVM record header (6 bytes = CONFIG_RECORD_OVERHEAD_BYTES).
 *
 * Written immediately before the config_nvm_params_t payload in EEPROM.
 */
typedef struct __attribute__((packed)) {
    uint8_t  magic;    /**< Must equal CONFIG_NVM_MAGIC                           */
    uint8_t  version;  /**< Must equal CONFIG_NVM_FORMAT_VERSION                  */
    uint16_t length;   /**< sizeof(config_nvm_params_t); guards against truncation */
    uint16_t crc16;    /**< CRC-16/CCITT over the config_nvm_params_t bytes       */
} config_nvm_header_t;

/**
 * @brief Complete NVM record (header + full parameter payload).
 *
 * The storage module reads / writes this as one binary blob.
 * sizeof(config_nvm_record_t) == CONFIG_TOTAL_RECORD_BYTES.
 */
typedef struct __attribute__((packed)) {
    config_nvm_header_t header;
    config_nvm_params_t params;
} config_nvm_record_t;


/* ===========================================================================
 * Compile-time layout assertions
 *
 * A build failure here means a struct silently gained padding or a field was
 * reordered.  Fix the struct or add __attribute__((packed)) and re-verify
 * the sizes against the EEPROM memory map.
 * =========================================================================== */

#ifdef __cplusplus

/* --- User struct sizes --- */
static_assert(sizeof(config_sec_a_t) ==  8U, "Section A user struct size mismatch");
static_assert(sizeof(config_sec_b_t) == 10U, "Section B user struct size mismatch");
static_assert(sizeof(config_sec_c_t) ==  6U, "Section C user struct size mismatch");
static_assert(sizeof(config_sec_d_t) ==  6U, "Section D user struct size mismatch");
static_assert(sizeof(config_sec_e_t) == 24U, "Section E user struct size mismatch");
static_assert(sizeof(config_sec_f_t) ==  8U, "Section F user struct size mismatch");
static_assert(sizeof(config_sec_g_t) ==  4U, "Section G user struct size mismatch");
static_assert(sizeof(config_sec_h_t) ==  2U, "Section H user struct size mismatch");
static_assert(sizeof(config_sec_i_t) ==  8U, "Section I user struct size mismatch");
static_assert(sizeof(config_sec_j_t) == 10U, "Section J user struct size mismatch");
static_assert(sizeof(config_sec_k_t) ==  4U, "Section K user struct size mismatch");
static_assert(sizeof(config_sec_l_t) == 80U, "Section L user struct size mismatch");
static_assert(sizeof(config_sec_m_t) ==  6U, "Section M user struct size mismatch");
static_assert(sizeof(config_sec_n_t) ==  8U, "Section N user struct size mismatch");

/* --- NVM struct sizes (must match EEPROM memory map exactly) --- */
static_assert(sizeof(config_nvm_sec_a_t) ==  9U, "NVM Section A size mismatch");
static_assert(sizeof(config_nvm_sec_b_t) == 11U, "NVM Section B size mismatch");
static_assert(sizeof(config_nvm_sec_c_t) ==  7U, "NVM Section C size mismatch");
static_assert(sizeof(config_nvm_sec_d_t) ==  7U, "NVM Section D size mismatch");
static_assert(sizeof(config_nvm_sec_e_t) == 25U, "NVM Section E size mismatch");
static_assert(sizeof(config_nvm_sec_f_t) ==  9U, "NVM Section F size mismatch");
static_assert(sizeof(config_nvm_sec_g_t) ==  5U, "NVM Section G size mismatch");
static_assert(sizeof(config_nvm_sec_h_t) ==  3U, "NVM Section H size mismatch");
static_assert(sizeof(config_nvm_sec_i_t) ==  9U, "NVM Section I size mismatch");
static_assert(sizeof(config_nvm_sec_j_t) == 11U, "NVM Section J size mismatch");
static_assert(sizeof(config_nvm_sec_k_t) ==  5U, "NVM Section K size mismatch");
static_assert(sizeof(config_nvm_sec_l_t) == 81U, "NVM Section L size mismatch");
static_assert(sizeof(config_nvm_sec_m_t) ==  7U, "NVM Section M size mismatch");
static_assert(sizeof(config_nvm_sec_n_t) ==  9U, "NVM Section N size mismatch");
static_assert(sizeof(config_nvm_sec_o_t) == 37U, "NVM Section O size mismatch");

/* --- Record-level --- */
static_assert(sizeof(config_nvm_params_t) == CONFIG_NVM_PAYLOAD_BYTES,     "NVM payload size mismatch");
static_assert(sizeof(config_nvm_header_t) == CONFIG_RECORD_OVERHEAD_BYTES, "NVM header size mismatch");
static_assert(sizeof(config_nvm_record_t) == CONFIG_TOTAL_RECORD_BYTES,    "NVM record total size mismatch");

#else /* C11 */

/* --- User struct sizes --- */
_Static_assert(sizeof(config_sec_a_t) ==  8U, "Section A user struct size mismatch");
_Static_assert(sizeof(config_sec_b_t) == 10U, "Section B user struct size mismatch");
_Static_assert(sizeof(config_sec_c_t) ==  6U, "Section C user struct size mismatch");
_Static_assert(sizeof(config_sec_d_t) ==  6U, "Section D user struct size mismatch");
_Static_assert(sizeof(config_sec_e_t) == 24U, "Section E user struct size mismatch");
_Static_assert(sizeof(config_sec_f_t) ==  8U, "Section F user struct size mismatch");
_Static_assert(sizeof(config_sec_g_t) ==  4U, "Section G user struct size mismatch");
_Static_assert(sizeof(config_sec_h_t) ==  2U, "Section H user struct size mismatch");
_Static_assert(sizeof(config_sec_i_t) ==  8U, "Section I user struct size mismatch");
_Static_assert(sizeof(config_sec_j_t) == 10U, "Section J user struct size mismatch");
_Static_assert(sizeof(config_sec_k_t) ==  4U, "Section K user struct size mismatch");
_Static_assert(sizeof(config_sec_l_t) == 80U, "Section L user struct size mismatch");
_Static_assert(sizeof(config_sec_m_t) ==  6U, "Section M user struct size mismatch");
_Static_assert(sizeof(config_sec_n_t) ==  8U, "Section N user struct size mismatch");

/* --- NVM struct sizes --- */
_Static_assert(sizeof(config_nvm_sec_a_t) ==  9U, "NVM Section A size mismatch");
_Static_assert(sizeof(config_nvm_sec_b_t) == 11U, "NVM Section B size mismatch");
_Static_assert(sizeof(config_nvm_sec_c_t) ==  7U, "NVM Section C size mismatch");
_Static_assert(sizeof(config_nvm_sec_d_t) ==  7U, "NVM Section D size mismatch");
_Static_assert(sizeof(config_nvm_sec_e_t) == 25U, "NVM Section E size mismatch");
_Static_assert(sizeof(config_nvm_sec_f_t) ==  9U, "NVM Section F size mismatch");
_Static_assert(sizeof(config_nvm_sec_g_t) ==  5U, "NVM Section G size mismatch");
_Static_assert(sizeof(config_nvm_sec_h_t) ==  3U, "NVM Section H size mismatch");
_Static_assert(sizeof(config_nvm_sec_i_t) ==  9U, "NVM Section I size mismatch");
_Static_assert(sizeof(config_nvm_sec_j_t) == 11U, "NVM Section J size mismatch");
_Static_assert(sizeof(config_nvm_sec_k_t) ==  5U, "NVM Section K size mismatch");
_Static_assert(sizeof(config_nvm_sec_l_t) == 81U, "NVM Section L size mismatch");
_Static_assert(sizeof(config_nvm_sec_m_t) ==  7U, "NVM Section M size mismatch");
_Static_assert(sizeof(config_nvm_sec_n_t) ==  9U, "NVM Section N size mismatch");
_Static_assert(sizeof(config_nvm_sec_o_t) == 37U, "NVM Section O size mismatch");

/* --- Record-level --- */
_Static_assert(sizeof(config_nvm_params_t) == CONFIG_NVM_PAYLOAD_BYTES,     "NVM payload size mismatch");
_Static_assert(sizeof(config_nvm_header_t) == CONFIG_RECORD_OVERHEAD_BYTES, "NVM header size mismatch");
_Static_assert(sizeof(config_nvm_record_t) == CONFIG_TOTAL_RECORD_BYTES,    "NVM record total size mismatch");

#endif /* __cplusplus */


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CONFIG_PARAMS_H_ */