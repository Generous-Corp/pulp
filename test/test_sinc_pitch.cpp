// Sinc resampling in RealtimePitchTimeProcessor — pitch-shifting a bright
// tone aliases less with the windowed-sinc reader than with Catmull-Rom.

#include <catch2/catch_test_macros.hpp>
#include <pulp/signal/realtime_pitch_time_processor.hpp>
#include <cmath>
#include <vector>

using namespace pulp::signal;

namespace {
constexpr double kPi = 3.14159265358979323846;

// Drive realtime_pitch process() in <= max_block chunks (process() asserts
// num_samples <= max_block, which fires in asserts-enabled builds).
void process_chunked(RealtimePitchTimeProcessor& proc, const float* in, float* out,
                     int n, int block) {
    for (int pos = 0; pos < n; pos += block) {
        const int m = std::min(block, n - pos);
        const float* ip[1] = {in + pos};
        float* op[1] = {out + pos};
        proc.process(ip, op, m);
    }
}

std::vector<float> bright_sine(double cyc_per_sample, int n) {
    std::vector<float> v(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        v[static_cast<size_t>(i)] = 0.6f * static_cast<float>(std::sin(2.0 * kPi * cyc_per_sample * i));
    return v;
}

// Magnitude at a normalized frequency (cycles/sample) via Goertzel.
double goertzel(const std::vector<float>& x, int from, int len, double cyc) {
    const double w = 2.0 * kPi * cyc, c = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (int i = from; i < from + len; ++i) {
        const double s0 = x[static_cast<size_t>(i)] + c * s1 - s2;
        s2 = s1; s1 = s0;
    }
    const double re = s1 - s2 * std::cos(w), im = s2 * std::sin(w);
    return std::sqrt(re * re + im * im);
}

// Process a bright tone up +7 st and return spurious-to-total energy ratio
// (lower = cleaner). Measures everything outside a narrow band around the
// shifted fundamental as spurious (alias) energy.
double alias_ratio(bool sinc) {
    RealtimePitchTimeConfig cfg;
    cfg.mode = PitchTimeMode::realtime_pitch;
    cfg.quality = PitchTimeQuality::quality;
    cfg.channels = 1;
    cfg.max_block = 512;
    cfg.sinc_resampling = sinc;
    RealtimePitchTimeProcessor proc;
    proc.prepare(48000.0, cfg);
    proc.set_pitch_semitones(7.0f);

    const double f0 = 0.135;               // bright input tone (cycles/sample)
    auto in = bright_sine(f0, 48000);
    std::vector<float> out(48000, 0.0f);
    process_chunked(proc, in.data(), out.data(), 48000, 512);

    const int from = 16000, len = 16000;   // settled region
    const double shifted = f0 * std::exp2(7.0 / 12.0);
    double total = 0.0;
    for (int i = from; i < from + len; ++i)
        total += static_cast<double>(out[static_cast<size_t>(i)]) * out[static_cast<size_t>(i)];
    const double fund = goertzel(out, from, len, shifted);
    const double fund_e = fund * fund / len; // ~ power at the fundamental
    return std::max(0.0, (total - fund_e)) / (total + 1e-12);
}
} // namespace

TEST_CASE("Sinc resampling is no worse than cubic through the pitch path",
          "[signal][sinc-pitch]") {
    // The windowed-sinc reader's decisive quality win is at the primitive
    // level (see test_sinc_resampler — under a fifth of linear's error).
    // Through the full phase vocoder the resampled stream is already
    // band-limited, so the end-to-end difference is small; this guards that
    // enabling sinc never regresses cleanliness vs cubic.
    const double cubic = alias_ratio(/*sinc=*/false);
    const double sinc = alias_ratio(/*sinc=*/true);
    INFO("alias ratio  cubic=" << cubic << "  sinc=" << sinc);
    REQUIRE(sinc <= cubic + 1e-3);
}

TEST_CASE("Sinc resampling preserves the shifted pitch + finite output",
          "[signal][sinc-pitch]") {
    RealtimePitchTimeConfig cfg;
    cfg.mode = PitchTimeMode::realtime_pitch;
    cfg.channels = 1;
    cfg.max_block = 512;
    cfg.sinc_resampling = true;
    RealtimePitchTimeProcessor proc;
    proc.prepare(48000.0, cfg);
    proc.set_pitch_semitones(7.0f);
    auto in = bright_sine(0.05, 48000);
    std::vector<float> out(48000, 0.0f);
    process_chunked(proc, in.data(), out.data(), 48000, 512);
    bool finite = true;
    float peak = 0.0f;
    for (float v : out) { finite = finite && std::isfinite(v); peak = std::max(peak, std::abs(v)); }
    REQUIRE(finite);
    REQUIRE(peak > 0.1f);
    REQUIRE(peak < 2.0f);
    const double shifted = 0.05 * std::exp2(7.0 / 12.0);
    const double at_shifted = goertzel(out, 16000, 16000, shifted);
    const double at_orig = goertzel(out, 16000, 16000, 0.05);
    REQUIRE(at_shifted > at_orig * 4.0); // energy moved to the shifted pitch
}
