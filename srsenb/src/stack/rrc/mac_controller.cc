/*
 * Copyright 2013-2020 Software Radio Systems Limited
 *
 * This file is part of srsLTE.
 *
 * srsLTE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsLTE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include "srsenb/hdr/stack/rrc/mac_controller.h"
#include "srslte/asn1/rrc_asn1_utils.h"
#include "srslte/interfaces/sched_interface.h"

namespace srsenb {

using namespace asn1::rrc;

rrc::ue::mac_controller::mac_controller(rrc::ue* rrc_ue_, const sched_interface::ue_cfg_t& sched_ue_cfg) :
  log_h("RRC"),
  rrc_ue(rrc_ue_),
  rrc_cfg(&rrc_ue_->parent->cfg),
  mac(rrc_ue_->parent->mac),
  current_sched_ue_cfg(sched_ue_cfg)
{
  if (current_sched_ue_cfg.supported_cc_list.empty() or not current_sched_ue_cfg.supported_cc_list[0].active) {
    log_h->warning("No PCell set. Picking enb_cc_idx=0 as PCell\n");
    current_sched_ue_cfg.supported_cc_list.resize(1);
    current_sched_ue_cfg.supported_cc_list[0].active     = true;
    current_sched_ue_cfg.supported_cc_list[0].enb_cc_idx = 0;
  }
}

int rrc::ue::mac_controller::handle_con_setup(const asn1::rrc::rrc_conn_setup_r8_ies_s& conn_setup)
{
  return apply_basic_conn_cfg(conn_setup.rr_cfg_ded);
}

int rrc::ue::mac_controller::handle_con_reest(const asn1::rrc::rrc_conn_reest_r8_ies_s& conn_reest)
{
  return apply_basic_conn_cfg(conn_reest.rr_cfg_ded);
}

//! Called when ConnectionSetup or Reestablishment is rejected (e.g. no MME connection)
void rrc::ue::mac_controller::handle_con_reject()
{
  if (not crnti_set) {
    crnti_set = true;
    // Need to schedule ConRes CE for UE to see the Reject message
    mac->ue_set_crnti(rrc_ue->rnti, rrc_ue->rnti, &current_sched_ue_cfg);
  }
}

//! Called in case of Handover
int rrc::ue::mac_controller::handle_crnti_ce(uint32_t temp_crnti)
{
  uint32_t target_enb_cc_idx = rrc_ue->cell_ded_list.get_ue_cc_idx(UE_PCELL_CC_IDX)->cell_common->enb_cc_idx;

  // Change PCell in MAC/Scheduler
  current_sched_ue_cfg.supported_cc_list.resize(1);
  current_sched_ue_cfg.supported_cc_list[0].active     = true;
  current_sched_ue_cfg.supported_cc_list[0].enb_cc_idx = target_enb_cc_idx;
  return mac->ue_set_crnti(temp_crnti, rrc_ue->rnti, &current_sched_ue_cfg);
}

int rrc::ue::mac_controller::apply_basic_conn_cfg(const asn1::rrc::rr_cfg_ded_s& rr_cfg)
{
  const auto* pcell = rrc_ue->cell_ded_list.get_ue_cc_idx(UE_PCELL_CC_IDX);

  // Set static config params
  current_sched_ue_cfg.maxharq_tx       = rrc_cfg->mac_cnfg.ul_sch_cfg.max_harq_tx.to_number();
  current_sched_ue_cfg.continuous_pusch = false;

  // Only PCell active at this point
  current_sched_ue_cfg.supported_cc_list.resize(1);
  current_sched_ue_cfg.supported_cc_list[0].active     = true;
  current_sched_ue_cfg.supported_cc_list[0].enb_cc_idx = pcell->cell_common->enb_cc_idx;

  // Only SRB0 and SRB1 active in the Scheduler at this point
  current_sched_ue_cfg.ue_bearers              = {};
  current_sched_ue_cfg.ue_bearers[0].direction = srsenb::sched_interface::ue_bearer_cfg_t::BOTH;
  current_sched_ue_cfg.ue_bearers[1].direction = srsenb::sched_interface::ue_bearer_cfg_t::BOTH;

  // Set basic antenna configuration
  current_sched_ue_cfg.dl_cfg.tm = SRSLTE_TM1;

  // Apply common PhyConfig updates (e.g. SR/CQI resources, antenna cfg)
  if (rr_cfg.phys_cfg_ded_present) {
    apply_phy_cfg_updates_common(rr_cfg.phys_cfg_ded);
  }

  // Other PUCCH params
  const asn1::rrc::sib_type2_s& sib2               = pcell->cell_common->sib2;
  current_sched_ue_cfg.pucch_cfg.delta_pucch_shift = sib2.rr_cfg_common.pucch_cfg_common.delta_pucch_shift.to_number();
  current_sched_ue_cfg.pucch_cfg.N_cs              = sib2.rr_cfg_common.pucch_cfg_common.ncs_an;
  current_sched_ue_cfg.pucch_cfg.n_rb_2            = sib2.rr_cfg_common.pucch_cfg_common.nrb_cqi;
  current_sched_ue_cfg.pucch_cfg.N_pucch_1         = sib2.rr_cfg_common.pucch_cfg_common.n1_pucch_an;

  // Configure MAC
  // In case of RRC Connection Setup/Reest message (Msg4), we need to resolve the contention by sending a ConRes CE
  mac->phy_config_enabled(rrc_ue->rnti, false);
  crnti_set = true;
  return mac->ue_set_crnti(rrc_ue->rnti, rrc_ue->rnti, &current_sched_ue_cfg);
}

void rrc::ue::mac_controller::handle_con_setup_complete()
{
  // Acknowledge Dedicated Configuration
  mac->phy_config_enabled(rrc_ue->rnti, true);
}

void rrc::ue::mac_controller::handle_con_reest_complete()
{
  // Acknowledge Dedicated Configuration
  mac->phy_config_enabled(rrc_ue->rnti, true);
}

void rrc::ue::mac_controller::handle_con_reconf(const asn1::rrc::rrc_conn_recfg_r8_ies_s& conn_recfg)
{
  if (conn_recfg.rr_cfg_ded_present and conn_recfg.rr_cfg_ded.phys_cfg_ded_present) {
    apply_phy_cfg_updates_common(conn_recfg.rr_cfg_ded.phys_cfg_ded);
  }

  if (conn_recfg.mob_ctrl_info_present) {
    // Handover Command
    handle_con_reconf_with_mobility();
  }

  // Apply changes to MAC scheduler
  mac->ue_cfg(rrc_ue->rnti, &current_sched_ue_cfg);
  mac->phy_config_enabled(rrc_ue->rnti, false);
}

void rrc::ue::mac_controller::handle_con_reconf_complete()
{
  // Setup all secondary carriers
  auto& list = current_sched_ue_cfg.supported_cc_list;
  for (const auto& ue_cell : rrc_ue->cell_ded_list) {
    uint32_t ue_cc_idx = ue_cell.ue_cc_idx;

    if (ue_cc_idx >= list.size()) {
      list.resize(ue_cc_idx + 1);
    }
    list[ue_cc_idx].active     = true;
    list[ue_cc_idx].enb_cc_idx = ue_cell.cell_common->enb_cc_idx;
  }

  // Setup all bearers
  apply_current_bearers_cfg();

  // Apply SCell+Bearer changes to MAC
  mac->ue_cfg(rrc_ue->rnti, &current_sched_ue_cfg);

  // Acknowledge Dedicated Configuration
  mac->phy_config_enabled(rrc_ue->rnti, true);
}

void rrc::ue::mac_controller::handle_con_reconf_with_mobility()
{
  // Temporarily freeze DL+UL grants for all DRBs.
  for (const drb_to_add_mod_s& drb : rrc_ue->bearer_list.get_established_drbs()) {
    current_sched_ue_cfg.ue_bearers[drb.lc_ch_id].direction = sched_interface::ue_bearer_cfg_t::IDLE;
  }

  // Temporarily freeze UL grants for SRBs. DL is required to send HO Cmd
  for (uint32_t srb_id = 0; srb_id < 3; ++srb_id) {
    current_sched_ue_cfg.ue_bearers[srb_id].direction = sched_interface::ue_bearer_cfg_t::DL;
  }
}

void rrc::ue::mac_controller::apply_current_bearers_cfg()
{
  srsenb::sched_interface::ue_bearer_cfg_t bearer_cfg = {};
  current_sched_ue_cfg.ue_bearers                     = {};
  current_sched_ue_cfg.ue_bearers[0].direction        = sched_interface::ue_bearer_cfg_t::BOTH;

  // Configure SRBs
  const srb_to_add_mod_list_l& srbs = rrc_ue->bearer_list.get_established_srbs();
  for (const srb_to_add_mod_s& srb : srbs) {
    bearer_cfg.direction                        = srsenb::sched_interface::ue_bearer_cfg_t::BOTH;
    bearer_cfg.group                            = 0;
    current_sched_ue_cfg.ue_bearers[srb.srb_id] = bearer_cfg;
  }

  // Configure DRBs
  const drb_to_add_mod_list_l& drbs = rrc_ue->bearer_list.get_established_drbs();
  for (const drb_to_add_mod_s& drb : drbs) {
    bearer_cfg.direction                          = sched_interface::ue_bearer_cfg_t::BOTH;
    bearer_cfg.group                              = drb.lc_ch_cfg.ul_specific_params.lc_ch_group;
    current_sched_ue_cfg.ue_bearers[drb.lc_ch_id] = bearer_cfg;
  }
}

void rrc::ue::mac_controller::apply_phy_cfg_updates_common(const asn1::rrc::phys_cfg_ded_s& phy_cfg)
{
  // Apply SR config
  if (phy_cfg.sched_request_cfg_present) {
    current_sched_ue_cfg.pucch_cfg.sr_configured = true;
    current_sched_ue_cfg.pucch_cfg.I_sr          = phy_cfg.sched_request_cfg.setup().sr_cfg_idx;
    current_sched_ue_cfg.pucch_cfg.n_pucch_sr    = phy_cfg.sched_request_cfg.setup().sr_pucch_res_idx;
  }

  // Apply CQI config
  if (phy_cfg.cqi_report_cfg_present) {
    if (phy_cfg.cqi_report_cfg.cqi_report_periodic_present) {
      auto& cqi_cfg                                              = phy_cfg.cqi_report_cfg.cqi_report_periodic.setup();
      current_sched_ue_cfg.dl_cfg.cqi_report.pmi_idx             = cqi_cfg.cqi_pmi_cfg_idx;
      current_sched_ue_cfg.pucch_cfg.n_pucch                     = cqi_cfg.cqi_pucch_res_idx;
      current_sched_ue_cfg.dl_cfg.cqi_report.periodic_configured = true;
    } else if (phy_cfg.cqi_report_cfg.cqi_report_mode_aperiodic_present) {
      current_sched_ue_cfg.aperiodic_cqi_period                   = rrc_cfg->cqi_cfg.period;
      current_sched_ue_cfg.dl_cfg.cqi_report.aperiodic_configured = true;
    }
  }

  // Configure 256QAM
  if (phy_cfg.cqi_report_cfg_pcell_v1250.is_present() and
      phy_cfg.cqi_report_cfg_pcell_v1250->alt_cqi_table_r12_present) {
    current_sched_ue_cfg.use_tbs_index_alt = true;
  }

  // Apply Antenna Configuration
  if (phy_cfg.ant_info_present) {
    if (phy_cfg.ant_info.type().value == phys_cfg_ded_s::ant_info_c_::types_opts::explicit_value) {
      current_sched_ue_cfg.dl_ant_info = srslte::make_ant_info_ded(phy_cfg.ant_info.explicit_value());
    } else {
      log_h->warning("No antenna configuration provided\n");
      current_sched_ue_cfg.dl_cfg.tm           = SRSLTE_TM1;
      current_sched_ue_cfg.dl_ant_info.tx_mode = sched_interface::ant_info_ded_t::tx_mode_t::tm1;
    }
  }
}

} // namespace srsenb