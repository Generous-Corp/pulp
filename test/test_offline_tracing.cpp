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

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <pulp/audio/offline_processor.hpp>
#include <pulp/runtime/trace.hpp>
#include <pulp/runtime/trace_session.hpp>

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
