#include <string.h>

#include <storage_module/storage_module.h>
#include "storage_module_internal.h"

#include <zephyr/drivers/eeprom.h>
#include <zephyr/sys/crc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(storage_module, CONFIG_STORAGE_MODULE_LOG_LEVEL);

static const struct device *eeprom_dev = DEVICE_DT_GET(DT_ALIAS(eeprom_0));
static bool sm_ready;
static bool sm_slot2_available;
/* Internal RAM buffer for USER configuration */
static config_params_t sm_user_ram;
/* ===========================================================================
 * Section byte-range table (CRC + SM_Clear only - see storage_module_internal.h)
 * =========================================================================== */

#define SM_SECTION_RANGE(letter) \
	{ \
		.nvm_offset = offsetof(config_nvm_params_t, sec_##letter), \
		.nvm_size   = sizeof(config_nvm_sec_##letter##_t), \
	}

static const sm_section_range_t sm_sections[CONFIG_SEC_MAX] = {
	[CONFIG_SEC_A] = SM_SECTION_RANGE(a),
	[CONFIG_SEC_B] = SM_SECTION_RANGE(b),
	[CONFIG_SEC_C] = SM_SECTION_RANGE(c),
	[CONFIG_SEC_D] = SM_SECTION_RANGE(d),
	[CONFIG_SEC_E] = SM_SECTION_RANGE(e),
	[CONFIG_SEC_F] = SM_SECTION_RANGE(f),
	[CONFIG_SEC_G] = SM_SECTION_RANGE(g),
	[CONFIG_SEC_H] = SM_SECTION_RANGE(h),
	[CONFIG_SEC_I] = SM_SECTION_RANGE(i),
	[CONFIG_SEC_J] = SM_SECTION_RANGE(j),
	[CONFIG_SEC_K] = SM_SECTION_RANGE(k),
	[CONFIG_SEC_L] = SM_SECTION_RANGE(l),
	[CONFIG_SEC_M] = SM_SECTION_RANGE(m),
	[CONFIG_SEC_N] = SM_SECTION_RANGE(n),
	[CONFIG_SEC_O] = SM_SECTION_RANGE(o),
};

#undef SM_SECTION_RANGE

/** EEPROM byte offset of each partition. */
static const size_t sm_partition_offset[SM_PART_COUNT] = {
	[SM_PART_FACTORY] = SM_PARTITION_OFFSET(SM_PART_FACTORY),
	[SM_PART_SLOT1]    = SM_PARTITION_OFFSET(SM_PART_SLOT1),
	[SM_PART_SLOT2]    = SM_PARTITION_OFFSET(SM_PART_SLOT2),
};

static void print_config_param( config_params_t g_config){
    LOG_INF("SEC A\r\n");
    LOG_INF("GFF: %d\tLWS: %d\tPIB:%d\r\n", g_config.sec_a.gff,g_config.sec_a.lws, g_config.sec_a.pib);

    LOG_INF("\nSEC B\r\n");
    LOG_INF("PWS: %d\tPWC: %d\tWGO: %d\r\n", g_config.sec_b.pws, g_config.sec_b.pwc, g_config.sec_b.wgo);

    LOG_INF("\nSEC C\r\n");
    LOG_INF("GDP: %d\tPLD: %d\tEFC: %d\r\n", g_config.sec_c.gdp, g_config.sec_c.pld, g_config.sec_c.efc);

    LOG_INF("\nSEC D\r\n");
    LOG_INF("SWL: %d\tSLD: %d\tLSP: %d\r\n", g_config.sec_d.swl, g_config.sec_d.sld, g_config.sec_d.lsp);

    LOG_INF("\nSEC E\r\n");
    LOG_INF("AUR: %d\tAUT: %d\tALR: %d\r\n", g_config.sec_e.aur, g_config.sec_e.aut, g_config.sec_e.alr);
    LOG_INF("ALT: %d\tMAS: %d\tMAT: %d\r\n", g_config.sec_e.alt, g_config.sec_e.mas, g_config.sec_e.mat);
    LOG_INF("MUS: %d\tMUT: %d\tDUD: %d\r\n", g_config.sec_e.mus, g_config.sec_e.mut, g_config.sec_e.dud);
    LOG_INF("DUT: %d\tDLD: %d\tDLT: %d\r\n", g_config.sec_e.dut, g_config.sec_e.dud, g_config.sec_e.dlt);
    LOG_INF("LAS: %d\tLAT: %d\tLDS: %d\r\n", g_config.sec_e.las, g_config.sec_e.lat, g_config.sec_e.lds);
    LOG_INF("LDS: %d\r\n", g_config.sec_e.lds);

    LOG_INF("\nSEC F\r\n");
    LOG_INF("SSD: %d\tRNW: %d\tRNP: %d\r\n", g_config.sec_f.ssd, g_config.sec_f.rnw, g_config.sec_f.rnp);
    LOG_INF("SDP: %d\r\n", g_config.sec_f.sdp);

    LOG_INF("\nSEC G\r\n");
    LOG_INF("ASR: %d\tATL: %d\r\n", g_config.sec_g.asr, g_config.sec_g.atl);

    LOG_INF("\nSEC H\r\n");
    LOG_INF("LFT: %d\r\n", g_config.sec_h.lft);

    LOG_INF("\nSEC I\r\n");
    LOG_INF("API: %d\tPKD: %d\tMSI: %d\r\n", g_config.sec_i.api, g_config.sec_i.pkd, g_config.sec_i.msi);

    LOG_INF("\nSEC J\r\n");
    LOG_INF("FSO: %d\tRSO: %d\tFPO: %d\r\n", g_config.sec_j.fso, g_config.sec_j.rso, g_config.sec_j.fpo);
    LOG_INF("RPO: %d\tPTC: %d\r\n", g_config.sec_j.rpo, g_config.sec_j.ptc);

    LOG_INF("\nSEC K\r\n");
    LOG_INF("WHD: %d\tGBR: %d\r\n", g_config.sec_k.whd, g_config.sec_k.gbr);

    LOG_INF("\nSEC L\r\n");
    LOG_INF("SID: %s\tWPS: %s\r\n", g_config.sec_l.sid, g_config.sec_l.wps);
    LOG_INF("WAT: %d\tWST: %d\tWIP: %d\r\n", g_config.sec_l.wat, g_config.sec_l.wst,  g_config.sec_l.wip);
    LOG_INF("WSM: %d\tWGI: %d\tRDN: %s\r\n", g_config.sec_l.wsm, g_config.sec_l.wgi,  g_config.sec_l.rdn);
    LOG_INF("RCP: %d\r\n", g_config.sec_l.rcp);

    LOG_INF("\nSEC M\r\n");
    LOG_INF("RAD: %d\tSYW: %d\tCID: %d\r\n", g_config.sec_m.rad, g_config.sec_m.syw, g_config.sec_m.cid);
    LOG_INF("SND: %d\tPMT: %d\r\n", g_config.sec_m.snd, g_config.sec_m.pmt);

    LOG_INF("\nSEC N\r\n");
    LOG_INF("COM: %d\tMPH: %d\tPLM: %d\r\n", g_config.sec_n.com, g_config.sec_n.mph, g_config.sec_n.plm);
    LOG_INF("MNO: %d\tMPL: %d\r\n", g_config.sec_n.mno, g_config.sec_n.mpl);
}

/* ===========================================================================
 * Per-section encode / decode
 *
 * Field-by-field on purpose: a raw memcpy keyed only by size/offset is
 * NOT safe for every section, because
 *   (a) some sections (e.g. Section O) carry user-only fields (fwv, stm)
 *       that must never reach EEPROM or the CRC, and
 *   (b) at least one section (Section I) intentionally uses a different
 *       field order in its user-facing struct than in its packed NVM
 *       struct (see the comment on config_sec_i_t / config_nvm_sec_i_t
 *       in config_params.h), so a byte-range copy would silently
 *       scramble the data.
 * Named field assignment sidesteps both problems and is immune to any
 * future reordering or padding changes on either side.
 * =========================================================================== */

static void sm_encode_sec_a(const config_sec_a_t *u, config_nvm_sec_a_t *n)
{
	n->is_configured = CONFIG_SECTION_CONFIGURED;
	n->gff = u->gff;
	n->lws = u->lws;
	n->pib = u->pib;
}
static void sm_decode_sec_a(const config_nvm_sec_a_t *n, config_sec_a_t *u)
{
	u->gff = n->gff;
	u->lws = n->lws;
	u->pib = n->pib;
}

static void sm_encode_sec_b(const config_sec_b_t *u, config_nvm_sec_b_t *n)
{
	n->is_configured = CONFIG_SECTION_CONFIGURED;
	n->pws = u->pws;
	n->pwc = u->pwc;
	n->wgo = u->wgo;
}
static void sm_decode_sec_b(const config_nvm_sec_b_t *n, config_sec_b_t *u)
{
	u->pws = n->pws;
	u->pwc = n->pwc;
	u->wgo = n->wgo;
}

static void sm_encode_sec_c(const config_sec_c_t *u, config_nvm_sec_c_t *n)
{
	n->is_configured = CONFIG_SECTION_CONFIGURED;
	n->gdp = u->gdp;
	n->pld = u->pld;
	n->efc = u->efc;
}
static void sm_decode_sec_c(const config_nvm_sec_c_t *n, config_sec_c_t *u)
{
	u->gdp = n->gdp;
	u->pld = n->pld;
	u->efc = n->efc;
}

static void sm_encode_sec_d(const config_sec_d_t *u, config_nvm_sec_d_t *n)
{
	n->is_configured = CONFIG_SECTION_CONFIGURED;
	n->swl = u->swl;
	n->sld = u->sld;
	n->lsp = u->lsp;
}
static void sm_decode_sec_d(const config_nvm_sec_d_t *n, config_sec_d_t *u)
{
	u->swl = n->swl;
	u->sld = n->sld;
	u->lsp = n->lsp;
}

static void sm_encode_sec_e(const config_sec_e_t *u, config_nvm_sec_e_t *n)
{
	n->is_configured = CONFIG_SECTION_CONFIGURED;
	n->aur = u->aur; n->aut = u->aut;
	n->alr = u->alr; n->alt = u->alt;
	n->mas = u->mas; n->mat = u->mat;
	n->mus = u->mus; n->mut = u->mut;
	n->dud = u->dud; n->dut = u->dut;
	n->dld = u->dld; n->dlt = u->dlt;
	n->las = u->las; n->lat = u->lat;
	n->lds = u->lds; n->ldt = u->ldt;
}
static void sm_decode_sec_e(const config_nvm_sec_e_t *n, config_sec_e_t *u)
{
	u->aur = n->aur; u->aut = n->aut;
	u->alr = n->alr; u->alt = n->alt;
	u->mas = n->mas; u->mat = n->mat;
	u->mus = n->mus; u->mut = n->mut;
	u->dud = n->dud; u->dut = n->dut;
	u->dld = n->dld; u->dlt = n->dlt;
	u->las = n->las; u->lat = n->lat;
	u->lds = n->lds; u->ldt = n->ldt;
}

static void sm_encode_sec_f(const config_sec_f_t *u, config_nvm_sec_f_t *n)
{
	n->is_configured = CONFIG_SECTION_CONFIGURED;
	n->ssd = u->ssd;
	n->rnw = u->rnw;
	n->rnp = u->rnp;
	n->sdp = u->sdp;
}
static void sm_decode_sec_f(const config_nvm_sec_f_t *n, config_sec_f_t *u)
{
	u->ssd = n->ssd;
	u->rnw = n->rnw;
	u->rnp = n->rnp;
	u->sdp = n->sdp;
}

static void sm_encode_sec_g(const config_sec_g_t *u, config_nvm_sec_g_t *n)
{
	n->is_configured = CONFIG_SECTION_CONFIGURED;
	n->asr = u->asr;
	n->atl = u->atl;
}
static void sm_decode_sec_g(const config_nvm_sec_g_t *n, config_sec_g_t *u)
{
	u->asr = n->asr;
	u->atl = n->atl;
}

static void sm_encode_sec_h(const config_sec_h_t *u, config_nvm_sec_h_t *n)
{
	n->is_configured = CONFIG_SECTION_CONFIGURED;
	n->lft = u->lft;
}
static void sm_decode_sec_h(const config_nvm_sec_h_t *n, config_sec_h_t *u)
{
	u->lft = n->lft;
}

/* Section I: NOTE the deliberate field-order difference between
 * config_sec_i_t (api, pkd, msi) and config_nvm_sec_i_t / wire order
 * (pkd, api, msi) - see config_params.h. Named assignment makes the
 * order irrelevant.
 */
static void sm_encode_sec_i(const config_sec_i_t *u, config_nvm_sec_i_t *n)
{
	n->is_configured = CONFIG_SECTION_CONFIGURED;
	n->pkd = u->pkd;
	n->api = u->api;
	n->msi = u->msi;
}
static void sm_decode_sec_i(const config_nvm_sec_i_t *n, config_sec_i_t *u)
{
	u->api = n->api;
	u->pkd = n->pkd;
	u->msi = n->msi;
}

static void sm_encode_sec_j(const config_sec_j_t *u, config_nvm_sec_j_t *n)
{
	n->is_configured = CONFIG_SECTION_CONFIGURED;
	n->fso = u->fso;
	n->rso = u->rso;
	n->fpo = u->fpo;
	n->rpo = u->rpo;
	n->ptc = u->ptc;
}
static void sm_decode_sec_j(const config_nvm_sec_j_t *n, config_sec_j_t *u)
{
	u->fso = n->fso;
	u->rso = n->rso;
	u->fpo = n->fpo;
	u->rpo = n->rpo;
	u->ptc = n->ptc;
}

static void sm_encode_sec_k(const config_sec_k_t *u, config_nvm_sec_k_t *n)
{
	n->is_configured = CONFIG_SECTION_CONFIGURED;
	n->whd = u->whd;
	n->gbr = u->gbr;
}
static void sm_decode_sec_k(const config_nvm_sec_k_t *n, config_sec_k_t *u)
{
	u->whd = n->whd;
	u->gbr = n->gbr;
}

static void sm_encode_sec_l(const config_sec_l_t *u, config_nvm_sec_l_t *n)
{
	n->is_configured = CONFIG_SECTION_CONFIGURED;
	n->ssz = u->ssz;
	memcpy(n->sid, u->sid, sizeof(n->sid));
	n->psz = u->psz;
	memcpy(n->wps, u->wps, sizeof(n->wps));
	n->wat = u->wat;
	n->wst = u->wst;
	n->wip = u->wip;
	n->wsm = u->wsm;
	n->wgi = u->wgi;
	memcpy(n->rdn, u->rdn, sizeof(n->rdn));
	n->rcp = u->rcp;
}
static void sm_decode_sec_l(const config_nvm_sec_l_t *n, config_sec_l_t *u)
{
	u->ssz = n->ssz;
	memcpy(u->sid, n->sid, sizeof(u->sid));
	u->psz = n->psz;
	memcpy(u->wps, n->wps, sizeof(u->wps));
	u->wat = n->wat;
	u->wst = n->wst;
	u->wip = n->wip;
	u->wsm = n->wsm;
	u->wgi = n->wgi;
	memcpy(u->rdn, n->rdn, sizeof(u->rdn));
	u->rcp = n->rcp;
}

static void sm_encode_sec_m(const config_sec_m_t *u, config_nvm_sec_m_t *n)
{
	n->is_configured = CONFIG_SECTION_CONFIGURED;
	n->rad = u->rad;
	n->syw = u->syw;
	n->cid = u->cid;
	n->snd = u->snd;
	n->pmt = u->pmt;
}
static void sm_decode_sec_m(const config_nvm_sec_m_t *n, config_sec_m_t *u)
{
	u->rad = n->rad;
	u->syw = n->syw;
	u->cid = n->cid;
	u->snd = n->snd;
	u->pmt = n->pmt;
}

static void sm_encode_sec_n(const config_sec_n_t *u, config_nvm_sec_n_t *n)
{
	n->is_configured = CONFIG_SECTION_CONFIGURED;
	n->com = u->com;
	n->mph = u->mph;
	n->plm = u->plm;
	n->mno = u->mno;
	n->mpl = u->mpl;
}
static void sm_decode_sec_n(const config_nvm_sec_n_t *n, config_sec_n_t *u)
{
	u->com = n->com;
	u->mph = n->mph;
	u->plm = n->plm;
	u->mno = n->mno;
	u->mpl = n->mpl;
}

static void sm_encode_sec_o(const config_sec_o_t *u, config_nvm_sec_o_t *n)
{
	n->is_configured = CONFIG_SECTION_CONFIGURED;
	n->hwv = u->hwv;
	n->tod = u->tod;
	n->sct = u->sct;
	n->rct = u->rct;
	n->psc = u->psc;
	n->plc = u->plc;
	n->lct = u->lct;
	n->m4c = u->m4c;
	n->plt = u->plt;
	n->tmn = u->tmn;
	memset(n->reserved, 0, sizeof(n->reserved));
}
static void sm_decode_sec_o(const config_nvm_sec_o_t *n, config_sec_o_t *u)
{
	/* u->fwv and u->stm intentionally untouched - see comment above. */
	u->hwv = n->hwv;
	u->tod = n->tod;
	u->sct = n->sct;
	u->rct = n->rct;
	u->psc = n->psc;
	u->plc = n->plc;
	u->lct = n->lct;
	u->m4c = n->m4c;
	u->plt = n->plt;
	u->tmn = n->tmn;
}

/**
 * @brief Encode the complete user-facing configuration into its EEPROM
 *        wire form. Every section is marked CONFIG_SECTION_CONFIGURED -
 *        this is only used for whole-record writes (SM_WriteConfig()),
 *        never for a partial/single-section update.
 */
static void sm_encode_params(const config_params_t *u, config_nvm_params_t *n)
{
	sm_encode_sec_a(&u->sec_a, &n->sec_a);
	sm_encode_sec_b(&u->sec_b, &n->sec_b);
	sm_encode_sec_c(&u->sec_c, &n->sec_c);
	sm_encode_sec_d(&u->sec_d, &n->sec_d);
	sm_encode_sec_e(&u->sec_e, &n->sec_e);
	sm_encode_sec_f(&u->sec_f, &n->sec_f);
	sm_encode_sec_g(&u->sec_g, &n->sec_g);
	sm_encode_sec_h(&u->sec_h, &n->sec_h);
	sm_encode_sec_i(&u->sec_i, &n->sec_i);
	sm_encode_sec_j(&u->sec_j, &n->sec_j);
	sm_encode_sec_k(&u->sec_k, &n->sec_k);
	sm_encode_sec_l(&u->sec_l, &n->sec_l);
	sm_encode_sec_m(&u->sec_m, &n->sec_m);
	sm_encode_sec_n(&u->sec_n, &n->sec_n);
	sm_encode_sec_o(&u->sec_o, &n->sec_o);
}

/**
 * @brief Decode a full NVM payload into the user-facing struct. Does NOT
 *        touch cfg_param->sec_o.fwv / cfg_param->sec_o.stm - those are
 *        user-only fields that were never in the EEPROM record to begin
 *        with, so the caller's existing values are preserved as-is.
 */
static void sm_decode_params(const config_nvm_params_t *n, config_params_t *u)
{
	sm_decode_sec_a(&n->sec_a, &u->sec_a);
	sm_decode_sec_b(&n->sec_b, &u->sec_b);
	sm_decode_sec_c(&n->sec_c, &u->sec_c);
	sm_decode_sec_d(&n->sec_d, &u->sec_d);
	sm_decode_sec_e(&n->sec_e, &u->sec_e);
	sm_decode_sec_f(&n->sec_f, &u->sec_f);
	sm_decode_sec_g(&n->sec_g, &u->sec_g);
	sm_decode_sec_h(&n->sec_h, &u->sec_h);
	sm_decode_sec_i(&n->sec_i, &u->sec_i);
	sm_decode_sec_j(&n->sec_j, &u->sec_j);
	sm_decode_sec_k(&n->sec_k, &u->sec_k);
	sm_decode_sec_l(&n->sec_l, &u->sec_l);
	sm_decode_sec_m(&n->sec_m, &u->sec_m);
	sm_decode_sec_n(&n->sec_n, &u->sec_n);
	sm_decode_sec_o(&n->sec_o, &u->sec_o);
}

/* ===========================================================================
 * Internal helpers
 * =========================================================================== */

/**
 * @brief CRC over user parameter data ONLY.
 *
 * Chains crc16_ccitt() across every section's payload bytes while
 * skipping each section's is_configured byte (internal NVM metadata).
 * User-only fields (sec_o.fwv, sec_o.stm) are already absent from
 * config_nvm_params_t, so they are automatically excluded too. Because
 * crc16_ccitt() applies no final XOR, feeding the running CRC back in as
 * the next call's seed is equivalent to computing one CRC over the
 * sections' payload bytes concatenated together - i.e. exactly "only the
 * actual user parameter data", with no record header, no per-section
 * status byte, and no internal-only field ever contributing.
 */
static uint16_t sm_calc_payload_crc(const config_nvm_params_t *payload)
{
	const uint8_t *base = (const uint8_t *)payload;
	uint16_t crc = SM_CRC16_SEED;

	for (size_t i = 0; i < CONFIG_SEC_MAX; i++) {
		const sm_section_range_t *r = &sm_sections[i];
		const uint8_t *field = base + r->nvm_offset + 1U; /* skip is_configured */
		size_t field_len = r->nvm_size - 1U;

		crc = crc16_ccitt(crc, field, field_len);
	}

	return crc;
}

/**
 * @brief Build a blank/fresh record: valid-looking header, every section
 *        marked cleared. Used whenever a partition has no usable data yet.
 */
static void sm_build_blank_record(config_nvm_record_t *rec)
{
	memset(rec, 0, sizeof(*rec));

	for (size_t i = 0; i < CONFIG_SEC_MAX; i++) {
		uint8_t *is_cfg = (uint8_t *)&rec->params + sm_sections[i].nvm_offset;

		*is_cfg = CONFIG_SECTION_CLEARED;
	}
}

/**
 * @brief Read and fully validate one partition's record (magic, version,
 *        length, CRC).
 *
 * @return SM_OK if the record is valid.
 *         SM_ERR_READ_ERROR on an EEPROM bus failure.
 *         SM_ERR_SECTION_ERASED if the partition has never been written
 *         (magic missing - fresh/blank EEPROM).
 *         SM_ERR_SECTION_CORRUPTED if the magic is present but the
 *         version or length is wrong (format mismatch).
 *         SM_ERR_CORRUPTED if the stored CRC does not match the payload.
 */
static sm_status_t sm_partition_load(sm_partition_id_t part, config_nvm_record_t *rec)
{
	int ret = eeprom_read(eeprom_dev, sm_partition_offset[part], rec, sizeof(*rec));

	if (ret < 0) {
		LOG_ERR("Partition %d read failed: %d", part, ret);
		return SM_ERR_READ_ERROR;
	}

	if (rec->header.magic != CONFIG_NVM_MAGIC) {
		LOG_INF("Partition %d has no valid record (magic 0x%02X) - fresh",
			part, rec->header.magic);
		return SM_ERR_SECTION_ERASED;
	}

	if ((rec->header.version != CONFIG_NVM_FORMAT_VERSION) ||
	    (rec->header.length != sizeof(config_nvm_params_t))) {
		LOG_ERR("Partition %d header mismatch (ver=%u len=%u)",
			part, rec->header.version, rec->header.length);
		return SM_ERR_SECTION_CORRUPTED;
	}

	uint16_t crc = sm_calc_payload_crc(&rec->params);

	if (crc != rec->header.crc16) {
		LOG_ERR("Partition %d CRC mismatch (calc=0x%04X stored=0x%04X)",
			part, crc, rec->header.crc16);
		return SM_ERR_CORRUPTED;
	}

	return SM_OK;
}

/**
 * @brief Stamp header fields (magic/version/length/CRC) and write the full
 *        record to a partition. Does NOT read back or verify - this is
 *        "only perform the write operation", used by SM_WriteConfig() so
 *        it never implicitly validates anything.
 *
 * @return SM_OK on success, SM_ERR_WRITE_ERROR on an EEPROM bus failure.
 */
static sm_status_t sm_partition_write_raw(sm_partition_id_t part, config_nvm_record_t *rec)
{
	rec->header.magic = CONFIG_NVM_MAGIC;
	rec->header.version = CONFIG_NVM_FORMAT_VERSION;
	rec->header.length = sizeof(config_nvm_params_t);
	rec->header.crc16 = sm_calc_payload_crc(&rec->params);
	LOG_INF("sm_partition_write_raw");

	int ret = eeprom_write(eeprom_dev, sm_partition_offset[part], rec, sizeof(*rec));

	if (ret < 0) {
		LOG_ERR("Partition %d write failed: %d", part, ret);
		return SM_ERR_WRITE_ERROR;
	}

	return SM_OK;
}

/**
 * @brief Stamp header fields, write the full record to a partition, and
 *        read it back to confirm the write landed correctly at the byte
 *        level. Used wherever the module itself needs assurance a copy
 *        landed correctly (Slot 2 backup, Slot 1 recovery, SM_Write()) -
 *        never for a plain application-requested SM_WriteConfig().
 *
 * @return SM_OK on success, SM_ERR_WRITE_ERROR / SM_ERR_VERIFY_ERROR on fail.
 */
static sm_status_t sm_partition_store(sm_partition_id_t part, config_nvm_record_t *rec)
{
	sm_status_t ret = sm_partition_write_raw(part, rec);

	if (ret != SM_OK) {
		return ret;
	}

	config_nvm_record_t verify;
	int rc = eeprom_read(eeprom_dev, sm_partition_offset[part], &verify, sizeof(verify));

	if (rc < 0) {
		LOG_ERR("Partition %d verify-read failed: %d", part, rc);
		return SM_ERR_VERIFY_ERROR;
	}

	if (memcmp(&verify, rec, sizeof(verify)) != 0) {
		LOG_ERR("Partition %d verify mismatch", part);
		return SM_ERR_VERIFY_ERROR;
	}

	return SM_OK;
}

/**
 * @brief Read the complete, currently-valid USER configuration: Slot 1 if
 *        valid, else Slot 2 if valid and available, else a freshly-built
 *        blank record. Used by SM_Write()/SM_Clear() (per spec section 7,
 *        step 1) so they always start from the real current USER data
 *        rather than assuming Slot 1 alone is authoritative.
 *
 * @return SM_OK / SM_ERR_READ_ERROR only; content-level problems
 *         (erased/corrupted, in both slots) are absorbed into a blank
 *         in-memory record.
 */
static sm_status_t sm_load_user_or_blank(config_nvm_record_t *rec)
{
	sm_status_t ret = sm_partition_load(SM_PART_SLOT1, rec);

	if (ret == SM_OK) {
		return SM_OK;
	}
	if (ret == SM_ERR_READ_ERROR) {
		return ret; /* Genuine HW failure - do not paper over it. */
	}

	LOG_WRN("Slot 1 not usable (%d)", ret);

	if (sm_slot2_available) {
		sm_status_t ret2 = sm_partition_load(SM_PART_SLOT2, rec);

		if (ret2 == SM_OK) {
			LOG_INF("Using Slot 2 as the base for this update");
			return SM_OK;
		}
		if (ret2 == SM_ERR_READ_ERROR) {
			return ret2;
		}
	}

	LOG_WRN("No valid USER configuration in either slot; starting from a blank record");
	sm_build_blank_record(rec);
	return SM_OK;
}

/**
 * @brief Runtime feasibility check for Slot 2: does the physical EEPROM
 *        actually have room for a third full record?
 */
static bool sm_detect_slot2_available(void)
{
	size_t dev_size = eeprom_get_size(eeprom_dev);
	size_t needed = SM_PARTITION_OFFSET(SM_PART_SLOT2) + SM_RECORD_SIZE;

	if (dev_size >= needed) {
		LOG_INF("Slot 2 available (EEPROM %zu B >= %zu B needed)", dev_size, needed);
		return true;
	}

	LOG_WRN("Slot 2 not available - EEPROM %zu B < %zu B needed; "
		"running with Factory + Slot 1 only", dev_size, needed);
	return false;
}

/* ===========================================================================
 * Public API
 * =========================================================================== */

bool SM_Slot2Available(void)
{
	return sm_slot2_available;
}

/**
 * @brief Translate a storage_config_t into the internal partition used for
 *        a whole-record USER operation (always Slot 1 - Slot 2 is never a
 *        direct target/source of an application-facing call).
 */
static sm_status_t sm_partition_for(storage_config_t which, sm_partition_id_t *out)
{
	if (which == STORAGE_CONFIG_USER) {
		*out = SM_PART_SLOT1;
		return SM_OK;
	}
	if (which == STORAGE_CONFIG_FACTORY) {
		*out = SM_PART_FACTORY;
		return SM_OK;
	}
	return SM_ERR_INVALID_PARTITION;
}

sm_status_t SM_ReadConfig(storage_config_t which, config_params_t *cfg_param)
{
	if (!sm_ready) {
		return SM_ERR_NOT_INITIALIZED;
	}
	if (cfg_param == NULL) {
		return SM_ERR_INVALID_PARAM;
	}

	if (which == STORAGE_CONFIG_FACTORY) {
		config_nvm_record_t rec;
		sm_status_t ret = sm_partition_load(SM_PART_FACTORY, &rec);

		if (ret != SM_OK) {
			return ret;
		}
		sm_decode_params(&rec.params, cfg_param);
		return SM_OK;
	}

	if (which != STORAGE_CONFIG_USER) {
		return SM_ERR_INVALID_PARTITION;
	}

	config_nvm_record_t rec;
	sm_status_t ret = sm_partition_load(SM_PART_SLOT1, &rec);

	if (ret == SM_OK) {
		sm_decode_params(&rec.params, cfg_param);
		return SM_OK;
	}
	if (ret == SM_ERR_READ_ERROR) {
		return ret;
	}
	LOG_WRN("Slot 1 unusable (%d); trying Slot 2", ret);

	if (sm_slot2_available) {
		ret = sm_partition_load(SM_PART_SLOT2, &rec);
		if (ret == SM_OK) {
			sm_decode_params(&rec.params, cfg_param);
			LOG_INF("USER configuration read from Slot 2");
			return SM_OK;
		}
		if (ret == SM_ERR_READ_ERROR) {
			return ret;
		}
	}

	return SM_ERR_NO_DATA;
}

sm_status_t SM_Init(sm_validity_t *user_sts, sm_validity_t *factory_sts)
{
	if ((user_sts == NULL) || (factory_sts == NULL)) {
		return SM_ERR_INVALID_PARAM;
	}

	if (!device_is_ready(eeprom_dev)) {
		LOG_ERR("EEPROM not ready!");
		return SM_ERR_HW_FAILED;
	}

	sm_ready = true;
	sm_slot2_available = sm_detect_slot2_available();

	config_nvm_record_t slot1_rec;
	sm_status_t ret1 = sm_partition_load(SM_PART_SLOT1, &slot1_rec);

	if (ret1 == SM_ERR_READ_ERROR) {
		return ret1; /* Genuine bus failure - surface it. */
	}

	if (ret1 == SM_OK) {
		*user_sts = SM_CFG_VALID;
		/* Seed the RAM shadow used by SM_Write()/SM_ValCrc() from the
		 * record we just loaded - otherwise sm_user_ram stays at its
		 * zero-initialized state after every reboot, and the first
		 * SM_ValCrc(STORAGE_CONFIG_USER) call will compare a stale/
		 * empty RAM copy against the real EEPROM CRC and fail. */
		sm_decode_params(&slot1_rec.params, &sm_user_ram);
	} else if (sm_slot2_available) {
		LOG_WRN("Slot 1 unusable (%d); checking Slot 2", ret1);

		config_nvm_record_t slot2_rec;
		sm_status_t ret2 = sm_partition_load(SM_PART_SLOT2, &slot2_rec);

		if (ret2 == SM_ERR_READ_ERROR) {
			return ret2;
		}

		if (ret2 == SM_OK) {
			/* Recover: copy Slot 2 -> Slot 1. Slot 2 is a local
			 * copy at this point (slot2_rec) - the EEPROM copy of
			 * Slot 2 is never touched by this recovery. */
			sm_status_t store_ret = sm_partition_store(SM_PART_SLOT1, &slot2_rec);

			if (store_ret != SM_OK) {
				LOG_ERR("Slot 1 recovery from Slot 2 failed: %d", store_ret);
				*user_sts = SM_CFG_INVALID;
			} else {
				LOG_INF("Slot 1 recovered from Slot 2");
				*user_sts = SM_CFG_VALID;
				sm_decode_params(&slot2_rec.params, &sm_user_ram);
			}
		} else {
			LOG_WRN("Slot 2 also unusable (%d)", ret2);
			*user_sts = SM_CFG_INVALID;
		}
	} else {
		LOG_WRN("Slot 1 unusable (%d) and Slot 2 not available", ret1);
		*user_sts = SM_CFG_INVALID;
	}

	/* ---- FACTORY health check (spec section 2) ---- */
	config_nvm_record_t factory_rec;
	sm_status_t retf = sm_partition_load(SM_PART_FACTORY, &factory_rec);

	if (retf == SM_ERR_READ_ERROR) {
		return retf;
	}
	*factory_sts = (retf == SM_OK) ? SM_CFG_VALID : SM_CFG_INVALID;

	return SM_OK;
}

sm_status_t SM_WriteConfig(storage_config_t which, const config_params_t *cfg_param)
{
	if (!sm_ready) {
		return SM_ERR_NOT_INITIALIZED;
	}
	if (cfg_param == NULL) {
		return SM_ERR_INVALID_PARAM;
	}

	sm_partition_id_t part;
	sm_status_t ret = sm_partition_for(which, &part);

	if (ret != SM_OK) {
		return ret;
	}

	/* Take a local copy before touching EEPROM, encode it, and write it.
	 * Only the write happens here - no CRC validation, no Slot 2 backup.
	 * Call SM_ValCrc() to get either of those.
	 */
	config_params_t copy = *cfg_param;
	sm_user_ram = *cfg_param;

	config_nvm_record_t rec = { 0 };

	sm_encode_params(&copy, &rec.params);

	LOG_INF("SM_WriteConfig");
	print_config_param(copy);

	return sm_partition_write_raw(part, &rec);
}

sm_status_t SM_Write(config_param_sec_t section, const void *data)
{
	if (!sm_ready) {
		return SM_ERR_NOT_INITIALIZED;
	}
	if (section >= CONFIG_SEC_MAX) {
		return SM_ERR_INVALID_SECTION;
	}
	if (data == NULL) {
		return SM_ERR_INVALID_PARAM;
	}

	config_nvm_record_t rec;

	/* 1. Read the complete current USER configuration to avoid overwriting other sections */
	sm_status_t ret = sm_load_user_or_blank(&rec);
	if (ret != SM_OK) {
		return ret;
	}

	/* 2. Update ONLY the requested section in the internal RAM buffer AND the NVM record */
	switch (section) {
	case CONFIG_SEC_A:
		sm_user_ram.sec_a = *(const config_sec_a_t *)data;
		sm_encode_sec_a(&sm_user_ram.sec_a, &rec.params.sec_a);
		break;
	case CONFIG_SEC_B:
		sm_user_ram.sec_b = *(const config_sec_b_t *)data;
		sm_encode_sec_b(&sm_user_ram.sec_b, &rec.params.sec_b);
		break;
	case CONFIG_SEC_C:
		sm_user_ram.sec_c = *(const config_sec_c_t *)data;
		sm_encode_sec_c(&sm_user_ram.sec_c, &rec.params.sec_c);
		break;
	case CONFIG_SEC_D:
		sm_user_ram.sec_d = *(const config_sec_d_t *)data;
		sm_encode_sec_d(&sm_user_ram.sec_d, &rec.params.sec_d);
		break;
	case CONFIG_SEC_E:
		sm_user_ram.sec_e = *(const config_sec_e_t *)data;
		sm_encode_sec_e(&sm_user_ram.sec_e, &rec.params.sec_e);
		break;
	case CONFIG_SEC_F:
		sm_user_ram.sec_f = *(const config_sec_f_t *)data;
		sm_encode_sec_f(&sm_user_ram.sec_f, &rec.params.sec_f);
		break;
	case CONFIG_SEC_G:
		sm_user_ram.sec_g = *(const config_sec_g_t *)data;
		sm_encode_sec_g(&sm_user_ram.sec_g, &rec.params.sec_g);
		break;
	case CONFIG_SEC_H:
		sm_user_ram.sec_h = *(const config_sec_h_t *)data;
		sm_encode_sec_h(&sm_user_ram.sec_h, &rec.params.sec_h);
		break;
	case CONFIG_SEC_I:
		sm_user_ram.sec_i = *(const config_sec_i_t *)data;
		sm_encode_sec_i(&sm_user_ram.sec_i, &rec.params.sec_i);
		break;
	case CONFIG_SEC_J:
		sm_user_ram.sec_j = *(const config_sec_j_t *)data;
		sm_encode_sec_j(&sm_user_ram.sec_j, &rec.params.sec_j);
		break;
	case CONFIG_SEC_K:
		sm_user_ram.sec_k = *(const config_sec_k_t *)data;
		sm_encode_sec_k(&sm_user_ram.sec_k, &rec.params.sec_k);
		break;
	case CONFIG_SEC_L:
		sm_user_ram.sec_l = *(const config_sec_l_t *)data;
		sm_encode_sec_l(&sm_user_ram.sec_l, &rec.params.sec_l);
		break;
	case CONFIG_SEC_M:
		sm_user_ram.sec_m = *(const config_sec_m_t *)data;
		sm_encode_sec_m(&sm_user_ram.sec_m, &rec.params.sec_m);
		break;
	case CONFIG_SEC_N:
		sm_user_ram.sec_n = *(const config_sec_n_t *)data;
		sm_encode_sec_n(&sm_user_ram.sec_n, &rec.params.sec_n);
		break;
	case CONFIG_SEC_O:
		sm_user_ram.sec_o = *(const config_sec_o_t *)data;
		sm_encode_sec_o(&sm_user_ram.sec_o, &rec.params.sec_o);
		break;
	default:
		return SM_ERR_INVALID_SECTION;
	}

	/* 3. Write to Slot 1. This internally updates the NVM header CRC for structural validity. */
	/* No RAM vs EEPROM validation is performed here. */
	return sm_partition_store(SM_PART_SLOT1, &rec);
}
sm_status_t SM_ValCrc(storage_config_t which)
{
	if (!sm_ready) {
		return SM_ERR_NOT_INITIALIZED;
	}

	sm_partition_id_t part;
	sm_status_t ret = sm_partition_for(which, &part);

	if (ret != SM_OK) {
		return ret;
	}

	/* Load the configuration from EEPROM (automatically checks if structurally intact) */
	config_nvm_record_t rec;
	ret = sm_partition_load(part, &rec);
	if (ret != SM_OK) {
		return ret;
	}

	/* Explicit Validation Logic for USER config */
	if (which == STORAGE_CONFIG_USER) {
		/* Calculate what the CRC *should* be based on our RAM buffer */
		config_nvm_record_t ram_rec = { 0 };
		sm_encode_params(&sm_user_ram, &ram_rec.params);
		uint16_t ram_crc = sm_calc_payload_crc(&ram_rec.params);

		/* Compare RAM CRC to the EEPROM CRC */
		if (ram_crc != rec.header.crc16) {
			LOG_ERR("Explicit CRC Validation failed: RAM (0x%04X) != EEPROM (0x%04X)", 
					ram_crc, rec.header.crc16);
			return SM_ERR_VERIFY_ERROR;
		}

		/* Validation passed -> Backup to Slot 2 */
		if (sm_slot2_available) {
			sm_status_t backup_ret = sm_partition_store(SM_PART_SLOT2, &rec);
			if (backup_ret != SM_OK) {
				LOG_ERR("Slot 2 backup failed during explicit CRC validation: %d", backup_ret);
			} else {
				LOG_INF("Explicit CRC Validation passed. Slot 2 updated.");
			}
		}
	}

	return SM_OK;
}
sm_status_t SM_Read(storage_config_t which, config_param_sec_t section, void *data)
{
	if (!sm_ready) {
		return SM_ERR_NOT_INITIALIZED;
	}
	if (section >= CONFIG_SEC_MAX) {
		return SM_ERR_INVALID_SECTION;
	}
	if (data == NULL) {
		return SM_ERR_INVALID_PARAM;
	}

	config_nvm_record_t rec;
	sm_status_t ret;

	if (which == STORAGE_CONFIG_FACTORY) {
		ret = sm_partition_load(SM_PART_FACTORY, &rec);
		if (ret != SM_OK) {
			return ret;
		}
	} else if (which == STORAGE_CONFIG_USER) {
		ret = sm_partition_load(SM_PART_SLOT1, &rec);
		if (ret == SM_ERR_READ_ERROR) {
			return ret;
		}
		if (ret != SM_OK) {
			LOG_WRN("Slot 1 unusable (%d); trying Slot 2", ret);
			if (!sm_slot2_available) {
				return ret;
			}
			ret = sm_partition_load(SM_PART_SLOT2, &rec);
			if (ret != SM_OK) {
				return ret;
			}
		}
	} else {
		return SM_ERR_INVALID_PARTITION;
	}

	const uint8_t *is_cfg = (const uint8_t *)&rec.params + sm_sections[section].nvm_offset;

	if (*is_cfg == CONFIG_SECTION_CLEARED) {
		return SM_ERR_SECTION_ERASED;
	}
	if (*is_cfg != CONFIG_SECTION_CONFIGURED) {
		LOG_ERR("Section %d has an invalid status byte 0x%02X", section, *is_cfg);
		return SM_ERR_SECTION_CORRUPTED;
	}

	switch (section) {
	case CONFIG_SEC_A: sm_decode_sec_a(&rec.params.sec_a, (config_sec_a_t *)data); break;
	case CONFIG_SEC_B: sm_decode_sec_b(&rec.params.sec_b, (config_sec_b_t *)data); break;
	case CONFIG_SEC_C: sm_decode_sec_c(&rec.params.sec_c, (config_sec_c_t *)data); break;
	case CONFIG_SEC_D: sm_decode_sec_d(&rec.params.sec_d, (config_sec_d_t *)data); break;
	case CONFIG_SEC_E: sm_decode_sec_e(&rec.params.sec_e, (config_sec_e_t *)data); break;
	case CONFIG_SEC_F: sm_decode_sec_f(&rec.params.sec_f, (config_sec_f_t *)data); break;
	case CONFIG_SEC_G: sm_decode_sec_g(&rec.params.sec_g, (config_sec_g_t *)data); break;
	case CONFIG_SEC_H: sm_decode_sec_h(&rec.params.sec_h, (config_sec_h_t *)data); break;
	case CONFIG_SEC_I: sm_decode_sec_i(&rec.params.sec_i, (config_sec_i_t *)data); break;
	case CONFIG_SEC_J: sm_decode_sec_j(&rec.params.sec_j, (config_sec_j_t *)data); break;
	case CONFIG_SEC_K: sm_decode_sec_k(&rec.params.sec_k, (config_sec_k_t *)data); break;
	case CONFIG_SEC_L: sm_decode_sec_l(&rec.params.sec_l, (config_sec_l_t *)data); break;
	case CONFIG_SEC_M: sm_decode_sec_m(&rec.params.sec_m, (config_sec_m_t *)data); break;
	case CONFIG_SEC_N: sm_decode_sec_n(&rec.params.sec_n, (config_sec_n_t *)data); break;
	case CONFIG_SEC_O: sm_decode_sec_o(&rec.params.sec_o, (config_sec_o_t *)data); break;
	default: return SM_ERR_INVALID_SECTION;
	}

	return SM_OK;
}

sm_status_t SM_Clear(config_param_sec_t section)
{
	if (!sm_ready) {
		return SM_ERR_NOT_INITIALIZED;
	}
	if (section >= CONFIG_SEC_MAX) {
		return SM_ERR_INVALID_SECTION;
	}

	config_nvm_record_t rec;
	sm_status_t ret = sm_load_user_or_blank(&rec);

	if (ret != SM_OK) {
		return ret;
	}

	const sm_section_range_t *r = &sm_sections[section];
	uint8_t *base = (uint8_t *)&rec.params + r->nvm_offset;

	memset(base + 1U, 0, r->nvm_size - 1U);
	base[0] = CONFIG_SECTION_CLEARED;

	return sm_partition_store(SM_PART_SLOT1, &rec);
}