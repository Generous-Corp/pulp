#include <catch2/catch_test_macros.hpp>

#include <pulp/host/forge_catalog_export.hpp>
#include <pulp/host/forge_catalog_index.hpp>
#include <pulp/host/signal_graph.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct CatalogIndexDiff {
    std::vector<std::string> missing_from_index;
    std::vector<std::string> stale_in_index;
};

CatalogIndexDiff compare_catalog_headers(const std::set<std::string>& discovered,
                                         const std::set<std::string>& indexed) {
    CatalogIndexDiff diff;
    std::set_difference(discovered.begin(), discovered.end(), indexed.begin(), indexed.end(),
                        std::back_inserter(diff.missing_from_index));
    std::set_difference(indexed.begin(), indexed.end(), discovered.begin(), discovered.end(),
                        std::back_inserter(diff.stale_in_index));
    return diff;
}

std::string join_headers(const std::vector<std::string>& headers) {
    std::string joined;
    for (const auto& header : headers) {
        if (!joined.empty())
            joined += ", ";
        joined += header;
    }
    return joined;
}

std::set<std::string> indexed_catalog_headers() {
    std::set<std::string> headers;
    for (const std::string_view name : pulp::host::forge_catalog::kHeaderNames) {
        headers.emplace(name);
    }
    return headers;
}

std::set<std::string> discovered_catalog_headers() {
    const std::filesystem::path include_dir =
        std::filesystem::path(PULP_SOURCE_DIR) / "core/host/include/pulp/host";
    std::set<std::string> headers;
    for (const auto& entry : std::filesystem::directory_iterator(include_dir)) {
        if (!entry.is_regular_file())
            continue;
        const std::string name = entry.path().filename().string();
        if (name == "forge_catalog_index.hpp")
            continue;
        if (name.starts_with("forge_") && name.ends_with("_catalog.hpp"))
            headers.emplace(name);
    }
    return headers;
}

std::set<std::string> included_catalog_headers() {
    const std::filesystem::path index_header =
        std::filesystem::path(PULP_SOURCE_DIR) /
        "core/host/include/pulp/host/forge_catalog_index.hpp";
    std::ifstream input(index_header);
    REQUIRE(input.is_open());

    constexpr std::string_view prefix = "#include <pulp/host/";
    std::set<std::string> headers;
    for (std::string line; std::getline(input, line);) {
        if (!line.starts_with(prefix) || !line.ends_with('>'))
            continue;
        const std::string name =
            line.substr(prefix.size(), line.size() - prefix.size() - 1);
        if (name.starts_with("forge_") && name.ends_with("_catalog.hpp"))
            headers.emplace(name);
    }
    return headers;
}

} // namespace

TEST_CASE("Forge catalog index covers every catalog header", "[host][forge-catalog]") {
    const std::set<std::string> indexed = indexed_catalog_headers();
    const CatalogIndexDiff diff = compare_catalog_headers(discovered_catalog_headers(), indexed);

    INFO("catalog headers missing from forge_catalog_index.hpp: "
         << join_headers(diff.missing_from_index));
    REQUIRE(diff.missing_from_index.empty());
    INFO("stale catalog headers in forge_catalog_index.hpp: " << join_headers(diff.stale_in_index));
    REQUIRE(diff.stale_in_index.empty());

    const CatalogIndexDiff include_diff =
        compare_catalog_headers(indexed, included_catalog_headers());
    INFO("indexed catalog headers missing an umbrella include: "
         << join_headers(include_diff.missing_from_index));
    REQUIRE(include_diff.missing_from_index.empty());
    INFO("umbrella includes missing from kHeaderNames: "
         << join_headers(include_diff.stale_in_index));
    REQUIRE(include_diff.stale_in_index.empty());
}

TEST_CASE("Forge catalog index comparison detects add and remove drift", "[host][forge-catalog]") {
    const std::set<std::string> indexed{"forge_delay_catalog.hpp", "forge_filter_catalog.hpp"};

    const CatalogIndexDiff added = compare_catalog_headers(
        {"forge_delay_catalog.hpp", "forge_filter_catalog.hpp", "forge_reverb_catalog.hpp"},
        indexed);
    REQUIRE(added.missing_from_index == std::vector<std::string>{"forge_reverb_catalog.hpp"});
    REQUIRE(added.stale_in_index.empty());

    const CatalogIndexDiff removed = compare_catalog_headers({"forge_filter_catalog.hpp"}, indexed);
    REQUIRE(removed.missing_from_index.empty());
    REQUIRE(removed.stale_in_index == std::vector<std::string>{"forge_delay_catalog.hpp"});

    const CatalogIndexDiff omitted_include =
        compare_catalog_headers(indexed, {"forge_delay_catalog.hpp"});
    REQUIRE(omitted_include.missing_from_index ==
            std::vector<std::string>{"forge_filter_catalog.hpp"});

    const CatalogIndexDiff stale_include =
        compare_catalog_headers({"forge_delay_catalog.hpp"}, indexed);
    REQUIRE(stale_include.stale_in_index == std::vector<std::string>{"forge_filter_catalog.hpp"});
}

TEST_CASE("sample-region Forge routes bind exact registered scalar identities",
          "[forge][catalog][sample-region]") {
    using namespace pulp::host;
    SignalGraph graph;
    REQUIRE(register_builtin_sample_region_types(graph));
    const auto nodes = forge_catalog_export_nodes();
    REQUIRE(audit_forge_catalog_export(nodes).empty());
    std::set<std::string_view> roles;
    for (const auto& row : kForgeSampleRegionV1) {
        REQUIRE(roles.insert(row.role).second);
        const auto* scalar = graph.sample_kernel_type(row.type_id, row.type_version);
        REQUIRE(scalar != nullptr);
        REQUIRE(is_sample_region_v1_descriptor(*scalar));
        REQUIRE(scalar->version == row.sample_kernel_version);
        REQUIRE(scalar->num_input_ports == row.inputs);
        REQUIRE(scalar->num_output_ports == row.outputs);
        REQUIRE(scalar->state_size == row.state_bytes);
        REQUIRE(scalar->state_alignment == row.state_alignment);
        const auto* block = graph.custom_node_type(row.type_id, row.type_version);
        REQUIRE(block != nullptr);
        REQUIRE(block->version == row.type_version);
        std::size_t routes = 0;
        for (const auto& node : nodes)
            for (const auto& realization : node.realizations)
                if (realization.type_id == row.type_id)
                    ++routes;
        REQUIRE(routes == 1);
        const bool boundary = row.role == "input_boundary" || row.role == "output_boundary";
        REQUIRE(row.placement == (boundary ? "region_builder_only" : "normal_node"));
        const auto expected_config =
            boundary                  ? SampleKernelConfigKind::BoundaryIndex
            : row.role == "constant"  ? SampleKernelConfigKind::FiniteConstant
            : row.role == "parameter" ? SampleKernelConfigKind::PromotedParameterId
                                      : SampleKernelConfigKind::None;
        REQUIRE(scalar->authored_config_kind == expected_config);
        const std::string_view config_name = boundary                  ? "BoundaryIndex"
                                             : row.role == "constant"  ? "FiniteConstant"
                                             : row.role == "parameter" ? "PromotedParameterId"
                                                                       : "None";
        REQUIRE(row.config_kind == config_name);
    }
    REQUIRE(roles.size() == 7);
    REQUIRE(forge_sample_region_v1("unknown.legacy.node") == nullptr);
}
