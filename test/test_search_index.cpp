// Unit tests for pulp::runtime::SearchIndex — the pure ranking/matching core of
// the off-UI-thread query service (R7). No threads here; threading is covered by
// test_query_service.cpp.

#include <catch2/catch_test_macros.hpp>

#include <pulp/runtime/search_index.hpp>

#include <algorithm>
#include <string>
#include <vector>

using pulp::runtime::CancellationToken;
using pulp::runtime::SearchIndex;
using pulp::runtime::SearchQueryOptions;

namespace {

std::vector<std::size_t> indices(const std::vector<SearchIndex::Match>& matches) {
    std::vector<std::size_t> out;
    out.reserve(matches.size());
    for (const auto& m : matches) out.push_back(m.index);
    return out;
}

SearchIndex make_index(std::vector<std::string> items) {
    SearchIndex index;
    index.build(std::move(items));
    return index;
}

}  // namespace

TEST_CASE("SearchIndex empty query returns every item in order", "[runtime][search-index]") {
    auto index = make_index({"kick", "snare", "hat"});
    auto matches = index.query("");
    REQUIRE(indices(matches) == std::vector<std::size_t>{0, 1, 2});
}

TEST_CASE("SearchIndex empty query honours max_results", "[runtime][search-index]") {
    auto index = make_index({"a", "b", "c", "d"});
    SearchQueryOptions options;
    options.max_results = 2;
    REQUIRE(index.query("", options).size() == 2);
}

TEST_CASE("SearchIndex ranks exact over prefix over substring over fuzzy",
          "[runtime][search-index]") {
    // "kick"        -> exact
    // "kick_deep"   -> prefix
    // "sidekick"    -> substring (not at position 0)
    // "knick_knack" -> fuzzy subsequence (k-i-c-k) but no contiguous "kick"
    auto index = make_index({"sidekick", "kick_deep", "kick", "knick_knack", "snare"});
    auto matches = index.query("kick");

    // "snare" has no k-i-c-k subsequence, so it must not match.
    REQUIRE(matches.size() == 4);
    // Best first: exact, then prefix, then substring, then fuzzy.
    REQUIRE(index.item(matches[0].index) == "kick");
    REQUIRE(index.item(matches[1].index) == "kick_deep");
    REQUIRE(index.item(matches[2].index) == "sidekick");
    REQUIRE(index.item(matches[3].index) == "knick_knack");
    // Scores strictly descending across tiers.
    REQUIRE(matches[0].score > matches[1].score);
    REQUIRE(matches[1].score > matches[2].score);
    REQUIRE(matches[2].score > matches[3].score);
}

TEST_CASE("SearchIndex matching is ASCII case-insensitive by default",
          "[runtime][search-index]") {
    auto index = make_index({"KICK", "Snare"});
    REQUIRE(indices(index.query("kick")) == std::vector<std::size_t>{0});
    REQUIRE(indices(index.query("SNARE")) == std::vector<std::size_t>{1});
}

TEST_CASE("SearchIndex case_sensitive option is respected", "[runtime][search-index]") {
    auto index = make_index({"KICK", "kick"});
    SearchQueryOptions options;
    options.case_sensitive = true;
    options.fuzzy = false;
    REQUIRE(indices(index.query("kick", options)) == std::vector<std::size_t>{1});
}

TEST_CASE("SearchIndex fuzzy=false excludes non-contiguous matches",
          "[runtime][search-index]") {
    auto index = make_index({"kick", "kraken"});
    SearchQueryOptions options;
    options.fuzzy = false;
    // "kraken" only matches "kick" as a subsequence, which fuzzy=false forbids.
    REQUIRE(indices(index.query("kick", options)) == std::vector<std::size_t>{0});
}

TEST_CASE("SearchIndex ranks shorter items ahead of longer ones", "[runtime][search-index]") {
    // Both are prefix matches for "ki"; the shorter item scores higher via the
    // length bonus (and the length tiebreak backs it up on exact ties).
    auto index = make_index({"kick_drum_long_name", "kit"});
    auto matches = index.query("ki");
    REQUIRE(matches.size() == 2);
    REQUIRE(index.item(matches[0].index) == "kit");
}

TEST_CASE("SearchIndex caps results with max_results", "[runtime][search-index]") {
    std::vector<std::string> items;
    for (int i = 0; i < 1000; ++i) items.push_back("kick_" + std::to_string(i));
    auto index = make_index(std::move(items));

    SearchQueryOptions options;
    options.max_results = 10;
    REQUIRE(index.query("kick", options).size() == 10);
}

TEST_CASE("SearchIndex query bails out when cancellation is observed",
          "[runtime][search-index]") {
    std::vector<std::string> items;
    for (int i = 0; i < 100000; ++i) items.push_back("kick_" + std::to_string(i));
    auto index = make_index(std::move(items));

    CancellationToken cancel;
    cancel.cancel();  // already cancelled -> first poll returns early
    auto matches = index.query("kick", {}, cancel);
    REQUIRE(matches.empty());
}

TEST_CASE("SearchIndex handles a large library quickly and deterministically",
          "[runtime][search-index]") {
    std::vector<std::string> items;
    items.reserve(50000);
    for (int i = 0; i < 50000; ++i) items.push_back("sample_" + std::to_string(i) + ".wav");
    auto index = make_index(std::move(items));
    REQUIRE(index.size() == 50000);

    SearchQueryOptions options;
    options.max_results = 25;
    auto a = index.query("123", options);
    auto b = index.query("123", options);
    REQUIRE(a.size() <= 25);
    REQUIRE_FALSE(a.empty());
    REQUIRE(indices(a) == indices(b));  // deterministic
}
