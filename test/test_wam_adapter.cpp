// Native unit coverage for the WAMv2 Processor->WASM bridge.
//
// The bridge ships compiled to WebAssembly (-fno-exceptions) and decodes
// untrusted host/JS input on the AudioWorklet render thread, so its hardening
// (non-throwing parameter parsing, frame-count bounding, JSON escaping) must be
// covered on a deterministic native path, not only through the emcc artifact.
//
// This compiles core/format/src/wasm/wam_adapter.cpp directly and links the
// real pulp libraries (so Processor::create_view() comes from format.cpp — no
// headless stub here, that is WASM-only).

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <pulp/format/web/wam_adapter.hpp>
#include "../examples/pulp-gain/pulp_gain.hpp"

#include <cmath>
#include <string>
#include <vector>

using pulp::format::wam::WamProcessorBridge;
using pulp::format::wam::WamDescriptorData;

namespace {
float rms(const std::vector<float>& v, int count) {
    double s = 0.0;
    for (int i = 0; i < count; ++i) s += static_cast<double>(v[i]) * v[i];
    return static_cast<float>(std::sqrt(s / count));
}
} // namespace

TEST_CASE("WAM bridge rejects malformed parameter ids without throwing", "[wam][rt-safety]") {
    WamProcessorBridge bridge(pulp::examples::create_pulp_gain);
    REQUIRE(bridge.initialize(48000.0, 128));

    // Under -fno-exceptions a thrown std::stoi would abort the worklet; the
    // from_chars guard must make these safe no-ops.
    REQUIRE_NOTHROW(bridge.set_parameter_value("not_a_number", 1.0f));
    REQUIRE_NOTHROW(bridge.set_parameter_value("", 0.5f));
    REQUIRE_NOTHROW(bridge.set_parameter_value("12x", 0.5f));
    REQUIRE(bridge.get_parameter_value("abc") == 0.0f);
}

TEST_CASE("WAM bridge clamps oversized blocks instead of overrunning", "[wam][rt-safety]") {
    WamProcessorBridge bridge(pulp::examples::create_pulp_gain);
    REQUIRE(bridge.initialize(48000.0, 128)); // planar buffers sized to 128

    std::vector<float> in(2 * 256, 0.25f), out(2 * 256, -99.0f);
    REQUIRE_NOTHROW(bridge.process(in.data(), out.data(), 2, 256)); // 256 > 128

    for (int i = 0; i < 2 * 128; ++i) REQUIRE(std::isfinite(out[i]));
}

TEST_CASE("WAM descriptor JSON escapes quotes and backslashes", "[wam]") {
    WamDescriptorData d;
    d.name = "Ev\"il\\Name";
    d.vendor = "Acme\tInc";
    std::string json = d.to_json();

    REQUIRE(json.find("Ev\\\"il\\\\Name") != std::string::npos);
    REQUIRE(json.find("Ev\"il") == std::string::npos); // no raw unescaped quote
}

TEST_CASE("WAM bridge exposes parameter metadata as JSON", "[wam]") {
    WamProcessorBridge bridge(pulp::examples::create_pulp_gain);
    REQUIRE(bridge.initialize(48000.0, 128));

    const std::string json = bridge.parameters_json();
    // PulpGain's three parameters, with id/label/unit/type fields.
    REQUIRE(json.front() == '[');
    REQUIRE(json.find("\"label\":\"Input Gain\"") != std::string::npos);
    REQUIRE(json.find("\"label\":\"Output Gain\"") != std::string::npos);
    REQUIRE(json.find("\"label\":\"Bypass\"") != std::string::npos);
    REQUIRE(json.find("\"unit\":\"dB\"") != std::string::npos);
    REQUIRE(json.find("\"type\":\"boolean\"") != std::string::npos); // Bypass
    REQUIRE(json.find("\"minValue\":-60") != std::string::npos);
    REQUIRE(json.find("\"step\":0.1") != std::string::npos);         // gain step
}

TEST_CASE("WAM bridge gain parameter and state round-trip", "[wam]") {
    WamProcessorBridge bridge(pulp::examples::create_pulp_gain);
    REQUIRE(bridge.initialize(48000.0, 128));

    constexpr int CH = 2, FR = 128, N = CH * FR;
    std::vector<float> in(N), out(N, 0.0f);
    for (int f = 0; f < FR; ++f) { in[f * CH] = 0.5f; in[f * CH + 1] = -0.5f; }

    // Default 0 dB in/out -> unity passthrough, distinct L/R preserved.
    bridge.process(in.data(), out.data(), CH, FR);
    REQUIRE(rms(out, N) == Catch::Approx(0.5f).margin(0.01f));
    REQUIRE(out[0] == Catch::Approx(0.5f).margin(0.01f));   // L
    REQUIRE(out[1] == Catch::Approx(-0.5f).margin(0.01f));  // R

    // Output gain +6 dB (~2x). PulpGain ids: "1" input, "2" output, "3" bypass.
    bridge.set_parameter_value("2", 6.0f);
    bridge.process(in.data(), out.data(), CH, FR);
    REQUIRE(rms(out, N) == Catch::Approx(1.0f).margin(0.03f));

    // Parameter read-back.
    bridge.set_parameter_value("1", 3.5f);
    REQUIRE(bridge.get_parameter_value("1") == Catch::Approx(3.5f).margin(1e-4f));

    // State round-trip: snapshot, mutate, restore.
    bridge.set_parameter_value("1", 7.0f);
    std::vector<uint8_t> saved = bridge.get_state();
    REQUIRE(saved.size() > 0);
    bridge.set_parameter_value("1", -12.0f);
    REQUIRE(bridge.set_state(saved.data(), saved.size()));
    REQUIRE(bridge.get_parameter_value("1") == Catch::Approx(7.0f).margin(1e-3f));
}
