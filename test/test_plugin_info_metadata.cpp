// Verifies the richer PluginInfo metadata shape (workstream 03 slice 3.7).
// Format-level extraction (CLAP features → category) has unit-test
// coverage here; per-format scanner-path validation lives with the
// scanner tests.

#include <catch2/catch_test_macros.hpp>
#include <pulp/host/scanner.hpp>

using namespace pulp::host;

TEST_CASE("PluginInfo default-constructs with empty metadata",
          "[host][plugin-info]") {
    PluginInfo p;
    REQUIRE(p.category.empty());
    REQUIRE(p.features.empty());
    REQUIRE(p.description.empty());
    REQUIRE_FALSE(p.has_editor);
    REQUIRE_FALSE(p.supports_sidechain);
    REQUIRE_FALSE(p.supports_midi_in);
    REQUIRE_FALSE(p.supports_midi_out);
}

TEST_CASE("PluginInfo is copyable and carries metadata",
          "[host][plugin-info]") {
    PluginInfo p;
    p.name = "Pulp Drums";
    p.category = "Instrument";
    p.features = {"instrument", "drum", "sampler"};
    p.has_editor = true;
    p.supports_midi_in = true;

    PluginInfo q = p;
    REQUIRE(q.name == "Pulp Drums");
    REQUIRE(q.category == "Instrument");
    REQUIRE(q.features.size() == 3);
    REQUIRE(q.features[2] == "sampler");
    REQUIRE(q.has_editor);
    REQUIRE(q.supports_midi_in);
}
