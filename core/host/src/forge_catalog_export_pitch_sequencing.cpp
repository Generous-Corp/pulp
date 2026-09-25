#include "forge_catalog_export_detail.hpp"

#include <pulp/host/forge_multiband_catalog.hpp>
#include <pulp/host/forge_pitch_catalog.hpp>
#include <pulp/host/forge_sequencing_catalog.hpp>
#include <pulp/host/forge_sidechain_catalog.hpp>

namespace pulp::host::forge_catalog_export_detail {

void append_pitch_sequencing(Nodes& nodes) {
    add(nodes, pitch::whammy::descriptor(),
        {realization("default", pitch::whammy::make_whammy_node())});
    add(nodes, pitch::harmony::descriptor(),
        {realization("default", pitch::harmony::make_harmony_engine_node())});

    add(nodes, sequencing::stage_seq::descriptor(),
        {realization("default", sequencing::stage_seq::make_stage_seq_node())});
    add(nodes, sequencing::cartesian::descriptor(),
        {
            realization("cartesian", sequencing::cartesian::make_cartesian_walk_node(
                                         sequencing::cartesian::default_grid(), false)),
            realization("row_major", sequencing::cartesian::make_cartesian_walk_node(
                                         sequencing::cartesian::default_grid(), true)),
        });
    add(nodes, sequencing::rungler::descriptor(),
        {realization("default", sequencing::rungler::make_rungler_node())});
    add(nodes, sequencing::quantize::descriptor(),
        {realization("default", sequencing::quantize::make_quantize_scale_node())});
    add(nodes, sequencing::gate_logic::descriptor(),
        {realization("default", sequencing::gate_logic::make_gate_logic_node())});
    add(nodes, sequencing::prob_gate::descriptor(),
        {realization("default", sequencing::prob_gate::make_prob_gate_node())});

    add(nodes, multiband::descriptor(),
        {realization("default", multiband::make_multiband_compressor_node())});
    add(nodes, sidechain::descriptor(),
        {realization("default", sidechain::make_sidechain_compressor_node())});
}

} // namespace pulp::host::forge_catalog_export_detail
