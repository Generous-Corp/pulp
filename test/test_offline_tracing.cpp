// Offline-render DSP tracing test.
//
// Proves the `dsp` / `dsp.node` Perfetto spans on the OFFLINE audio render path
// (offline_render / offline_process in core/audio). Offline is deadline-free and
// deterministic, so these spans carry no real-time hazard — unlike the live
// process() callback, which is never instrumented (plan §0c, trace.hpp).
//
// Config-agnostic: with PULP_TRACING=OFF (default/CI) it verifies the no-op
// contract — start() fails, no session is active, and the render still runs with
// the span macros compiled to nothing. With PULP_TRACING=ON it drives a real
// session, then byte-scans the flushed .pftrace for the interned span names (the
// D3-spike token check, mirroring test_tracing_session.cpp — no trace_processor
// needed).

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <string>
#include <vector>

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/offline_processor.hpp>
#include <pulp/format/headless.hpp>
#include <pulp/runtime/trace.hpp>
#include <pulp/runtime/trace_session.hpp>

// A real example effect plugin, to prove the offline `dsp.node` spans wrap real
// Processor::process() code and not just a synthetic callback. Only this one
// plugin header is included here: the example plugins share unscoped
// `pulp::examples::kBypass` enumerators that collide across a single TU.
#include "pulp_compressor.hpp"

using pulp::runtime::Tracing;

namespace {

// A tiny deterministic stereo clip: a few blocks' worth of samples.
pulp::audio::AudioFileData make_clip(uint32_t sample_rate, uint64_t frames) {
    pulp::audio::AudioFileData clip;
    clip.sample_rate = sample_rate;
    clip.channels.resize(2);
    for (auto& ch : clip.channels) {
        ch.resize(static_cast<size_t>(frames));
        for (size_t i = 0; i < ch.size(); ++i)
            ch[i] = 0.25f * static_cast<float>((i % 32)) / 32.0f;
    }
    return clip;
}

// A stand-in DSP callback that emits per-node `dsp.node` spans with static
// names (the compile-time-literal contract the macros enforce). One node is
// deliberately "expensive" — the trace, not the scalar load average, reveals it.
void render_two_node_chain(const float* in, float* out, int channels,
                           int block_size, double /*sample_rate*/) {
    {
        PULP_TRACE_SCOPE_NAMED("dsp.node", "oscillator_bank");
        for (int i = 0; i < channels * block_size; ++i) out[i] = in[i];
    }
    {
        PULP_TRACE_SCOPE_NAMED("dsp.node", "lead_oversampler");
        for (int i = 0; i < channels * block_size; ++i) {
            float x = out[i];
            // A little redundant work so the span has measurable width.
            for (int k = 0; k < 8; ++k) x = x - (x * x * x) * 0.16666667f;
            out[i] = x;
        }
    }
}

}  // namespace

TEST_CASE("offline render emits dsp / dsp.node spans", "[tracing][audio][offline]") {
    const auto clip = make_clip(48000, 2048);

    if (!pulp::runtime::kTracingEnabled) {
        // OFF contract: the controller no-ops and the render still succeeds with
        // the span macros compiled out.
        REQUIRE_FALSE(Tracing::start({"dsp", "dsp.node"}));
        REQUIRE_FALSE(Tracing::active());
        auto out = pulp::audio::offline_process(clip, render_two_node_chain, 512);
        REQUIRE(out.has_value());
        REQUIRE(out->num_frames() == clip.num_frames());
        REQUIRE_FALSE(Tracing::stop().ok);
        return;
    }

    // ON: capture the offline render into a real trace.
    auto path = std::filesystem::temp_directory_path() / "pulp-offline-trace.pftrace";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    REQUIRE(Tracing::start({"dsp", "dsp.node"}, path.string(), /*ring_kb=*/8192));
    REQUIRE(Tracing::active());

    auto out = pulp::audio::offline_process(clip, render_two_node_chain, 512);
    REQUIRE(out.has_value());
    REQUIRE(out->num_frames() == clip.num_frames());

    auto r = Tracing::stop();
    REQUIRE(r.ok);
    REQUIRE(r.trace_bytes > 0);
    REQUIRE_FALSE(Tracing::active());

    std::ifstream f(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    REQUIRE_FALSE(bytes.empty());
    // Interned span names are stored as UTF-8 in the trace stream.
    REQUIRE(bytes.find("offline_render") != std::string::npos);
    REQUIRE(bytes.find("offline_block") != std::string::npos);
    REQUIRE(bytes.find("oscillator_bank") != std::string::npos);
    REQUIRE(bytes.find("lead_oversampler") != std::string::npos);
    // Per-block span args (interned key names) the trace-stdlib SQL reads via
    // EXTRACT_ARG(arg_set_id, 'debug.<key>').
    REQUIRE(bytes.find("block_index") != std::string::npos);
    REQUIRE(bytes.find("position_samples") != std::string::npos);
}

namespace {

// PulpCompressor param IDs (avoid the header's unscoped enum names here so the
// intent — this is the compressor's threshold/ratio — stays explicit).
constexpr pulp::state::ParamID kCompThreshold = 1;  // dB
constexpr pulp::state::ParamID kCompRatio = 2;      // ratio

// Drive a real PulpCompressor through HeadlessHost, one `dsp.node` span per
// block. The loud sine keeps the compressor's gain-computation branch busy so
// the span carries real work.
void render_real_compressor_chain() {
    constexpr int kBlock = 512;
    constexpr int kBlocks = 4;

    pulp::format::HeadlessHost comp(&pulp::examples::create_pulp_compressor);
    comp.prepare(48000.0, kBlock, 2, 2);
    comp.state().set_value(kCompThreshold, -35.0f);
    comp.state().set_value(kCompRatio, 12.0f);

    pulp::audio::Buffer<float> in(2, kBlock);
    pulp::audio::Buffer<float> out(2, kBlock);
    const pulp::audio::Buffer<float>& in_c = in;

    double phase = 0.0;
    const double inc = 2.0 * std::numbers::pi * 220.0 / 48000.0;
    for (int b = 0; b < kBlocks; ++b) {
        for (int i = 0; i < kBlock; ++i) {
            const float s = 0.7f * static_cast<float>(std::sin(phase));
            in.channel(0)[i] = s;
            in.channel(1)[i] = s;
            phase += inc;
            if (phase > 2.0 * std::numbers::pi) phase -= 2.0 * std::numbers::pi;
        }
        auto out_v = out.view();
        auto in_v = in_c.view();
        PULP_TRACE_SCOPE_NAMED("dsp.node", "compressor");
        comp.process(out_v, in_v);
    }
}

}  // namespace

TEST_CASE("a real Processor under HeadlessHost emits a named dsp.node span",
          "[tracing][audio][offline][format]") {
    if (!pulp::runtime::kTracingEnabled) {
        // OFF contract: start() no-ops, the render still runs with the span
        // macros compiled out.
        REQUIRE_FALSE(Tracing::start({"dsp", "dsp.node"}));
        REQUIRE_FALSE(Tracing::active());
        render_real_compressor_chain();
        REQUIRE_FALSE(Tracing::stop().ok);
        return;
    }

    auto path =
        std::filesystem::temp_directory_path() / "pulp-offline-plugin-trace.pftrace";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    REQUIRE(Tracing::start({"dsp", "dsp.node"}, path.string(), /*ring_kb=*/8192));
    REQUIRE(Tracing::active());

    render_real_compressor_chain();

    auto r = Tracing::stop();
    REQUIRE(r.ok);
    REQUIRE(r.trace_bytes > 0);
    REQUIRE_FALSE(Tracing::active());

    std::ifstream f(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    REQUIRE_FALSE(bytes.empty());
    // The real plugin's per-block span name is interned in the trace stream.
    REQUIRE(bytes.find("compressor") != std::string::npos);
}
