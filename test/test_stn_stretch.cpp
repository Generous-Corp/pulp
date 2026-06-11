// Integration: noise-morphing path in RealtimePitchTimeProcessor.
//
// Verifies that enabling config.noise_morphing (a) leaves a pure tone
// intact (the tonal path is untouched) and (b) time-stretched noise is
// less "tonalized" (lower off-lag autocorrelation) than the plain
// phase-vocoder path — the artifact noise morphing is designed to fix.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/realtime_pitch_time_processor.hpp>
#include <cmath>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

namespace {
constexpr double kSr = 48000.0;

std::vector<float> sine(double hz, float amp, int n) {
    std::vector<float> v(static_cast<size_t>(n));
    const double w = 2.0 * 3.14159265358979323846 * hz / kSr;
    for (int i = 0; i < n; ++i) v[static_cast<size_t>(i)] = amp * static_cast<float>(std::sin(w * i));
    return v;
}

std::vector<float> white(int n, std::uint32_t seed) {
    std::vector<float> v(static_cast<size_t>(n));
    std::uint32_t s = seed;
    for (int i = 0; i < n; ++i) {
        s = s * 1664525u + 1013904223u;
        v[static_cast<size_t>(i)] = (static_cast<float>(s >> 8) * (1.0f / 8388608.0f) - 1.0f) * 0.3f;
    }
    return v;
}

RealtimePitchTimeConfig stretch_config(bool morph) {
    RealtimePitchTimeConfig c;
    c.mode = PitchTimeMode::time_stretch;
    c.quality = PitchTimeQuality::quality;
    c.channels = 1;
    c.max_block = 4096;
    c.max_time_ratio = 2.0f;
    c.noise_morphing = morph;
    return c;
}

std::vector<float> stretch(const std::vector<float>& input, float ratio, bool morph) {
    RealtimePitchTimeProcessor proc;
    proc.prepare(kSr, stretch_config(morph));
    proc.set_time_ratio(ratio);
    std::vector<float> out;
    out.reserve(static_cast<size_t>(input.size() * ratio + 4096));
    std::vector<float> chunk(1024, 0.0f);
    float* cp[1] = {chunk.data()};
    const float* ip[1];
    const int n = static_cast<int>(input.size());
    for (int pos = 0; pos + 480 <= n; pos += 480) {
        ip[0] = input.data() + pos;
        proc.feed(ip, 480);
        while (proc.available_stretched() >= 1024) {
            proc.read_stretched(cp, 1024);
            out.insert(out.end(), chunk.begin(), chunk.end());
        }
    }
    return out;
}

// Largest |normalized autocorrelation| over a lag band — a tonalization /
// periodicity proxy. White noise → ~0; tonalized noise shows peaks.
float max_offlag_autocorr(const std::vector<float>& y, int min_lag, int max_lag) {
    const int n = static_cast<int>(y.size());
    double energy = 0.0;
    for (int i = 0; i < n; ++i) energy += static_cast<double>(y[static_cast<size_t>(i)]) * y[static_cast<size_t>(i)];
    if (energy < 1e-12) return 0.0f;
    float worst = 0.0f;
    for (int lag = min_lag; lag <= max_lag && lag < n; lag += 1) {
        double acc = 0.0;
        for (int i = 0; i + lag < n; ++i)
            acc += static_cast<double>(y[static_cast<size_t>(i)]) * y[static_cast<size_t>(i + lag)];
        worst = std::max(worst, static_cast<float>(std::abs(acc) / energy));
    }
    return worst;
}
} // namespace

TEST_CASE("Noise morphing leaves a pure tone intact", "[signal][stn-stretch]") {
    auto tone = sine(440.0, 0.7f, 96000);
    auto out = stretch(tone, 1.5f, /*morph=*/true);
    REQUIRE(static_cast<int>(out.size()) > 100000);
    // The tone is tonal content, so the noise mask is ~0 there: output
    // stays bounded and finite (no morphing-injected blowups).
    float peak = 0.0f;
    bool finite = true;
    for (float s : out) { peak = std::max(peak, std::abs(s)); finite = finite && std::isfinite(s); }
    REQUIRE(finite);
    REQUIRE(peak < 1.5f);
    REQUIRE(peak > 0.2f); // the tone survived (not silenced by the split)
}

TEST_CASE("Noise morphing reduces tonalization of stretched noise",
          "[signal][stn-stretch]") {
    auto noise = white(96000, 0xC0FFEEu);
    auto pv = stretch(noise, 2.0f, /*morph=*/false);
    auto morph = stretch(noise, 2.0f, /*morph=*/true);
    REQUIRE(static_cast<int>(pv.size()) > 120000);
    REQUIRE(static_cast<int>(morph.size()) > 120000);

    // Skip the warm-up tail; measure on the steady stretched body.
    auto tail = [](const std::vector<float>& v) {
        return std::vector<float>(v.begin() + 20000, v.begin() + 20000 + 60000);
    };
    const float pv_tonal = max_offlag_autocorr(tail(pv), 64, 2500);
    const float morph_tonal = max_offlag_autocorr(tail(morph), 64, 2500);
    INFO("PV off-lag autocorr=" << pv_tonal << "  morph=" << morph_tonal);
    // Morphing decorrelates frame to frame, so the periodicity the phase
    // vocoder imposes on stretched noise is reduced.
    REQUIRE(morph_tonal < pv_tonal);
}

TEST_CASE("Noise morphing is bypassed at unity (bit-identical to baseline)",
          "[signal][stn-stretch]") {
    auto noise = white(48000, 0x5EEDu);
    // At ratio 1.0 there is no scaling, so morphing must not engage: the
    // output is exactly the non-morphing path.
    auto plain = stretch(noise, 1.0f, /*morph=*/false);
    auto morph = stretch(noise, 1.0f, /*morph=*/true);
    REQUIRE(plain.size() == morph.size());
    float max_diff = 0.0f;
    for (size_t i = 0; i < plain.size(); ++i)
        max_diff = std::max(max_diff, std::abs(plain[i] - morph[i]));
    REQUIRE(max_diff == 0.0f);
}

TEST_CASE("Noise morphing keeps stereo lanes coherent for identical input",
          "[signal][stn-stretch]") {
    // Shared morpher seeds → identical channels in == identical channels out,
    // even under shift (the noise phase is coherent across channels).
    RealtimePitchTimeProcessor proc;
    auto cfg = stretch_config(/*morph=*/true);
    cfg.channels = 2;
    proc.prepare(kSr, cfg);
    proc.set_time_ratio(1.5f);
    auto lane = white(48000, 0xA11CEu);
    std::vector<float> outL, outR;
    std::vector<float> chunk(1024 * 2, 0.0f);
    float* cp[2] = {chunk.data(), chunk.data() + 1024};
    const float* ip[2];
    for (int pos = 0; pos + 480 <= 48000; pos += 480) {
        ip[0] = lane.data() + pos;
        ip[1] = lane.data() + pos;
        proc.feed(ip, 480);
        while (proc.available_stretched() >= 1024) {
            proc.read_stretched(cp, 1024);
            outL.insert(outL.end(), chunk.begin(), chunk.begin() + 1024);
            outR.insert(outR.end(), chunk.begin() + 1024, chunk.end());
        }
    }
    REQUIRE(outL.size() > 40000);
    float max_diff = 0.0f;
    for (size_t i = 0; i < outL.size(); ++i)
        max_diff = std::max(max_diff, std::abs(outL[i] - outR[i]));
    REQUIRE(max_diff == 0.0f);
}

TEST_CASE("Noise morphing output is finite and bounded", "[signal][stn-stretch]") {
    auto noise = white(48000, 0x1234u);
    auto out = stretch(noise, 2.0f, /*morph=*/true);
    REQUIRE(static_cast<int>(out.size()) > 60000);
    for (float s : out) REQUIRE(std::isfinite(s));
    float peak = 0.0f;
    for (float s : out) peak = std::max(peak, std::abs(s));
    REQUIRE(peak < 2.0f);
}
