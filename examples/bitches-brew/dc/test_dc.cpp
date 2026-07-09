// DC — the suite's held-value guard.
//
// A control voltage is a number that must arrive unchanged. Every assertion
// here uses `==` on floats deliberately: DC is the one place in the codebase
// where "close enough" is the bug. A tolerance would hide exactly the
// smoothing, ramping, or dithering this test exists to forbid.
//
// Scope note. These tests exercise `Processor::process()`. They do NOT prove a
// held value survives the *host bus* — an adapter can hand the host a correct
// buffer flagged as silent, and the host will then substitute silence. That
// defect is invisible from here and is covered at the adapter boundary by
// test_au_v2_effect.cpp and test_vst3_plugin_state.cpp (`[silence]`). Neither
// layer proves a sample value becomes a real voltage; that needs hardware.

#include <catch2/catch_test_macros.hpp>

#include "dc_processor.hpp"
#include <pulp/format/headless.hpp>

#include <vector>

using namespace pulp;
using namespace pulp::examples::brew;

namespace {

/// Render one block and return every output sample, so a test can assert on the
/// whole buffer rather than spot-checking sample 0.
std::vector<float> render_block(format::HeadlessHost& host,
                                int num_samples,
                                int channels = 2,
                                const format::ProcessContext* ctx = nullptr) {
    audio::Buffer<float> in(static_cast<std::size_t>(channels),
                            static_cast<std::size_t>(num_samples));
    audio::Buffer<float> out(static_cast<std::size_t>(channels),
                             static_cast<std::size_t>(num_samples));
    in.clear();
    out.clear();

    std::vector<const float*> in_ptrs(static_cast<std::size_t>(channels));
    for (int c = 0; c < channels; ++c)
        in_ptrs[static_cast<std::size_t>(c)] = in.channel(c).data();
    audio::BufferView<const float> iv(in_ptrs.data(),
                                      static_cast<std::size_t>(channels),
                                      static_cast<std::size_t>(num_samples));
    auto ov = out.view();
    if (ctx != nullptr)
        host.process(ov, iv, *ctx);
    else
        host.process(ov, iv);

    std::vector<float> flat;
    flat.reserve(static_cast<std::size_t>(channels * num_samples));
    for (int c = 0; c < channels; ++c)
        for (int n = 0; n < num_samples; ++n)
            flat.push_back(out.channel(c)[static_cast<std::size_t>(n)]);
    return flat;
}

bool all_equal(const std::vector<float>& v, float expected) {
    for (float s : v)
        if (s != expected) return false;
    return !v.empty();
}

}  // namespace

TEST_CASE("DC descriptor is a two-channel CV source", "[brew][dc]") {
    auto proc = create_dc();
    const auto desc = proc->descriptor();
    REQUIRE(desc.name == "DC");
    REQUIRE(desc.manufacturer == "Bitches Brew");
    REQUIRE(desc.category == format::PluginCategory::Effect);
    // No MIDI: DC is driven entirely by its own parameters.
    REQUIRE_FALSE(desc.accepts_midi);
}

// A fresh instance must emit no voltage. This is a hardware-safety property,
// not a style choice: a CV generator that comes up at full scale points a
// voltage at whatever module happens to be patched to its output.
TEST_CASE("DC emits silence until asked for a value", "[brew][dc][safety]") {
    format::HeadlessHost host(create_dc);
    host.prepare(48000.0, 512, 2, 2);
    REQUIRE(all_equal(render_block(host, 512), 0.0f));
}

// The guard the whole suite leans on: what you set is exactly what comes out,
// on every sample, at every block size, at every sample rate. Bit-exact.
TEST_CASE("DC holds its value bit-exactly across block sizes and sample rates",
          "[brew][dc][dc-fidelity]") {
    // 1 is not academic — hosts really do render single-sample blocks.
    const int block_sizes[] = {1, 32, 64, 512, 4096};
    const double sample_rates[] = {44100.0, 48000.0, 96000.0, 192000.0};
    const float values[] = {0.5f, -0.5f, 1.0f, -1.0f, 0.0f, 0.001f, 0.333333f};

    for (double sr : sample_rates) {
        for (int block : block_sizes) {
            for (float v : values) {
                format::HeadlessHost host(create_dc);
                host.prepare(sr, 4096, 2, 2);
                host.state().set_value(DcProcessor::kValue, v);

                CAPTURE(sr, block, v);
                REQUIRE(all_equal(render_block(host, block), v));

                // Successive blocks must not drift: no accumulator, no ramp.
                REQUIRE(all_equal(render_block(host, block), v));
            }
        }
    }
}

// The output stage itself (clamp / scale / invert) is brew-core's, and is tested
// there. What DC owes is proof that its parameters actually reach it.
TEST_CASE("DC scale and invert reach the rendered buffer", "[brew][dc]") {
    format::HeadlessHost host(create_dc);
    host.prepare(48000.0, 512, 2, 2);

    host.state().set_value(DcProcessor::kValue, 1.0f);
    host.state().set_value(DcProcessor::kOutputScale, 0.25f);
    REQUIRE(all_equal(render_block(host, 512), 0.25f));

    host.state().set_value(DcProcessor::kInvert, 1.0f);
    REQUIRE(all_equal(render_block(host, 512), -0.25f));
}

// Bypass means stop driving the patch. Some hosts bypass by short-circuiting
// process(); others keep calling it, and on those a plug-in that ignores the flag
// leaves its voltage at the jack after the user pressed Bypass.
TEST_CASE("DC emits nothing while bypassed", "[brew][dc][safety][bypass]") {
    format::HeadlessHost host(create_dc);
    host.prepare(48000.0, 512, 2, 2);
    host.state().set_value(DcProcessor::kValue, 0.75f);

    format::ProcessContext ctx;
    ctx.sample_rate = 48000.0;
    ctx.num_samples = 512;
    ctx.is_bypassed = true;
    REQUIRE(all_equal(render_block(host, 512, 2, &ctx), 0.0f));

    // And the value comes straight back when the host un-bypasses.
    ctx.is_bypassed = false;
    REQUIRE(all_equal(render_block(host, 512, 2, &ctx), 0.75f));
}

// Whatever is on the input bus is irrelevant: DC generates, it does not process.
// This is also the shape that trips the adapter silence-flag bug — output
// synthesized from a silent input.
TEST_CASE("DC ignores its input bus", "[brew][dc]") {
    format::HeadlessHost host(create_dc);
    host.prepare(48000.0, 256, 2, 2);
    host.state().set_value(DcProcessor::kValue, 0.75f);
    REQUIRE(all_equal(render_block(host, 256), 0.75f));
}
