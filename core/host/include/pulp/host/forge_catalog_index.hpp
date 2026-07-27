#pragma once

// Canonical index of Pulp's Forge-facing CustomNodeType catalog packs.
//
// Keep the includes and kHeaderNames entries in lockstep. The catalog-index
// contract test compares this list with every forge_*_catalog.hpp leaf header
// in the source include tree, so adding or removing a pack cannot leave a
// downstream consumer with an incomplete, stale header list.

#include <pulp/host/forge_analog_vcf_catalog.hpp>
#include <pulp/host/forge_character_delay_catalog.hpp>
#include <pulp/host/forge_fdn_reverb_catalog.hpp>
#include <pulp/host/forge_lofi_catalog.hpp>
#include <pulp/host/forge_modulation_catalog.hpp>

#include <array>
#include <string_view>

namespace pulp::host::forge_catalog {

inline constexpr std::array<std::string_view, 5> kHeaderNames{{
    "forge_analog_vcf_catalog.hpp",
    "forge_character_delay_catalog.hpp",
    "forge_fdn_reverb_catalog.hpp",
    "forge_lofi_catalog.hpp",
    "forge_modulation_catalog.hpp",
}};

} // namespace pulp::host::forge_catalog
