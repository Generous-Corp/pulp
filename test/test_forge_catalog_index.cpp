#include <catch2/catch_test_macros.hpp>

#include <pulp/host/forge_catalog_index.hpp>

#include <algorithm>
#include <filesystem>
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

} // namespace

TEST_CASE("Forge catalog index covers every catalog header", "[host][forge-catalog]") {
    const CatalogIndexDiff diff =
        compare_catalog_headers(discovered_catalog_headers(), indexed_catalog_headers());

    INFO("catalog headers missing from forge_catalog_index.hpp: "
         << join_headers(diff.missing_from_index));
    REQUIRE(diff.missing_from_index.empty());
    INFO("stale catalog headers in forge_catalog_index.hpp: " << join_headers(diff.stale_in_index));
    REQUIRE(diff.stale_in_index.empty());
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
}
