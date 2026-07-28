#pragma once

// Canonical index of Pulp's Forge-facing CustomNodeType catalog packs.
//
// Keep the includes and kHeaderNames entries in lockstep. The catalog-index
// contract test compares this list with every forge_*_catalog.hpp leaf header
// in the source include tree, so adding or removing a pack cannot leave a
// downstream consumer with an incomplete, stale header list.
//
// Leaf headers under detail/ are deliberately absent: they are implementation
// splits of a pack that is already listed here (today
// detail/forge_effect_modulation_extended_catalog.hpp backs
// forge_effect_modulation_catalog.hpp), not packs a Forge agent can consume.

#include <pulp/host/forge_analog_vcf_catalog.hpp>
#include <pulp/host/forge_character_delay_catalog.hpp>
#include <pulp/host/forge_distortion_catalog.hpp>
#include <pulp/host/forge_drum_catalog.hpp>
#include <pulp/host/forge_dynamics_catalog.hpp>
#include <pulp/host/forge_effect_modulation_catalog.hpp>
#include <pulp/host/forge_eurorack_utility_catalog.hpp>
#include <pulp/host/forge_fdn_reverb_catalog.hpp>
#include <pulp/host/forge_fuzz_catalog.hpp>
#include <pulp/host/forge_lofi_catalog.hpp>
#include <pulp/host/forge_modulation_catalog.hpp>
#include <pulp/host/forge_pitch_catalog.hpp>
#include <pulp/host/forge_saturator_catalog.hpp>
#include <pulp/host/forge_sequencing_catalog.hpp>
#include <pulp/host/forge_space_catalog.hpp>
#include <pulp/host/forge_synthesis_catalog.hpp>
#include <pulp/host/forge_tape_catalog.hpp>

#include <array>
#include <string_view>

namespace pulp::host::forge_catalog {

inline constexpr std::array<std::string_view, 17> kHeaderNames{{
    "forge_analog_vcf_catalog.hpp",
    "forge_character_delay_catalog.hpp",
    "forge_distortion_catalog.hpp",
    "forge_drum_catalog.hpp",
    "forge_dynamics_catalog.hpp",
    "forge_effect_modulation_catalog.hpp",
    "forge_eurorack_utility_catalog.hpp",
    "forge_fdn_reverb_catalog.hpp",
    "forge_fuzz_catalog.hpp",
    "forge_lofi_catalog.hpp",
    "forge_modulation_catalog.hpp",
    "forge_pitch_catalog.hpp",
    "forge_saturator_catalog.hpp",
    "forge_sequencing_catalog.hpp",
    "forge_space_catalog.hpp",
    "forge_synthesis_catalog.hpp",
    "forge_tape_catalog.hpp",
}};

} // namespace pulp::host::forge_catalog
