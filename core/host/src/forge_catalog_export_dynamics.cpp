#include "forge_catalog_export_detail.hpp"

#include <pulp/host/forge_dynamics_catalog.hpp>

namespace pulp::host::forge_catalog_export_detail {

void append_dynamics(Nodes& nodes) {
    add(nodes, dynamics::feedforward_compressor_descriptor(),
        {realization("zero_lookahead", dynamics::make_feedforward_compressor_node(0.0f)),
         realization("lookahead_3ms", dynamics::make_feedforward_compressor_node(3.0f)),
         realization("lookahead_10ms", dynamics::make_feedforward_compressor_node(10.0f))});
    add(nodes, dynamics::true_peak::descriptor(),
        {realization("linked_0ms", dynamics::true_peak::make_node(0.0f, true)),
         realization("linked_5ms", dynamics::true_peak::make_node(5.0f, true)),
         realization("linked_10ms", dynamics::true_peak::make_node(10.0f, true)),
         realization("independent_5ms", dynamics::true_peak::make_node(5.0f, false))});
    add(nodes, dynamics::vca::vca_compressor_descriptor(),
        {realization("default", dynamics::vca::make_vca_compressor_node(0.0f, 4.0)),
         realization("lookahead_3ms_k4", dynamics::vca::make_vca_compressor_node(3.0f, 4.0)),
         realization("lookahead_10ms_k4", dynamics::vca::make_vca_compressor_node(10.0f, 4.0)),
         realization("zero_latency_k2", dynamics::vca::make_vca_compressor_node(0.0f, 2.0)),
         realization("zero_latency_k8", dynamics::vca::make_vca_compressor_node(0.0f, 8.0))});
    add(nodes, dynamics::fet::fet_compressor_descriptor(),
        {realization("default", dynamics::fet::make_fet_compressor_node())});
    add(nodes, dynamics::diode::diode_bridge_compressor_descriptor(),
        {
            realization("feedback", dynamics::diode::make_diode_bridge_compressor_node(true)),
            realization("feedforward", dynamics::diode::make_diode_bridge_compressor_node(false)),
        });
}

} // namespace pulp::host::forge_catalog_export_detail
