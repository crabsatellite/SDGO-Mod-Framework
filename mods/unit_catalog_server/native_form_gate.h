#pragma once
#include "human_build_capture.h"
#include "sdgo/unit_form_preflight.h"
#include "sdgo/unit_form_gate_state.h"

namespace unit_catalog_server {
bool InstallFormGate(const SdgoApi*,SdgoMod*,std::shared_ptr<const sdgo::unit_forms::GraphBindings>,
                     const std::map<std::uint32_t,sdgo::unit_forms::BattleDefinition>*,
                     const HumanBuildCapture*,std::string&);
SdgoUnitFormGateStateV1 ReadFormGateState();
SdgoUnitFormRebindStateV1 ReadFormRebindState();
} // namespace unit_catalog_server
