#ifndef STORAGE_MODULE_H_
#define STORAGE_MODULE_H_

#include <stdint.h>
#include <stdbool.h>
#include <common_config/config_params.h>

#ifdef __cplusplus
extern "C"{
#endif

/**
 * @brief Storage Module status / error codes.
 */
typedef enum {
	SM_OK = 0,
	SM_ERR_INVALID_PARAM,
	SM_ERR_INVALID_SECTION,
	SM_ERR_INVALID_PARTITION,
	SM_ERR_HW_FAILED,
	SM_ERR_NOT_INITIALIZED,
	SM_ERR_READ_ERROR,
	SM_ERR_WRITE_ERROR,
	SM_ERR_VERIFY_ERROR,      /**< Raw write->read-back byte compare failed (HW-level). */
	SM_ERR_SECTION_ERASED,
	SM_ERR_SECTION_CORRUPTED,
	SM_ERR_CORRUPTED,         /**< Record CRC does not match on load. */
	SM_ERR_NO_DATA,           /**< No valid configuration available anywhere. */
} sm_status_t;

/**
 * @brief The two logical configuration types the application is aware of.
 *
 * Internally, USER is backed by up to two physical EEPROM partitions
 * (a primary "Slot 1" and an optional secondary/backup "Slot 2"), but that
 * split is entirely private to the Storage Module. The application only
 * ever selects USER or FACTORY - it cannot address a slot directly.
 */
typedef enum {
	STORAGE_CONFIG_FACTORY = 0,
	STORAGE_CONFIG_USER,
} storage_config_t;

/**
 * @brief Health/validity of a logical configuration type, as reported by
 *        SM_Init().
 */
typedef enum {
	SM_CFG_INVALID = 0,
	SM_CFG_VALID,
} sm_validity_t;

/**
 * @brief Initialize the Storage Module and perform the power-up partition
 *        health check.
 *
 * USER health check:
 *   1. Check and validate the CRC of Slot 1.
 *   2. If Slot 1 is valid, *user_sts = SM_CFG_VALID.
 *   3. If Slot 1 is invalid and Slot 2 is available, check and validate
 *      Slot 2.
 *   4. If Slot 2 is valid, copy Slot 2's configuration into Slot 1 (Slot 2
 *      itself is left completely untouched - not erased, cleared or
 *      modified) and set *user_sts = SM_CFG_VALID.
 *   5. If neither slot holds a valid configuration, *user_sts = SM_CFG_INVALID.
 *
 * FACTORY health check:
 *   1. Check whether a Factory configuration exists and validate its CRC.
 *   2. *factory_sts = SM_CFG_VALID if present and valid, SM_CFG_INVALID
 *      otherwise (missing or corrupted).
 *
 * The application only ever learns the validity of USER / FACTORY; it does
 * not learn (and cannot ask) whether a valid USER configuration originally
 * came from Slot 1 or Slot 2.
 *
 * @param user_sts    Destination for USER validity. Must not be NULL.
 * @param factory_sts Destination for FACTORY validity. Must not be NULL.
 *
 * @return SM_OK once both health checks have completed (check *user_sts
 *         and *factory_sts for the actual result - SM_OK does NOT imply
 *         either configuration is valid). SM_ERR_HW_FAILED if the EEPROM
 *         device itself is not ready, SM_ERR_READ_ERROR on a genuine
 *         EEPROM bus failure, SM_ERR_INVALID_PARAM if either pointer is
 *         NULL.
 */
sm_status_t SM_Init(sm_validity_t *user_sts, sm_validity_t *factory_sts);

/**
 * @brief Read a complete logical configuration, validating its CRC first.
 *
 * For STORAGE_CONFIG_USER:
 *   1. Check Slot 1's CRC. If valid, return the Slot 1 configuration.
 *   2. If Slot 1 is invalid and Slot 2 is available, check Slot 2's CRC.
 *      If valid, return the Slot 2 configuration (Slot 2 is used purely
 *      as the read source here - it is not promoted/copied into Slot 1;
 *      that promotion only happens in SM_Init()).
 *   3. If no valid USER configuration exists anywhere, return SM_ERR_NO_DATA.
 *
 * For STORAGE_CONFIG_FACTORY:
 *   Validate the Factory configuration's CRC and return it, or report that
 *   none is available.
 *
 * @param which     STORAGE_CONFIG_USER or STORAGE_CONFIG_FACTORY.
 * @param cfg_param Destination for the decoded configuration.
 *
 * @return SM_OK on success. SM_ERR_NO_DATA if no valid USER configuration
 *         exists in either slot. SM_ERR_SECTION_ERASED / SM_ERR_CORRUPTED /
 *         SM_ERR_SECTION_CORRUPTED if FACTORY is missing or invalid.
 *         SM_ERR_INVALID_PARTITION if `which` is neither USER nor FACTORY.
 */
sm_status_t SM_ReadConfig(storage_config_t which, config_params_t *cfg_param);

/**
 * @brief Write a complete configuration to the requested logical
 *        destination.
 *
 * This performs ONLY the write operation - it does not calculate or
 * validate the CRC as a side effect, and (for USER) it does not touch the
 * Slot 2 backup. Call SM_ValCrc() afterwards to validate what was written
 * and, for USER, to refresh the Slot 2 backup.
 *
 * @param which     STORAGE_CONFIG_USER (-> Slot 1) or STORAGE_CONFIG_FACTORY.
 * @param cfg_param Source configuration to write.
 *
 * @return SM_OK if the write completed at the hardware level.
 *         SM_ERR_WRITE_ERROR on an EEPROM bus failure.
 *         SM_ERR_INVALID_PARTITION if `which` is neither USER nor FACTORY.
 */
sm_status_t SM_WriteConfig(storage_config_t which, const config_params_t *cfg_param);

/**
 * @brief Perform the complete CRC calculation/validation for a logical
 *        configuration.
 *
 * The CRC is calculated only over the user parameter structure defined in
 * config_params.h - internal NVM metadata (per-section is_configured/
 * clear-status bytes, record header) never participates.
 *
 * For STORAGE_CONFIG_USER, if Slot 1 validates successfully and Slot 2 is
 * available, the now-validated Slot 1 data is also copied to Slot 2 as the
 * secondary backup (this is the only place Slot 2 is written from a
 * normal, non-recovery code path). If Slot 2 is not available, this step
 * is silently skipped - it is not an error.
 *
 * @param which STORAGE_CONFIG_USER or STORAGE_CONFIG_FACTORY.
 *
 * @return SM_OK if the stored CRC matches the recalculated CRC.
 *         SM_ERR_CORRUPTED if it does not.
 *         SM_ERR_SECTION_ERASED if nothing has been written yet.
 *         SM_ERR_INVALID_PARTITION if `which` is neither USER nor FACTORY.
 */
sm_status_t SM_ValCrc(storage_config_t which);

/**
 * @brief Read a single configuration parameter section.
 *
 * The record's CRC is verified before any data is returned. For
 * STORAGE_CONFIG_USER, Slot 1 is checked first, falling back to Slot 2 if
 * Slot 1 is invalid and Slot 2 is available (mirroring SM_ReadConfig()).
 * The section's own clear-status byte is also checked: if the section has
 * been cleared, SM_ERR_SECTION_ERASED is returned instead of stale data.
 *
 * @param which   STORAGE_CONFIG_USER or STORAGE_CONFIG_FACTORY.
 * @param section Section to read.
 * @param data    Destination buffer; must be large enough for the
 *                user-facing struct of that section (e.g. config_sec_a_t
 *                for CONFIG_SEC_A). For CONFIG_SEC_O, only the
 *                EEPROM-backed fields are written; fwv and stm are left
 *                untouched.
 *
 * @return SM_OK on success. SM_ERR_INVALID_SECTION if section is out of
 *         range, SM_ERR_INVALID_PARAM if data is NULL,
 *         SM_ERR_INVALID_PARTITION if `which` is invalid, others on fail.
 */
sm_status_t SM_Read(storage_config_t which, config_param_sec_t section, void *data);

/**
 * @brief Write a single configuration parameter section to the USER
 *        configuration (Slot 1).
 *
 * Before writing the requested section:
 *   1. The complete, currently-valid USER configuration is read into an
 *      internal RAM copy (Slot 1 if valid, else Slot 2 if valid, else a
 *      blank record).
 *   2. Only the requested section is modified in that RAM copy; every
 *      other section is carried over unchanged.
 *   3. The updated RAM copy is committed to Slot 1 (the section being
 *      written is invalidated and re-committed first, so a power loss
 *      mid-write leaves that section reading as cleared rather than
 *      corrupted or stale).
 *   4. The CRC of the updated RAM structure is compared against the CRC
 *      recalculated from what Slot 1 now holds in EEPROM, and the result
 *      is returned.
 *
 * This is specifically a USER-configuration operation; there is no
 * FACTORY equivalent.
 *
 * @param section Section to write.
 * @param data    Source data; must match the user-facing struct of that
 *                section (e.g. config_sec_a_t for CONFIG_SEC_A). For
 *                CONFIG_SEC_O, fwv and stm in the source are ignored -
 *                they are never written to EEPROM.
 *
 * @return SM_OK on success. SM_ERR_INVALID_SECTION if section is out of
 *         range, SM_ERR_INVALID_PARAM if data is NULL,
 *         SM_ERR_VERIFY_ERROR if the post-write CRC check fails, others
 *         on fail.
 */
sm_status_t SM_Write(config_param_sec_t section, const void *data);

/**
 * @brief Clear a configuration parameter section in the USER configuration
 *        (Slot 1).
 *
 * Marks the section's clear-status byte as cleared and zeroes its
 * EEPROM-backed payload. A subsequent SM_Read(STORAGE_CONFIG_USER, ...)
 * of that section returns SM_ERR_SECTION_ERASED.
 *
 * @param section Section to clear.
 *
 * @return SM_OK on success, others on fail.
 */
sm_status_t SM_Clear(config_param_sec_t section);

/**
 * @brief Report whether the Slot 2 secondary backup is available on this
 *        build/EEPROM.
 *
 * This is a diagnostic query only - it does not let the application
 * address Slot 2 in any way. Slot 2 is optional: it is only available
 * when the physical EEPROM has enough room for a third full record
 * (Factory + Slot 1 + Slot 2). When it is not available, the module still
 * works normally with Factory and Slot 1 only, and every Slot-2-related
 * operation is skipped rather than failing.
 *
 * @return true if Slot 2 is available, false otherwise. Meaningless
 *         before SM_Init() has been called successfully.
 */
bool SM_Slot2Available(void);


#ifdef __cplusplus
}
#endif

#endif /* STORAGE_MODULE_H_ */