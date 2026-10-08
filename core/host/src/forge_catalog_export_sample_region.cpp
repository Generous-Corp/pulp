#include "forge_catalog_export_detail.hpp"

#include <pulp/host/signal_graph.hpp>

#include <iterator>
#include <stdexcept>

namespace pulp::host::forge_catalog_export_detail {
namespace {

// These keys and descriptions are Forge catalog policy. The registered sample
// region descriptors themselves remain generic Pulp host machinery; this
// adapter joins them to the existing Forge projection without changing its
// public IDs, ordering, or JSON shape.
constexpr std::string_view kRegionKeys[] = {
    "sample_region_input_boundary",
    "sample_region_output_boundary",
    "sample_region_constant",
    "sample_region_parameter",
    "sample_region_add",
    "sample_region_multiply",
    "sample_region_unit_delay",
};

SampleKernelConfigKind config_kind(std::string_view value) noexcept {
    if (value == "BoundaryIndex")
        return SampleKernelConfigKind::BoundaryIndex;
    if (value == "FiniteConstant")
        return SampleKernelConfigKind::FiniteConstant;
    if (value == "PromotedParameterId")
        return SampleKernelConfigKind::PromotedParameterId;
    if (value == "None")
        return SampleKernelConfigKind::None;
    return SampleKernelConfigKind::Invalid;
}

} // namespace

void append_sample_region(Nodes& nodes) {
    SignalGraph registry;
    if (!register_builtin_sample_region_types(registry))
        throw std::logic_error("sample-region catalog registration failed");

    std::size_t key_index = 0;
    for (const auto& row : kForgeSampleRegionV1) {
        if (key_index >= std::size(kRegionKeys))
            throw std::logic_error("sample-region Forge projection has too many routes");

        const auto* scalar = registry.sample_kernel_type(row.type_id, row.type_version);
        const auto* block = registry.custom_node_type(row.type_id, row.type_version);
        if (!scalar || !block || scalar->version != row.sample_kernel_version ||
            scalar->authored_config_kind != config_kind(row.config_kind) ||
            scalar->num_input_ports != row.inputs || scalar->num_output_ports != row.outputs ||
            scalar->state_size != row.state_bytes || scalar->state_alignment != row.state_alignment)
            throw std::logic_error("sample-region catalog differs from registered exact kernel");

        ForgeNodeDescriptor descriptor;
        descriptor.key = kRegionKeys[key_index++];
        descriptor.label = row.label;
        descriptor.description =
            row.placement == "region_builder_only"
                ? "Internal exact-version region boundary; created only by the region builder."
                : "Exact-version block and scalar node for ordinary placement or a sample region.";
        descriptor.realizations.emplace_back("default", row.type_id);
        add(nodes, std::move(descriptor), {realization("default", *block)});
    }

    if (key_index != std::size(kRegionKeys))
        throw std::logic_error("sample-region Forge projection is missing routes");
}

} // namespace pulp::host::forge_catalog_export_detail
