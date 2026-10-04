/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Configuration Module - field range validation.
 *
 * Every numeric field is checked against the min/max constants from
 * config_params.h before a Write is accepted.  A single out-of-range
 * field rejects the entire packet (no partial writes).
 *
 * String fields (Section M SSID / password, RDN) are validated for
 * length; their content is opaque to this layer.
 *
 * Section N byte-array fields (SYW, AES) have no meaningful per-byte
 * range; they are accepted as-is once the parser confirms the correct
 * byte count.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/logging/log.h>
#include <common_config/config_params.h>
#include <config_module/config_module.h>
#include "config_module_internal.h"

LOG_MODULE_DECLARE(config_module, CONFIG_CONFIG_MODULE_LOG_LEVEL);

/* Helper macro: log and return false if field is out of range */
#define CHECK(field, val, lo, hi)						\
	do {									\
		if ((val) < (lo) || (val) > (hi)) {				\
			LOG_WRN("Validation: " #field				\
				" %lld out of range [%lld, %lld]",		\
				(long long)(val), (long long)(lo),		\
				(long long)(hi));				\
			return false;						\
		}								\
	} while (0)

static bool validate_a(const config_params_t *p)
{
	CHECK(GFF, p->sec_a.gff, CFG_A_GFF_MIN, CFG_A_GFF_MAX);
	CHECK(LWS, p->sec_a.lws, CFG_A_LWS_MIN, CFG_A_LWS_MAX);
	CHECK(PIB, p->sec_a.pib, CFG_A_PIB_MIN, CFG_A_PIB_MAX);
	return true;
}

static bool validate_b(const config_params_t *p)
{
	CHECK(PWS, p->sec_b.pws, CFG_B_PWS_MIN, CFG_B_PWS_MAX);
	CHECK(PWC, p->sec_b.pwc, CFG_B_PWC_MIN, CFG_B_PWC_MAX);
	CHECK(WGO, p->sec_b.wgo, CFG_B_WGO_MIN, CFG_B_WGO_MAX);
	return true;
}

static bool validate_c(const config_params_t *p)
{
	CHECK(GDP, p->sec_c.gdp, CFG_C_GDP_MIN, CFG_C_GDP_MAX);
	CHECK(PLD, p->sec_c.pld, CFG_C_PLD_MIN, CFG_C_PLD_MAX);
	CHECK(EFC, p->sec_c.efc, CFG_C_EFC_MIN, CFG_C_EFC_MAX);
	return true;
}

static bool validate_d(const config_params_t *p)
{
	CHECK(SWL, p->sec_d.swl, CFG_D_SWL_MIN, CFG_D_SWL_MAX);
	CHECK(SLD, p->sec_d.sld, CFG_D_SLD_MIN, CFG_D_SLD_MAX);
	CHECK(LSP, p->sec_d.lsp, CFG_D_LSP_MIN, CFG_D_LSP_MAX);
	return true;
}

static bool validate_e(const config_params_t *p)
{
	CHECK(AUR, p->sec_e.aur, CFG_E_RPM_MIN, CFG_E_RPM_MAX);
	CHECK(AUT, p->sec_e.aut, CFG_E_TIME_MIN, CFG_E_TIME_MAX);
	CHECK(ALR, p->sec_e.alr, CFG_E_RPM_MIN, CFG_E_RPM_MAX);
	CHECK(ALT, p->sec_e.alt, CFG_E_TIME_MIN, CFG_E_TIME_MAX);
	CHECK(MAS, p->sec_e.mas, CFG_E_RPM_MIN, CFG_E_RPM_MAX);
	CHECK(MAT, p->sec_e.mat, CFG_E_TIME_MIN, CFG_E_TIME_MAX);
	CHECK(MUS, p->sec_e.mus, CFG_E_RPM_MIN, CFG_E_RPM_MAX);
	CHECK(MUT, p->sec_e.mut, CFG_E_TIME_MIN, CFG_E_TIME_MAX);
	CHECK(DUD, p->sec_e.dud, CFG_E_RPM_MIN, CFG_E_RPM_MAX);
	CHECK(DUT, p->sec_e.dut, CFG_E_TIME_MIN, CFG_E_TIME_MAX);
	CHECK(DLD, p->sec_e.dld, CFG_E_RPM_MIN, CFG_E_RPM_MAX);
	CHECK(DLT, p->sec_e.dlt, CFG_E_TIME_MIN, CFG_E_TIME_MAX);
	CHECK(LAS, p->sec_e.las, CFG_E_RPM_MIN, CFG_E_RPM_MAX);
	CHECK(LAT, p->sec_e.lat, CFG_E_TIME_MIN, CFG_E_TIME_MAX);
	CHECK(LDS, p->sec_e.lds, CFG_E_RPM_MIN, CFG_E_RPM_MAX);
	CHECK(LDT, p->sec_e.ldt, CFG_E_TIME_MIN, CFG_E_TIME_MAX);
	return true;
}

static bool validate_f(const config_params_t *p)
{
	CHECK(SSD, p->sec_f.ssd, CFG_F_SSD_MIN, CFG_F_SSD_MAX);
	CHECK(RNW, p->sec_f.rnw, CFG_F_RNW_MIN, CFG_F_RNW_MAX);
	CHECK(RNP, p->sec_f.rnp, CFG_F_RNP_MIN, CFG_F_RNP_MAX);
	CHECK(SDP, p->sec_f.sdp, CFG_F_SDP_MIN, CFG_F_SDP_MAX);
	return true;
}

static bool validate_g(const config_params_t *p)
{
	CHECK(ASR, p->sec_g.asr, CFG_G_ASR_MIN, CFG_G_ASR_MAX);
	CHECK(ATL, p->sec_g.atl, CFG_G_ATL_MIN, CFG_G_ATL_MAX);
	return true;
}

static bool validate_h(const config_params_t *p)
{
	CHECK(LFT, p->sec_h.lft, CFG_H_LFT_MIN, CFG_H_LFT_MAX);
	return true;
}

static bool validate_i(const config_params_t *p)
{
	CHECK(API, p->sec_i.api, CFG_I_API_MIN, CFG_I_API_MAX);
	CHECK(PKD, p->sec_i.pkd, CFG_I_PKD_MIN, CFG_I_PKD_MAX);
	CHECK(MSI, p->sec_i.msi, CFG_I_MSI_MIN, CFG_I_MSI_MAX);
	return true;
}

static bool validate_j(const config_params_t *p)
{
	CHECK(FSO, p->sec_j.fso, CFG_J_FSO_MIN, CFG_J_FSO_MAX);
	CHECK(RSO, p->sec_j.rso, CFG_J_RSO_MIN, CFG_J_RSO_MAX);
	CHECK(FPO, p->sec_j.fpo, CFG_J_FPO_MIN, CFG_J_FPO_MAX);
	CHECK(RPO, p->sec_j.rpo, CFG_J_RPO_MIN, CFG_J_RPO_MAX);
	CHECK(PTC, p->sec_j.ptc, CFG_J_PTC_MIN, CFG_J_PTC_MAX);
	return true;
}

static bool validate_k(const config_params_t *p)
{
	CHECK(WHD, p->sec_k.whd, CFG_K_WHD_MIN, CFG_K_WHD_MAX);
	CHECK(GBR, p->sec_k.gbr, CFG_K_GBR_MIN, CFG_K_GBR_MAX);
	return true;
}

static bool validate_l(const config_params_t *p)
{
	/* ssid_size and pwd_size are auto-filled by the parser from
	 * strlen(ssid/pwd); validate they landed in the legal range. */
	if (p->sec_l.ssz == 0 ||
	    p->sec_l.ssz > CFG_L_SSID_MAX_LEN) {
		LOG_WRN("Validation: SSID length %u out of range [1, %u]",
			p->sec_l.ssz, CFG_L_SSID_MAX_LEN);
		return false;
	}
	if (p->sec_l.psz == 0 ||
	    p->sec_l.psz > CFG_L_PWD_MAX_LEN) {
		LOG_WRN("Validation: Password length %u out of range [1, %u]",
			p->sec_l.psz, CFG_L_PWD_MAX_LEN);
		return false;
	}
	CHECK(WAT, p->sec_l.wat, CFG_L_WAT_MIN, CFG_L_WAT_MAX);
	CHECK(WST, p->sec_l.wst, CFG_L_WST_MIN, CFG_L_WST_MAX);
	CHECK(RCP, p->sec_l.rcp, CFG_L_RCP_MIN, CFG_L_RCP_MAX);
	return true;
}

static bool validate_m(const config_params_t *p)
{
	CHECK(RAD, p->sec_m.rad, CFG_M_RAD_MIN, CFG_M_RAD_MAX);
	CHECK(SYW, p->sec_m.syw, CFG_M_SYW_MIN, CFG_M_SYW_MAX);
	CHECK(CID, p->sec_m.cid, CFG_M_CID_MIN, CFG_M_CID_MAX);
	CHECK(SND, p->sec_m.snd, CFG_M_SND_MIN, CFG_M_SND_MAX);
	CHECK(PMT, p->sec_m.pmt, CFG_M_PMT_MIN, CFG_M_PMT_MAX);
	return true;
}


static bool validate_n(const config_params_t *p)
{
	CHECK(COM, p->sec_n.com, CFG_N_COM_MIN, CFG_N_COM_MAX);
	CHECK(MPH, p->sec_n.mph, CFG_N_MPH_MIN, CFG_N_MPH_MAX);
	CHECK(PLM, p->sec_n.plm, CFG_N_PLM_MIN, CFG_N_PLM_MAX);
	CHECK(MNO, p->sec_n.mno, CFG_N_MNO_MIN, CFG_N_MNO_MAX);
	CHECK(MPL, p->sec_n.mpl, CFG_N_MPL_MIN, CFG_N_MPL_MAX);
	return true;
}


static bool validate_o(const config_params_t *p)
{
	CHECK(STM_DD,   p->sec_o.stm.tm_mday,   1U,  31U);
	CHECK(STM_MM,   p->sec_o.stm.tm_mon,    0U,  11U);
	CHECK(STM_YY,   p->sec_o.stm.tm_year,   70U, 200U); /* Minimum 1970 */
	CHECK(STM_HR,   p->sec_o.stm.tm_hour,   0U,  23U);
	CHECK(STM_MN,   p->sec_o.stm.tm_min,    0U,  59U);
	CHECK(STM_SC,   p->sec_o.stm.tm_sec,    0U,  59U);

	return true;
}

typedef bool (*validate_fn_t)(const config_params_t *);

static const validate_fn_t validate_table[] = {
	validate_a, validate_b, validate_c,
	validate_d, validate_e, validate_f,
	validate_g, validate_h, validate_i,
	validate_j, validate_k, validate_l,
	validate_m, validate_n, validate_o,
};

bool cfg_validate_section(char sec_id, const config_params_t *params)
{
	if (sec_id < 'A' || sec_id > 'O') {
		return false;
	}
	return validate_table[sec_id - 'A'](params);
}