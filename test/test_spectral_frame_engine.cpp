// SpectralFrameEngine — DSP STFT/WOLA engine tests.
//
// Covers the acceptance gates from the engine's design record: neutral
// reconstruction below -100 dBFS, COLA flatness, block-size invariance,
// coherent channel grouping (1..16), frame cadence, exact reported
// latency, no allocation after prepare(), reset determinism, and the
// variable-synthesis-hop split API used by time-scale processors.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/spectral_frame_engine.hpp>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <new>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

// ── allocation sentinel ─────────────────────────────────────────────────────
// Replaces global operator new/delete for this test binary so realtime-path
// tests can assert that process() performs no heap allocation. External
// linkage: sibling TUs in this suite reference it via extern declaration.

std::atomic<long> g_alloc_count{0};

void* operator new(std::size_t size) {
    ++g_alloc_count;
    if (void* p = std::malloc(size)) return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) {
    ++g_alloc_count;
    if (void* p = std::malloc(size)) return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<std::vector<float>> make_tone_channels(int channels, int length) {
    std::vector<std::vector<float>> chans(static_cast<size_t>(channels));
    for (auto& c : chans) {
        c.resize(static_cast<size_t>(length));
        for (int i = 0; i < length; ++i)
            c[static_cast<size_t>(i)] =
                0.5f * std::sin(static_cast<float>(2.0 * kPi * 440.0 * i / 48000.0))
                + 0.2f * std::sin(static_cast<float>(2.0 * kPi * 1237.0 * i / 48000.0 + 0.7));
    }
    return chans;
}

std::vector<float*> ptrs(std::vector<std::vector<float>>& v) {
    std::vector<float*> p;
    for (auto& c : v) p.push_back(c.data());
    return p;
}

std::vector<const float*> cptrs(const std::vector<std::vector<float>>& v) {
    std::vector<const float*> p;
    for (auto& c : v) p.push_back(c.data());
    return p;
}

// Run the engine over `in` with an identity frame callback, block size B.
std::vector<std::vector<float>> run_identity(SpectralFrameEngine& engine,
                                             const std::vector<std::vector<float>>& in,
                                             int block) {
    const int channels = static_cast<int>(in.size());
    const int length = static_cast<int>(in[0].size());
    std::vector<std::vector<float>> out(static_cast<size_t>(channels),
                                        std::vector<float>(static_cast<size_t>(length), 0.0f));
    auto in_ptrs = cptrs(in);
    auto out_ptrs = ptrs(out);
    std::vector<const float*> ip(in_ptrs.size());
    std::vector<float*> op(out_ptrs.size());
    for (int pos = 0; pos < length; pos += block) {
        const int n = std::min(block, length - pos);
        for (size_t ch = 0; ch < ip.size(); ++ch) {
            ip[ch] = in_ptrs[ch] + pos;
            op[ch] = out_ptrs[ch] + pos;
        }
        engine.process(ip.data(), op.data(), n,
                       [](std::complex<float>* const*, int) {});
    }
    return out;
}

double null_depth_db(const std::vector<float>& out, const std::vector<float>& in,
                     int latency, int skip, int tail) {
    double err = 0.0, ref = 0.0;
    int count = 0;
    for (int i = skip; i + latency + tail < static_cast<int>(out.size()); ++i) {
        const double d = static_cast<double>(out[static_cast<size_t>(i + latency)])
                       - static_cast<double>(in[static_cast<size_t>(i)]);
        err += d * d;
        ref += static_cast<double>(in[static_cast<size_t>(i)]) * in[static_cast<size_t>(i)];
        ++count;
    }
    REQUIRE(count > 0);
    return 10.0 * std::log10(err / ref + 1e-300);
}

} // namespace

TEST_CASE("SpectralFrameEngine neutral reconstruction below -100 dBFS",
          "[signal][spectral-frame-engine]") {
    SpectralFrameEngineConfig config;
    config.fft_size = 2048;
    config.analysis_hop = 512;
    config.channels = 1;
    SpectralFrameEngine engine;
    engine.prepare(config);

    auto in = make_tone_channels(1, 96000);
    auto out = run_identity(engine, in, 512);

    const double depth = null_depth_db(out[0], in[0], engine.latency_samples(), 4096, 4096);
    INFO("null depth: " << depth << " dB");
    REQUIRE(depth < -100.0);
}

TEST_CASE("SpectralFrameEngine tapers stream edges instead of spiking",
          "[signal][spectral-frame-engine][issue-3975]") {
    // Regression for #3975: the OLA normalization used to divide
    // partial-overlap samples at the very start of the stream by a
    // near-zero coverage, producing a full-scale spike in the first
    // ~fft_size/hop samples. The whole output — including sample 0 —
    // must stay within a small per-sample step for a sustained tone.
    SpectralFrameEngineConfig config;
    config.fft_size = 2048;
    config.analysis_hop = 512;
    SpectralFrameEngine engine;
    engine.prepare(config);

    auto in = make_tone_channels(1, 48000);
    auto out = run_identity(engine, in, 480); // non-hop-aligned blocks

    float max_step = 0.0f, peak = 0.0f;
    for (size_t i = 1; i < out[0].size(); ++i) {
        max_step = std::max(max_step, std::abs(out[0][i] - out[0][i - 1]));
        peak = std::max(peak, std::abs(out[0][i]));
    }
    INFO("whole-stream max step: " << max_step << ", peak: " << peak);
    // A 440+1237 Hz tone at 0.7 amp has per-sample steps well under 0.2;
    // the old edge spike was ~2.0 (full-scale). Peak must not exceed the
    // input amplitude envelope.
    REQUIRE(max_step < 0.3f);
    REQUIRE(peak < 1.0f);
}

TEST_CASE("SpectralFrameEngine preserves non-default steady-state body normalization",
          "[signal][spectral-frame-engine][issue-3975]") {
    for (auto window : {WindowFunction::Type::blackman, WindowFunction::Type::flat_top}) {
        SpectralFrameEngineConfig config;
        config.fft_size = 1024;
        config.analysis_hop = 512;
        config.window = window;
        SpectralFrameEngine engine;
        engine.prepare(config);

        std::vector<std::vector<float>> in(1, std::vector<float>(32768, 1.0f));
        auto out = run_identity(engine, in, 512);

        float lo = 1e9f, hi = -1e9f;
        for (int i = 8192; i < 24576; ++i) {
            lo = std::min(lo, out[0][static_cast<size_t>(i)]);
            hi = std::max(hi, out[0][static_cast<size_t>(i)]);
        }
        INFO("window index: " << static_cast<int>(window));
        REQUIRE_THAT(lo, WithinAbs(1.0f, 1e-3f));
        REQUIRE_THAT(hi, WithinAbs(1.0f, 1e-3f));
    }
}

TEST_CASE("SpectralFrameEngine COLA flatness on DC", "[signal][spectral-frame-engine]") {
    SpectralFrameEngineConfig config;
    config.fft_size = 1024;
    config.analysis_hop = 256;
    SpectralFrameEngine engine;
    engine.prepare(config);

    std::vector<std::vector<float>> in(1, std::vector<float>(32768, 1.0f));
    auto out = run_identity(engine, in, 256);

    float lo = 1e9f, hi = -1e9f;
    for (int i = 8192; i < 24576; ++i) {
        lo = std::min(lo, out[0][static_cast<size_t>(i)]);
        hi = std::max(hi, out[0][static_cast<size_t>(i)]);
    }
    REQUIRE_THAT(lo, WithinAbs(1.0f, 1e-4f));
    REQUIRE_THAT(hi, WithinAbs(1.0f, 1e-4f));
}

TEST_CASE("SpectralFrameEngine output is block-size invariant",
          "[signal][spectral-frame-engine]") {
    auto in = make_tone_channels(1, 48000);
    SpectralFrameEngineConfig config;
    config.fft_size = 2048;
    config.analysis_hop = 512;

    SpectralFrameEngine reference_engine;
    reference_engine.prepare(config);
    auto reference = run_identity(reference_engine, in, 4096);

    // 480 deliberately not a divisor/multiple of the hop: frames must
    // still land at fft_size + k * hop for any feed chunking.
    for (int block : {32, 480, 512, 1000}) {
        SpectralFrameEngine engine;
        engine.prepare(config);
        auto out = run_identity(engine, in, block);
        float max_diff = 0.0f;
        for (size_t i = 0; i < out[0].size(); ++i)
            max_diff = std::max(max_diff, std::abs(out[0][i] - reference[0][i]));
        INFO("block: " << block);
        REQUIRE(max_diff == 0.0f);
    }
}

TEST_CASE("SpectralFrameEngine keeps identical channels identical for 1..16 channels",
          "[signal][spectral-frame-engine]") {
    for (int channels : {1, 2, 4, 8, 16}) {
        SpectralFrameEngineConfig config;
        config.fft_size = 1024;
        config.analysis_hop = 256;
        config.channels = channels;
        SpectralFrameEngine engine;
        engine.prepare(config);

        auto in = make_tone_channels(channels, 16384);
        auto out = run_identity(engine, in, 512);

        float max_diff = 0.0f;
        for (int ch = 1; ch < channels; ++ch)
            for (size_t i = 0; i < out[0].size(); ++i)
                max_diff = std::max(max_diff, std::abs(out[static_cast<size_t>(ch)][i] - out[0][i]));
        INFO("channels: " << channels);
        REQUIRE(max_diff == 0.0f);
    }
}

TEST_CASE("SpectralFrameEngine frame cadence and bin count",
          "[signal][spectral-frame-engine]") {
    SpectralFrameEngineConfig config;
    config.fft_size = 1024;
    config.analysis_hop = 256;
    SpectralFrameEngine engine;
    engine.prepare(config);

    REQUIRE(engine.num_bins() == 513);

    std::vector<std::vector<float>> in(1, std::vector<float>(10240, 0.25f));
    auto in_ptr = cptrs(in);
    int frames = 0;
    int bins_seen = 0;
    engine.analyze(in_ptr.data(), 10240,
                   [&](std::complex<float>* const*, int bins) {
                       ++frames;
                       bins_seen = bins;
                   });
    // First frame at sample 1024, then every 256: 1 + (10240 - 1024) / 256.
    REQUIRE(frames == 37);
    REQUIRE(bins_seen == 513);
}

TEST_CASE("SpectralFrameEngine64 analyzes and synthesizes double frames",
          "[signal][spectral-frame-engine][f64]") {
    SpectralFrameEngineConfig config;
    config.fft_size = 256;
    config.analysis_hop = 64;
    config.max_block = 64;
    SpectralFrameEngine64 engine;
    engine.prepare(config);

    std::vector<double> input(512, 0.0);
    std::vector<double> output(512, 0.0);
    input[0] = 1.0;
    const double* in_ptrs[1] = {input.data()};
    double* out_ptrs[1] = {output.data()};

    int frames = 0;
    int bins_seen = 0;
    for (int pos = 0; pos < static_cast<int>(input.size()); pos += 64) {
        in_ptrs[0] = input.data() + pos;
        out_ptrs[0] = output.data() + pos;
        engine.process(in_ptrs, out_ptrs, 64,
                       [&](std::complex<double>* const* frames_ptr, int bins) {
                           ++frames;
                           bins_seen = bins;
                           frames_ptr[0][0] += std::complex<double>{0.0, 0.0};
                       });
    }

    REQUIRE(frames > 0);
    REQUIRE(bins_seen == 129);
    for (double sample : output)
        REQUIRE(std::isfinite(sample));
}

TEST_CASE("SpectralFrameEngine measured impulse latency matches latency_samples",
          "[signal][spectral-frame-engine]") {
    SpectralFrameEngineConfig config;
    config.fft_size = 2048;
    config.analysis_hop = 512;
    SpectralFrameEngine engine;
    engine.prepare(config);

    const int impulse_at = 5000;
    std::vector<std::vector<float>> in(1, std::vector<float>(16384, 0.0f));
    in[0][impulse_at] = 1.0f;
    auto out = run_identity(engine, in, 512);

    int peak_index = 0;
    float peak = 0.0f;
    for (size_t i = 0; i < out[0].size(); ++i)
        if (std::abs(out[0][i]) > peak) {
            peak = std::abs(out[0][i]);
            peak_index = static_cast<int>(i);
        }
    REQUIRE(peak_index == impulse_at + engine.latency_samples());
    REQUIRE_THAT(peak, WithinAbs(1.0f, 1e-3f));
}

TEST_CASE("SpectralFrameEngine process allocates nothing after prepare",
          "[signal][spectral-frame-engine]") {
    SpectralFrameEngineConfig config;
    config.fft_size = 2048;
    config.analysis_hop = 512;
    config.channels = 2;
    SpectralFrameEngine engine;
    engine.prepare(config);

    auto in = make_tone_channels(2, 8192);
    std::vector<std::vector<float>> out(2, std::vector<float>(8192, 0.0f));
    auto in_ptr = cptrs(in);
    auto out_ptr = ptrs(out);

    // Warm up (covers any lazy first-call work), then count.
    engine.process(in_ptr.data(), out_ptr.data(), 4096,
                   [](std::complex<float>* const*, int) {});
    const long before = g_alloc_count.load();
    engine.process(in_ptr.data(), out_ptr.data(), 4096,
                   [](std::complex<float>* const*, int) {});
    const long after = g_alloc_count.load();
    REQUIRE(after == before);
}

TEST_CASE("SpectralFrameEngine reset restores deterministic state",
          "[signal][spectral-frame-engine]") {
    SpectralFrameEngineConfig config;
    config.fft_size = 1024;
    config.analysis_hop = 256;
    SpectralFrameEngine engine;
    engine.prepare(config);

    auto in = make_tone_channels(1, 8192);
    auto first = run_identity(engine, in, 256);
    engine.reset();
    auto second = run_identity(engine, in, 256);

    float max_diff = 0.0f;
    for (size_t i = 0; i < first[0].size(); ++i)
        max_diff = std::max(max_diff, std::abs(first[0][i] - second[0][i]));
    REQUIRE(max_diff == 0.0f);
}

TEST_CASE("SpectralFrameEngine split API stretches DC cleanly at a larger synthesis hop",
          "[signal][spectral-frame-engine]") {
    SpectralFrameEngineConfig config;
    config.fft_size = 1024;
    config.analysis_hop = 256;
    config.max_synthesis_hop = 512;
    SpectralFrameEngine engine;
    engine.prepare(config);

    const int synthesis_hop = 384; // 1.5x time stretch
    std::vector<std::vector<float>> in(1, std::vector<float>(32768, 1.0f));
    auto in_ptr = cptrs(in);
    std::vector<float> collected;
    collected.reserve(65536);

    std::vector<float> chunk(1024, 0.0f);
    float* chunk_ptr[1] = {chunk.data()};
    engine.analyze(in_ptr.data(), 32768,
                   [&](std::complex<float>* const* frames, int) {
                       engine.synthesize_frame(frames, synthesis_hop);
                       while (engine.available_output() >= 1024) {
                           engine.read_output(chunk_ptr, 1024);
                           collected.insert(collected.end(), chunk.begin(), chunk.end());
                       }
                   });

    // 1.5x stretch: collected output length tracks 1.5x the input length.
    REQUIRE(static_cast<int>(collected.size()) > 40000);

    float lo = 1e9f, hi = -1e9f;
    for (size_t i = 8192; i < collected.size() - 4096; ++i) {
        lo = std::min(lo, collected[i]);
        hi = std::max(hi, collected[i]);
    }
    // Per-sample window-energy normalization keeps DC flat even though
    // Hann^2 at this hop is not COLA by itself.
    REQUIRE_THAT(lo, WithinAbs(1.0f, 1e-3f));
    REQUIRE_THAT(hi, WithinAbs(1.0f, 1e-3f));
}
