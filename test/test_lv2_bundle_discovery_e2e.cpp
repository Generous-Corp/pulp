// End-to-end: the bundle this build produced is discoverable the way a host
// discovers one.
//
// The two halves of LV2 packaging were only ever tested apart. The generator
// suite asserts the strings `generate_plugin_ttl()` returns; the discovery
// suite parses TTL written by hand into a scratch directory. Neither notices if
// what the build emits and what the reader expects drift apart, and the result
// of that drift is a bundle that builds clean and no host can see — the exact
// state every Pulp LV2 bundle was in before the build learned to emit a
// manifest at all.
//
// So this reads the real artifact: the .lv2 directory the build wrote, through
// pulp::host's own discovery entry points, starting from manifest.ttl.

#include <catch2/catch_test_macros.hpp>

#include "lv2_discovery.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

// The bundle directory the build wrote, injected by CMake so the test never
// guesses a layout.
fs::path fixture_bundle() {
    return fs::path(PULP_LV2_TTL_FIXTURE_BUNDLE_DIR);
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

} // namespace

TEST_CASE("a built LV2 bundle carries the manifest a host discovers it by", "[lv2][host][e2e]") {
    const auto bundle = fixture_bundle();

    // Control first: if the bundle itself is missing, every assertion below
    // would fail for a reason that has nothing to do with TTL emission.
    REQUIRE(fs::is_directory(bundle));
    REQUIRE_FALSE(pulp::host::detail::resolve_lv2_binary(bundle.string()).empty());

    const auto manifest = bundle / "manifest.ttl";
    INFO("bundle: " << bundle.string());
    REQUIRE(fs::exists(manifest));

    const auto manifest_text = read_file(manifest);
    // A host reads the URI and follows lv2:binary / rdfs:seeAlso out of here.
    REQUIRE(manifest_text.find("urn:pulp:test:lv2-ttl-fixture") != std::string::npos);
    REQUIRE(manifest_text.find("lv2:binary") != std::string::npos);
    REQUIRE(manifest_text.find("rdfs:seeAlso") != std::string::npos);

    SECTION("the seeAlso target the manifest names actually exists") {
        // The manifest derives this name by stripping the binary's extension.
        // If that ever disagrees with where the description was written, a host
        // reads the manifest, follows the pointer and finds nothing — which
        // looks to the user exactly like a plugin that does not exist.
        const auto binary = fs::path(pulp::host::detail::resolve_lv2_binary(bundle.string()));
        auto described = binary;
        described.replace_extension(".ttl");
        REQUIRE(fs::exists(described));
        REQUIRE_FALSE(read_file(described).empty());
    }
}

TEST_CASE("host discovery reads the built bundle's real port roles", "[lv2][host][e2e]") {
    const auto bundle = fixture_bundle();
    REQUIRE(fs::is_directory(bundle));

    // The reader under test is the one pulp::host uses, not a parser written
    // for the test.
    const auto roles = pulp::host::detail::discover_lv2_ports(bundle.string());

    // The fixture declares stereo in, stereo out, then two parameters -- and
    // the layout appends a latency output control port unconditionally, always
    // last. That trailing port is easy to forget and is exactly what makes the
    // count a real assertion: seven, not six. This is where the manifest and
    // the runtime are compared, so a host connecting by these indices reaches
    // the slots the adapter reads.
    REQUIRE(roles.size() == 7);

    for (int i = 0; i < 2; ++i) {
        INFO("audio input " << i);
        REQUIRE(roles[static_cast<std::size_t>(i)].index == i);
        REQUIRE(roles[static_cast<std::size_t>(i)].is_audio);
        REQUIRE(roles[static_cast<std::size_t>(i)].is_input);
    }
    for (int i = 2; i < 4; ++i) {
        INFO("audio output " << i);
        REQUIRE(roles[static_cast<std::size_t>(i)].index == i);
        REQUIRE(roles[static_cast<std::size_t>(i)].is_audio);
        REQUIRE_FALSE(roles[static_cast<std::size_t>(i)].is_input);
    }

    // Control ports follow the audio ports, in declaration order, carrying the
    // range the plugin declared -- this is what makes every Pulp parameter
    // reachable by a host at all.
    REQUIRE(roles[4].index == 4);
    REQUIRE(roles[4].is_control);
    REQUIRE(roles[4].is_input);
    REQUIRE(roles[4].name == "Gain");
    REQUIRE(roles[4].min_value == 0.0f);
    REQUIRE(roles[4].max_value == 2.0f);
    REQUIRE(roles[4].default_value == 1.0f);

    REQUIRE(roles[5].index == 5);
    REQUIRE(roles[5].is_control);
    REQUIRE(roles[5].is_input);
    REQUIRE(roles[5].name == "Mix");
    REQUIRE(roles[5].max_value == 1.0f);

    // The latency port is an OUTPUT control port and always the final index.
    // `connect_port()` reserves that index, so a plugin that grew a port
    // without the layout growing with it would collide here first.
    REQUIRE(roles[6].index == 6);
    REQUIRE(roles[6].is_control);
    REQUIRE_FALSE(roles[6].is_input);

    // Negative control: the fixture declares no MIDI, so no atom port may be
    // claimed and nothing may sit past the latency index. Without this, a
    // layout that silently shifted every control port by one would still
    // satisfy the count above.
    REQUIRE(
        std::none_of(roles.begin(), roles.end(), [](const auto& role) { return role.index > 6; }));
}
