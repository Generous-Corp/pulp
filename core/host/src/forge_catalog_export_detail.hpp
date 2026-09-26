#pragma once

// Private seam between forge_catalog_export.cpp and the per-family translation
// units that construct its nodes.
//
// Every catalog pack's node factory is header-inline, so a translation unit
// that constructs a realization also instantiates that node's DSP. One file
// constructing every pack therefore compiles the whole Forge DSP library in a
// single object (measured at 14.5 s, 10.6 s of it backend, the slowest
// translation unit in the tree). Each family below lives in its own .cpp that
// includes only its packs, so the families compile in parallel and an edit to
// one pack's export rebuilds one family. forge_catalog_export_nodes() appends
// them in the order the catalog has always been exported in.

#include <pulp/host/forge_catalog_export.hpp>

#include <string_view>
#include <utility>
#include <vector>

namespace pulp::host::forge_catalog_export_detail {

using Nodes = std::vector<ForgeCatalogExportNode>;

inline ForgeCatalogExportRealization realization(std::string_view mode, CustomNodeType node) {
    return {mode, std::move(node.type_id), std::move(node.baked_params)};
}

inline void add(Nodes& nodes, ForgeNodeDescriptor descriptor,
                std::vector<ForgeCatalogExportRealization> realizations) {
    nodes.push_back({std::move(descriptor), std::move(realizations)});
}

/// Append fuzz, saturator, analog VCF, character delay and distortion.
void append_effects(Nodes& nodes);
/// Append Eurorack utilities and the drum engines.
void append_eurorack_drums(Nodes& nodes);
/// Append the dynamics processors.
void append_dynamics(Nodes& nodes);
/// Append the modulation effects.
void append_modulation(Nodes& nodes);
/// Append FDN reverb, the lo-fi pack and the modulation sources.
void append_lofi(Nodes& nodes);
/// Append pitch, sequencing, multiband and sidechain.
void append_pitch_sequencing(Nodes& nodes);
/// Append space, synthesis, tape and wavetable.
void append_space_synthesis(Nodes& nodes);

} // namespace pulp::host::forge_catalog_export_detail
