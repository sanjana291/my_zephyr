#ifndef STORAGE_MODULE_INTERNAL_H_
#define STORAGE_MODULE_INTERNAL_H_

#include <stddef.h>
#include <stdint.h>
#include <common_config/config_params.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * Partition layout
 *
 * Each partition stores one complete config_nvm_record_t (6-byte header +
 * the full config_nvm_params_t payload, CONFIG_TOTAL_RECORD_BYTES total).
 * Partitions are laid out back-to-back in the EEPROM:
 *
 *   Partition 0 - Factory  (mandatory)
 *   Partition 1 - Slot 1   (mandatory - primary user configuration)
 *   Partition 2 - Slot 2   (optional  - secondary/backup configuration)
 *
 * Factory and Slot 1 must always fit; this is enforced at compile time
 * against CONFIG_EEPROM_ALLOCATION_BYTES below. Slot 2 is not required to
 * fit: whether it is actually usable is decided at *runtime* in
 * storage_module.c by comparing SM_PARTITION_OFFSET(SM_PART_SLOT2) +
 * SM_RECORD_SIZE against the real EEPROM device size (eeprom_get_size()).
 * This means Slot 2 becomes available automatically - no code change
 * required - the moment a large-enough EEPROM is fitted, even if
 * CONFIG_EEPROM_ALLOCATION_BYTES has not been bumped.
 * =========================================================================== */

typedef enum {
	SM_PART_FACTORY = 0,   /**< Partition 0 - factory configuration (mandatory)        */
	SM_PART_SLOT1,          /**< Partition 1 - primary user configuration (mandatory)   */
	SM_PART_SLOT2,          /**< Partition 2 - secondary/backup configuration (optional)*/

	SM_PART_COUNT
} sm_partition_id_t;

/** Number of partitions that MUST fit in the EEPROM (Factory + Slot 1). */
#define SM_MANDATORY_PART_COUNT  (2U)

/** Size in bytes of a single partition (one full NVM record). */
#define SM_RECORD_SIZE  ((size_t)CONFIG_TOTAL_RECORD_BYTES)

/** Offset of partition `idx` from the start of the EEPROM. */
#define SM_PARTITION_OFFSET(idx)  ((size_t)(idx) * SM_RECORD_SIZE)

/* Fail the build if the two mandatory partitions (Factory + Slot 1) do not
 * fit in the EEPROM. Slot 2 is intentionally NOT part of this check - it
 * is optional by design and its availability is decided at runtime.
 */
_Static_assert(((size_t)SM_MANDATORY_PART_COUNT * SM_RECORD_SIZE) <= (size_t)CONFIG_EEPROM_ALLOCATION_BYTES,
	       "Storage Module: Factory + Slot 1 do not fit in CONFIG_EEPROM_ALLOCATION_BYTES");

/** CRC-16/CCITT seed used for every write and verify. */
#define SM_CRC16_SEED  (0xFFFFU)

/* ===========================================================================
 * Section byte-range table
 *
 * Describes where each config_param_sec_t section lives inside
 * config_nvm_params_t (storage-side, includes the is_configured byte).
 * Used for:
 *   - computing the payload CRC (see sm_calc_payload_crc()), which must
 *     skip every section's is_configured byte since that is internal NVM
 *     metadata and not user parameter data;
 *   - SM_Clear(), which zeroes a section's EEPROM-backed payload without
 *     needing to know the field layout.
 *
 * Field-by-field encode/decode between config_params_t and
 * config_nvm_params_t (which is where user-only fields like sec_o.fwv /
 * sec_o.stm get dropped, and where a couple of sections have a different
 * on-wire field order than their user-facing struct, e.g. Section I) is
 * done separately, per section, in storage_module.c - NOT via this table.
 * A single generic memcpy keyed only by size is not safe for those
 * sections; see sm_encode_sec_i()/sm_decode_sec_o() and friends.
 * =========================================================================== */

typedef struct {
	size_t nvm_offset;   /**< Offset of the section (incl. is_configured byte)
			       *   within config_nvm_params_t.                    */
	size_t nvm_size;      /**< Size of the section incl. the is_configured byte;
				*   this is how many bytes SM_Clear() wipes.       */
} sm_section_range_t;

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_MODULE_INTERNAL_H_ */