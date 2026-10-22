// Build-time (embedded) script bytecode: the blob pulp_add_js_bytecode()
// embeds, its registration, and the bytecode cache taking it instead of
// compiling.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/js_engine.hpp>
#include <pulp/view/widget_bridge.hpp>

#include "js_bytecode_embed_bridge_fixture_data.hpp"
#include "js_bytecode_embed_fixture_data.hpp"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace pulp::view;

namespace {

std::string read_fixture() {
    std::ifstream in(PULP_TEST_JS_BYTECODE_FIXTURE, std::ios::binary);
    REQUIRE(in);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Fresh state for every case: nothing cached, nothing registered.
struct Reset {
    Reset() {
        clear_script_bytecode_cache();
        detail::clear_embedded_script_bytecode_for_tests();
    }
    ~Reset() {
        clear_script_bytecode_cache();
        detail::clear_embedded_script_bytecode_for_tests();
    }
};

double evaluate_fresh(const std::string& code) {
    auto engine = create_js_engine(JsEngineType::quickjs);
    REQUIRE(engine);
    return engine->evaluate_script(code).getWithDefault<double>(-1);
}

double reference_result(const std::string& code) {
    auto engine = create_js_engine(JsEngineType::quickjs);
    REQUIRE(engine);
    return engine->evaluate(code).getWithDefault<double>(-2);
}

// Offset of the bytecode payload: everything after the header.
std::size_t payload_offset(const std::vector<std::uint8_t>& blob) {
    const std::size_t fingerprint =
        blob[12] | (blob[13] << 8) | (blob[14] << 16) | (std::size_t(blob[15]) << 24);
    return 16 + fingerprint + 8 + 32 + 8 + 32;
}

} // namespace

TEST_CASE("Embedded bytecode replaces the compile and runs the same program",
          "[view][js-engine][editor-open]") {
    Reset reset;
    const auto code = read_fixture();
    const auto expected = reference_result(code);
    REQUIRE(expected > 0);

    const auto blob = build_embedded_script_bytecode(code);
    REQUIRE_FALSE(blob.empty());
    REQUIRE(register_embedded_script_bytecode(blob.data(), blob.size()));

    CHECK(evaluate_fresh(code) == expected);
    auto stats = script_bytecode_cache_stats();
    CHECK(stats.embedded == 1);
    CHECK(stats.compiles == 0);

    // A later realm reads the now-cached bytecode as an ordinary hit.
    CHECK(evaluate_fresh(code) == expected);
    stats = script_bytecode_cache_stats();
    CHECK(stats.hits == 1);
    CHECK(stats.compiles == 0);
}

TEST_CASE("Embedded bytecode serves the instantiation precompile without compiling",
          "[view][js-engine][editor-open]") {
    Reset reset;
    const auto code = read_fixture();
    const auto blob = build_embedded_script_bytecode(code);
    REQUIRE(register_embedded_script_bytecode(blob.data(), blob.size()));

    CHECK(precompile_scripts({code}) == 0); // nothing compiled...
    CHECK(script_bytecode_cached(code));    // ...yet the editor will hit
    auto stats = script_bytecode_cache_stats();
    CHECK(stats.embedded == 1);
    CHECK(stats.compiles == 0);
    CHECK(evaluate_fresh(code) == reference_result(code));
    CHECK(script_bytecode_cache_stats().compiles == 0);
}

TEST_CASE("Embedded bytecode is never used for a different source",
          "[view][js-engine][editor-open]") {
    Reset reset;
    const auto code = read_fixture();
    const auto blob = build_embedded_script_bytecode(code);
    REQUIRE(register_embedded_script_bytecode(blob.data(), blob.size()));

    // Same length, one character different: the length filter passes, the
    // SHA-256 must not.
    auto edited = code;
    const auto at = edited.find("(i * 7 + 3)");
    REQUIRE(at != std::string::npos);
    edited[at + 5] = '5';
    REQUIRE(edited.size() == code.size());

    CHECK(evaluate_fresh(edited) == reference_result(edited));
    CHECK(reference_result(edited) != reference_result(code));
    auto stats = script_bytecode_cache_stats();
    CHECK(stats.embedded == 0);
    CHECK(stats.compiles == 1);
}

TEST_CASE("Malformed or foreign embedded bytecode is refused", "[view][js-engine][editor-open]") {
    Reset reset;
    const auto code = read_fixture();
    const auto blob = build_embedded_script_bytecode(code);
    REQUIRE(blob.size() > 100);

    SECTION("truncated") {
        CHECK_FALSE(register_embedded_script_bytecode(blob.data(), blob.size() - 1));
    }
    SECTION("padded") {
        auto padded = blob;
        padded.push_back(0);
        CHECK_FALSE(register_embedded_script_bytecode(padded.data(), padded.size()));
    }
    SECTION("bad magic") {
        auto bad = blob;
        bad[0] = 'X';
        CHECK_FALSE(register_embedded_script_bytecode(bad.data(), bad.size()));
    }
    SECTION("unknown format") {
        auto bad = blob;
        bad[8] = 99;
        CHECK_FALSE(register_embedded_script_bytecode(bad.data(), bad.size()));
    }
    SECTION("another engine build") {
        auto foreign = blob;
        foreign[16 + 2] ^= 0x01; // inside the fingerprint string
        CHECK_FALSE(register_embedded_script_bytecode(foreign.data(), foreign.size()));
    }
    SECTION("the cross-compile placeholder") {
        const std::uint8_t placeholder[] = {'-'};
        CHECK_FALSE(register_embedded_script_bytecode(placeholder, sizeof placeholder));
        CHECK_FALSE(register_embedded_script_bytecode(nullptr, 0));
    }
    // Nothing was registered: the script compiles from source as before.
    CHECK(evaluate_fresh(code) == reference_result(code));
    CHECK(script_bytecode_cache_stats().embedded == 0);
}

TEST_CASE("Embedded bytecode that fails its integrity check compiles from source",
          "[view][js-engine][editor-open]") {
    Reset reset;
    const auto code = read_fixture();
    auto blob = build_embedded_script_bytecode(code);
    // A well-formed header over a damaged payload registers (the header is
    // checked eagerly) but is never deserialized.
    blob[payload_offset(blob) + 4] ^= 0xFF;
    REQUIRE(register_embedded_script_bytecode(blob.data(), blob.size()));

    CHECK(evaluate_fresh(code) == reference_result(code));
    auto stats = script_bytecode_cache_stats();
    CHECK(stats.embedded == 0);
    CHECK(stats.compiles == 1);
}

TEST_CASE("The engine bytecode fingerprint is stable and names the engine",
          "[view][js-engine][editor-open]") {
    const auto fingerprint = script_bytecode_engine_fingerprint();
    CHECK(fingerprint.rfind("quickjs-", 0) == 0);
    CHECK(fingerprint.find("unavailable") == std::string::npos);
    CHECK(fingerprint == script_bytecode_engine_fingerprint());
}

TEST_CASE("pulp_add_js_bytecode embeds blobs this engine accepts",
          "[view][js-engine][editor-open]") {
    Reset reset;
    const auto code = read_fixture();
    using namespace js_bytecode_embed_fixture;

    SECTION("a plain script") {
        REQUIRE(register_embedded_script_bytecode(embedded_fixture_js_pjsbc,
                                                  embedded_fixture_js_pjsbc_size));
        CHECK(evaluate_fresh(code) == reference_result(code));
        CHECK(script_bytecode_cache_stats().embedded == 1);
        CHECK(script_bytecode_cache_stats().compiles == 0);
    }
    SECTION("a WidgetBridge script matches what load_script() evaluates") {
        REQUIRE(register_embedded_script_bytecode(embedded_bridge_fixture_js_pjsbc,
                                                  embedded_bridge_fixture_js_pjsbc_size));
        const auto loaded = WidgetBridge::loaded_script_source(code);
        evaluate_fresh(loaded);
        CHECK(script_bytecode_cache_stats().embedded == 1);
        CHECK(script_bytecode_cache_stats().compiles == 0);
        // Control: the raw script is not what that blob was compiled from.
        evaluate_fresh(code);
        CHECK(script_bytecode_cache_stats().compiles == 1);
    }
}
