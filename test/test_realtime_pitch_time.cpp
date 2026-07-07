// RealtimePitchTimeProcessor / MultichannelPhaseCoordinator /
// SpectralEnvelopeShifter / LatencyAwareControlSmoother — Phase-3
// spectral-primitive tests.
//
// Acceptance gates from the design records: steady-sine pitch accuracy
// within +/-2 cents across shifts, neutral transparency, exact reported
// latency, multichannel identity and stereo phase preservation, finite
// output under automation ramps, independent time stretching that leaves
// pitch untouched, formant preservation under pitch shift, exact formant
// bypass, control-smoother block invariance, and the no-alloc sentinel.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/latency_aware_control_smoother.hpp>
#include <pulp/signal/multichannel_phase_coordinator.hpp>
#include <pulp/signal/realtime_pitch_time_processor.hpp>
#include <pulp/signal/spectral_envelope_shifter.hpp>
#include <atomic>
#include <cmath>
#include <complex>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

extern std::atomic<long> g_alloc_count; // sentinel defined in test_spectral_frame_engine.cpp

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSr = 48000.0;

std::vector<float> sine(double freq, double amp, int length, double phase = 0.0) {
    std::vector<float> v(static_cast<size_t>(length));
    for (int i = 0; i < length; ++i)
        v[static_cast<size_t>(i)] =
            static_cast<float>(amp * std::sin(2.0 * kPi * freq * i / kSr + phase));
    return v;
}

// FFT-peak frequency with parabolic interpolation over a Hann-windowed
// 65536-point transform of the middle of `x`.
double measure_peak_hz(const std::vector<float>& x, int start, double lo_hz = 0.0,
                       double hi_hz = 24000.0) {
    constexpr int n = 65536;
    REQUIRE(start + n <= static_cast<int>(x.size()));
    Fft fft(n);
    std::vector<std::complex<float>> buf(n);
    for (int i = 0; i < n; ++i) {
        const float w = 0.5f - 0.5f * static_cast<float>(std::cos(2.0 * kPi * i / n));
        buf[static_cast<size_t>(i)] = {x[static_cast<size_t>(start + i)] * w, 0.0f};
    }
    fft.forward(buf.data());
    const int klo = std::max(1, static_cast<int>(lo_hz * n / kSr));
    const int khi = std::min(n / 2 - 2, static_cast<int>(hi_hz * n / kSr));
    int kmax = klo;
    double mmax = 0.0;
    for (int k = klo; k <= khi; ++k) {
        const double m = std::abs(buf[static_cast<size_t>(k)]);
        if (m > mmax) { mmax = m; kmax = k; }
    }
    const double m0 = std::abs(buf[static_cast<size_t>(kmax - 1)]);
    const double m1 = std::abs(buf[static_cast<size_t>(kmax)]);
    const double m2 = std::abs(buf[static_cast<size_t>(kmax + 1)]);
    const double denom = m0 - 2.0 * m1 + m2;
    const double delta = std::abs(denom) > 1e-12 ? 0.5 * (m0 - m2) / denom : 0.0;
    return (kmax + delta) * kSr / n;
}

// Spectral argmax of a ~150 Hz moving-average-smoothed magnitude
// spectrum inside [lo_hz, hi_hz] — a crude formant-peak locator.
double smoothed_band_peak_hz(const std::vector<float>& x, int start, double lo_hz, double hi_hz) {
    constexpr int n = 16384;
    REQUIRE(start + n <= static_cast<int>(x.size()));
    Fft fft(n);
    std::vector<std::complex<float>> buf(n);
    for (int i = 0; i < n; ++i) {
        const float w = 0.5f - 0.5f * static_cast<float>(std::cos(2.0 * kPi * i / n));
        buf[static_cast<size_t>(i)] = {x[static_cast<size_t>(start + i)] * w, 0.0f};
    }
    fft.forward(buf.data());
    std::vector<double> mag(n / 2);
    for (int k = 0; k < n / 2; ++k) mag[static_cast<size_t>(k)] = std::abs(buf[static_cast<size_t>(k)]);
    const int half_width = static_cast<int>(75.0 * n / kSr); // ~150 Hz window
    const int klo = static_cast<int>(lo_hz * n / kSr);
    const int khi = static_cast<int>(hi_hz * n / kSr);
    int best_k = klo;
    double best = -1.0;
    for (int k = klo; k <= khi; ++k) {
        double acc = 0.0;
        for (int j = std::max(0, k - half_width); j <= std::min(n / 2 - 1, k + half_width); ++j)
            acc += mag[static_cast<size_t>(j)];
        if (acc > best) { best = acc; best_k = k; }
    }
    return best_k * kSr / n;
}

// Synthetic vowel: harmonics of f0 with a two-bump (700 Hz / 1800 Hz)
// log-amplitude envelope — a source-filter fixture for formant tests.
std::vector<float> vowel(double f0, int length) {
    std::vector<float> v(static_cast<size_t>(length), 0.0f);
    auto env_db = [](double f) {
        const double b1 = (f - 700.0) / 220.0;
        const double b2 = (f - 1800.0) / 320.0;
        return 18.0 * std::exp(-b1 * b1) + 14.0 * std::exp(-b2 * b2) - 30.0 - f / 400.0;
    };
    for (int h = 1; h * f0 < 8000.0; ++h) {
        const double f = h * f0;
        const double amp = std::pow(10.0, env_db(f) / 20.0);
        for (int i = 0; i < length; ++i)
            v[static_cast<size_t>(i)] +=
                static_cast<float>(amp * std::sin(2.0 * kPi * f * i / kSr + 0.37 * h));
    }
    return v;
}

std::vector<std::vector<float>> run_pitch(RealtimePitchTimeProcessor& proc,
                                          const std::vector<std::vector<float>>& in,
                                          int block) {
    const int channels = static_cast<int>(in.size());
    const int length = static_cast<int>(in[0].size());
    std::vector<std::vector<float>> out(static_cast<size_t>(channels),
                                        std::vector<float>(static_cast<size_t>(length), 0.0f));
    std::vector<const float*> ip(static_cast<size_t>(channels));
    std::vector<float*> op(static_cast<size_t>(channels));
    for (int pos = 0; pos < length; pos += block) {
        const int n = std::min(block, length - pos);
        for (int ch = 0; ch < channels; ++ch) {
            ip[static_cast<size_t>(ch)] = in[static_cast<size_t>(ch)].data() + pos;
            op[static_cast<size_t>(ch)] = out[static_cast<size_t>(ch)].data() + pos;
        }
        proc.process(ip.data(), op.data(), n);
    }
    return out;
}

RealtimePitchTimeConfig quality_config(int channels = 1) {
    RealtimePitchTimeConfig config;
    config.quality = PitchTimeQuality::quality;
    config.channels = channels;
    config.max_block = 4096;
    return config;
}

} // namespace

TEST_CASE("RealtimePitchTimeProcessor steady-sine pitch accuracy within 2 cents",
          "[signal][pitch-time]") {
    for (const float semitones : {1.0f, 7.0f, -5.0f, 12.0f}) {
        RealtimePitchTimeProcessor proc;
        proc.prepare(kSr, quality_config());
        proc.set_pitch_semitones(semitones);
        proc.reset(); // settle the smoother at the target immediately

        std::vector<std::vector<float>> in{sine(440.0, 0.7, 144000)};
        auto out = run_pitch(proc, in, 512);

        const double expected = 440.0 * std::exp2(static_cast<double>(semitones) / 12.0);
        const double measured =
            measure_peak_hz(out[0], 40000, expected * 0.9, expected * 1.1);
        const double cents = 1200.0 * std::log2(measured / expected);
        INFO("shift " << semitones << " st: measured " << measured << " Hz, "
                      << cents << " cents");
        REQUIRE(std::abs(cents) < 2.0);
    }
}

TEST_CASE("RealtimePitchTimeProcessor neutral settings are transparent",
          "[signal][pitch-time]") {
    RealtimePitchTimeProcessor proc;
    proc.prepare(kSr, quality_config());
    proc.set_pitch_semitones(0.0f);

    std::vector<std::vector<float>> in{sine(440.0, 0.5, 96000)};
    for (int i = 0; i < 96000; ++i)
        in[0][static_cast<size_t>(i)] +=
            static_cast<float>(0.2 * std::sin(2.0 * kPi * 1237.0 * i / kSr + 0.7));
    auto out = run_pitch(proc, in, 512);

    const int latency = proc.latency_samples();
    double err = 0.0, ref = 0.0;
    for (int i = 8192; i + latency + 4096 < 96000; ++i) {
        const double d = static_cast<double>(out[0][static_cast<size_t>(i + latency)])
                       - static_cast<double>(in[0][static_cast<size_t>(i)]);
        err += d * d;
        ref += static_cast<double>(in[0][static_cast<size_t>(i)]) * in[0][static_cast<size_t>(i)];
    }
    const double depth = 10.0 * std::log10(err / ref + 1e-300);
    INFO("neutral null depth: " << depth << " dB");
    REQUIRE(depth < -90.0);
}

TEST_CASE("RealtimePitchTimeProcessor impulse latency matches latency_samples at unity",
          "[signal][pitch-time]") {
    RealtimePitchTimeProcessor proc;
    proc.prepare(kSr, quality_config());
    proc.set_pitch_semitones(0.0f);

    const int impulse_at = 9000;
    std::vector<std::vector<float>> in{std::vector<float>(32768, 0.0f)};
    in[0][impulse_at] = 1.0f;
    auto out = run_pitch(proc, in, 512);

    int peak_index = 0;
    float peak = 0.0f;
    for (size_t i = 0; i < out[0].size(); ++i)
        if (std::abs(out[0][i]) > peak) {
            peak = std::abs(out[0][i]);
            peak_index = static_cast<int>(i);
        }
    REQUIRE(std::abs(peak_index - (impulse_at + proc.latency_samples())) <= 1);
}

TEST_CASE("RealtimePitchTimeProcessor keeps identical channels identical at +5 st",
          "[signal][pitch-time]") {
    RealtimePitchTimeProcessor proc;
    proc.prepare(kSr, quality_config(4));
    proc.set_pitch_semitones(5.0f);
    proc.reset();

    auto lane = sine(330.0, 0.4, 48000);
    for (int i = 0; i < 48000; ++i)
        lane[static_cast<size_t>(i)] +=
            static_cast<float>(0.15 * std::sin(2.0 * kPi * 991.0 * i / kSr + 1.1));
    std::vector<std::vector<float>> in{lane, lane, lane, lane};
    auto out = run_pitch(proc, in, 512);

    float max_diff = 0.0f;
    for (int ch = 1; ch < 4; ++ch)
        for (size_t i = 0; i < out[0].size(); ++i)
            max_diff = std::max(max_diff, std::abs(out[static_cast<size_t>(ch)][i] - out[0][i]));
    REQUIRE(max_diff == 0.0f);
}

TEST_CASE("RealtimePitchTimeProcessor preserves stereo phase offsets at +4 st",
          "[signal][pitch-time]") {
    RealtimePitchTimeProcessor proc;
    proc.prepare(kSr, quality_config(2));
    proc.set_pitch_semitones(4.0f);
    proc.reset();

    const double phase_offset = 0.9;
    std::vector<std::vector<float>> in{sine(500.0, 0.6, 144000),
                                       sine(500.0, 0.6, 144000, phase_offset)};
    auto out = run_pitch(proc, in, 512);

    // Cross-spectrum phase at the shifted tone.
    constexpr int n = 65536;
    Fft fft(n);
    std::vector<std::complex<float>> fa(n), fb(n);
    for (int i = 0; i < n; ++i) {
        const float w = 0.5f - 0.5f * static_cast<float>(std::cos(2.0 * kPi * i / n));
        fa[static_cast<size_t>(i)] = {out[0][static_cast<size_t>(40000 + i)] * w, 0.0f};
        fb[static_cast<size_t>(i)] = {out[1][static_cast<size_t>(40000 + i)] * w, 0.0f};
    }
    fft.forward(fa.data());
    fft.forward(fb.data());
    int kmax = 1;
    double mmax = 0.0;
    for (int k = 1; k < n / 2; ++k)
        if (std::abs(fa[static_cast<size_t>(k)]) > mmax) {
            mmax = std::abs(fa[static_cast<size_t>(k)]);
            kmax = k;
        }
    const double measured =
        std::arg(fb[static_cast<size_t>(kmax)] * std::conj(fa[static_cast<size_t>(kmax)]));
    double err = measured - phase_offset;
    err -= 2.0 * kPi * std::round(err / (2.0 * kPi));
    INFO("phase offset error: " << err << " rad");
    REQUIRE(std::abs(err) < 0.05);
}

TEST_CASE("RealtimePitchTimeProcessor stays finite and click-free under a pitch ramp",
          "[signal][pitch-time]") {
    RealtimePitchTimeProcessor proc;
    proc.prepare(kSr, quality_config());
    proc.set_pitch_semitones(0.0f);
    proc.reset();

    std::vector<std::vector<float>> in{sine(880.0, 0.7, 96000)};
    std::vector<std::vector<float>> out{std::vector<float>(96000, 0.0f)};
    const float* ip[1];
    float* op[1];
    for (int pos = 0; pos < 96000; pos += 480) {
        // Ramp 0 → +12 st over the first second, then hold.
        proc.set_pitch_semitones(std::min(12.0f, 12.0f * static_cast<float>(pos) / 48000.0f));
        ip[0] = in[0].data() + pos;
        op[0] = out[0].data() + pos;
        proc.process(ip, op, 480);
    }

    float max_step = 0.0f;
    for (int i = proc.latency_samples() + 1; i < 96000; ++i) {
        const float a = out[0][static_cast<size_t>(i)];
        REQUIRE(std::isfinite(a));
        max_step = std::max(max_step, std::abs(a - out[0][static_cast<size_t>(i - 1)]));
    }
    // An 880 Hz..1760 Hz sine at 0.7 amplitude has per-sample steps below
    // ~0.17; a frame-boundary click would approach twice the amplitude.
    INFO("max per-sample step: " << max_step);
    REQUIRE(max_step < 0.5f);
}

TEST_CASE("RealtimePitchTimeProcessor time-stretch changes duration but not pitch",
          "[signal][pitch-time]") {
    RealtimePitchTimeConfig config = quality_config();
    config.mode = PitchTimeMode::time_stretch;
    config.max_time_ratio = 2.0f;
    RealtimePitchTimeProcessor proc;
    proc.prepare(kSr, config);
    proc.set_time_ratio(1.5f);

    std::vector<std::vector<float>> in{sine(440.0, 0.7, 96000)};
    const float* ip[1] = {in[0].data()};
    std::vector<float> collected;
    collected.reserve(160000);
    std::vector<float> chunk(1024, 0.0f);
    float* cp[1] = {chunk.data()};
    for (int pos = 0; pos < 96000; pos += 480) {
        ip[0] = in[0].data() + pos;
        proc.feed(ip, 480);
        while (proc.available_stretched() >= 1024) {
            proc.read_stretched(cp, 1024);
            collected.insert(collected.end(), chunk.begin(), chunk.end());
        }
    }

    REQUIRE_THAT(static_cast<float>(proc.achieved_time_ratio()), WithinAbs(1.5f, 1e-3f));
    // 96000 input → ~144000 stretched (minus the analysis tail).
    REQUIRE(static_cast<int>(collected.size()) > 130000);
    const double f = measure_peak_hz(collected, 40000, 396.0, 484.0);
    const double cents = 1200.0 * std::log2(f / 440.0);
    INFO("stretched tone: " << f << " Hz (" << cents << " cents)");
    REQUIRE(std::abs(cents) < 2.0);
}

TEST_CASE("RealtimePitchTimeProcessor preserves formants when asked to",
          "[signal][pitch-time]") {
    auto in_data = vowel(110.0, 144000);
    std::vector<std::vector<float>> in{in_data};

    auto formant_peak_after = [&](FormantMode mode) {
        RealtimePitchTimeConfig config = quality_config();
        config.formant_mode = mode;
        RealtimePitchTimeProcessor proc;
        proc.prepare(kSr, config);
        proc.set_pitch_semitones(7.0f);
        proc.reset();
        auto out = run_pitch(proc, in, 512);
        return smoothed_band_peak_hz(out[0], 40000, 400.0, 1400.0);
    };

    const double input_peak = smoothed_band_peak_hz(in[0], 40000, 400.0, 1400.0);
    REQUIRE_THAT(input_peak, !WithinAbs(0.0, 1.0)); // sanity: fixture has a peak

    const double preserved = formant_peak_after(FormantMode::preserve);
    const double followed = formant_peak_after(FormantMode::follow);
    INFO("input " << input_peak << " Hz, preserved " << preserved
                  << " Hz, followed " << followed << " Hz");
    // Preserve: first formant stays near its input location.
    REQUIRE(std::abs(preserved - input_peak) < 150.0);
    // Follow: the envelope moves with the pitch (x1.498), well clear of input.
    REQUIRE(followed > input_peak * 1.25);
}

TEST_CASE("RealtimePitchTimeProcessor process allocates nothing after prepare",
          "[signal][pitch-time]") {
    RealtimePitchTimeProcessor proc;
    RealtimePitchTimeConfig config = quality_config(2);
    config.formant_mode = FormantMode::preserve;
    config.max_block = 8192; // the block size this test drives (asserted in process())
    proc.prepare(kSr, config);
    proc.set_pitch_semitones(3.0f);
    proc.set_formant_semitones(-2.0f);
    proc.reset();

    auto lane = sine(330.0, 0.4, 16384);
    std::vector<std::vector<float>> in{lane, lane};
    std::vector<std::vector<float>> out(2, std::vector<float>(16384, 0.0f));
    const float* ip[2] = {in[0].data(), in[1].data()};
    float* op[2] = {out[0].data(), out[1].data()};

    proc.process(ip, op, 8192); // warm-up
    const long before = g_alloc_count.load();
    ip[0] = in[0].data() + 8192;
    ip[1] = in[1].data() + 8192;
    op[0] = out[0].data() + 8192;
    op[1] = out[1].data() + 8192;
    proc.process(ip, op, 8192);
    REQUIRE(g_alloc_count.load() == before);
}

TEST_CASE("RealtimePitchTimeProcessor output is block-size invariant when settled",
          "[signal][pitch-time]") {
    auto make_out = [](int block) {
        RealtimePitchTimeProcessor proc;
        proc.prepare(kSr, quality_config());
        proc.set_pitch_semitones(3.0f);
        proc.reset();
        std::vector<std::vector<float>> in{sine(440.0, 0.6, 48000)};
        return run_pitch(proc, in, block);
    };
    auto a = make_out(256);
    auto b = make_out(2048);
    float max_diff = 0.0f;
    for (size_t i = 0; i < a[0].size(); ++i)
        max_diff = std::max(max_diff, std::abs(a[0][i] - b[0][i]));
    REQUIRE(max_diff < 1e-6f);
}

// ── SpectralEnvelopeShifter ─────────────────────────────────────────────────

TEST_CASE("SpectralEnvelopeShifter warp 1 is an exact bypass", "[signal][envelope]") {
    SpectralEnvelopeShifterConfig config;
    config.fft_size = 1024;
    SpectralEnvelopeShifter shifter;
    shifter.prepare(config);

    std::vector<std::complex<float>> frame(513);
    for (int k = 0; k < 513; ++k)
        frame[static_cast<size_t>(k)] = {0.1f * static_cast<float>(k % 17),
                                         0.05f * static_cast<float>(k % 11)};
    auto copy = frame;
    std::complex<float>* frames[1] = {frame.data()};
    shifter.process_group(frames, 1, 513, 1.0f);
    for (int k = 0; k < 513; ++k)
        REQUIRE(frame[static_cast<size_t>(k)] == copy[static_cast<size_t>(k)]);
}

TEST_CASE("SpectralEnvelopeShifter64 processes double frames", "[signal][envelope][f64]") {
    SpectralEnvelopeShifterConfig config;
    config.fft_size = 1024;
    SpectralEnvelopeShifter64 shifter;
    shifter.prepare(config);

    std::vector<std::complex<double>> frame(513);
    for (int k = 0; k < 513; ++k)
        frame[static_cast<size_t>(k)] = {0.001 * static_cast<double>(k + 1), 0.0};
    std::complex<double>* frames[1] = {frame.data()};
    shifter.process_group(frames, 1, 513, 0.75);
    for (const auto& bin : frame) {
        REQUIRE(std::isfinite(bin.real()));
        REQUIRE(std::isfinite(bin.imag()));
    }
}

TEST_CASE("SpectralEnvelopeShifter moves envelope features by 1/warp", "[signal][envelope]") {
    SpectralEnvelopeShifterConfig config;
    config.fft_size = 2048;
    // The fixture bump is ~30 bins wide (quefrency ~68), so order 128
    // resolves it while staying well below the 8-bin comb's quefrency
    // (2048 / 8 = 256); the sparse comb also needs a deeper
    // true-envelope refinement than the streaming default.
    config.order = 128;
    config.true_envelope_iterations = 16;
    SpectralEnvelopeShifter shifter;
    shifter.prepare(config);

    // Harmonic comb shaped by a single bump around bin 100.
    const int bins = 1025;
    std::vector<std::complex<float>> frame(static_cast<size_t>(bins), {0.0f, 0.0f});
    for (int k = 8; k < bins; k += 8) {
        const double d = (k - 100.0) / 30.0;
        const float amp = static_cast<float>(std::exp(-d * d) + 0.02);
        frame[static_cast<size_t>(k)] = {amp, 0.0f};
    }
    std::complex<float>* frames[1] = {frame.data()};
    shifter.process_group(frames, 1, bins, 0.5f); // features move to 2x frequency

    auto band_energy = [&](int lo, int hi) {
        double e = 0.0;
        for (int k = lo; k <= hi; ++k) e += std::abs(frame[static_cast<size_t>(k)]);
        return e;
    };
    // The bump should now dominate around bin 200, not bin 100.
    REQUIRE(band_energy(170, 230) > 2.0 * band_energy(70, 130));
}

// ── LatencyAwareControlSmoother ─────────────────────────────────────────────

TEST_CASE("LatencyAwareControlSmoother advancement is block-size independent",
          "[signal][control-smoother]") {
    LatencyAwareControlSmoother::Config config;
    config.attack_seconds = 0.05f;
    config.release_seconds = 0.02f;

    LatencyAwareControlSmoother one;
    one.prepare(48000.0, config);
    one.set_target(10.0f);
    one.advance(480);

    LatencyAwareControlSmoother many;
    many.prepare(48000.0, config);
    many.set_target(10.0f);
    for (int i = 0; i < 480; ++i) many.advance(1);

    REQUIRE_THAT(one.current(), WithinAbs(many.current(), 1e-5f));
}

TEST_CASE("LatencyAwareControlSmoother honors asymmetric attack and release",
          "[signal][control-smoother]") {
    LatencyAwareControlSmoother::Config config;
    config.attack_seconds = 0.1f;
    config.release_seconds = 0.01f;
    LatencyAwareControlSmoother smoother;
    smoother.prepare(48000.0, config);

    smoother.set_target(1.0f);
    const float up = smoother.advance(480); // 10 ms up the slow attack
    smoother.set_immediate(1.0f);
    smoother.set_target(0.0f);
    smoother.advance(480);                  // 10 ms down the fast release
    const float down = smoother.current();

    REQUIRE(up < 0.2f);    // slow attack barely moved
    REQUIRE(down < 0.4f);  // fast release nearly settled
    REQUIRE(1.0f - down > up);
}

TEST_CASE("LatencyAwareControlSmoother semitone domain exposes exact ratios",
          "[signal][control-smoother]") {
    LatencyAwareControlSmoother::Config config;
    config.domain = LatencyAwareControlSmoother::Domain::semitone;
    LatencyAwareControlSmoother smoother;
    smoother.prepare(48000.0, config);
    smoother.set_immediate(7.0f);
    REQUIRE_THAT(smoother.ratio_at(0), WithinAbs(std::exp2(7.0f / 12.0f), 1e-6f));

    smoother.set_target(0.0f);
    const float mid_value = smoother.value_at(240);
    const float mid_ratio = smoother.ratio_at(240);
    REQUIRE_THAT(mid_ratio, WithinAbs(std::exp2(mid_value / 12.0f), 1e-6f));
}

TEST_CASE("LatencyAwareControlSmoother value_at rewinds along the trajectory",
          "[signal][control-smoother]") {
    LatencyAwareControlSmoother::Config config;
    config.attack_seconds = 0.05f;
    LatencyAwareControlSmoother smoother;
    smoother.prepare(48000.0, config);
    smoother.set_target(1.0f);
    smoother.advance(960);

    const float behind = smoother.value_at(-480);
    const float now = smoother.current();
    const float ahead = smoother.value_at(480);
    REQUIRE(behind < now);
    REQUIRE(now < ahead);
    REQUIRE(ahead <= 1.0f);
}

// ── MultichannelPhaseCoordinator ────────────────────────────────────────────

TEST_CASE("MultichannelPhaseCoordinator applies one rotation to all channels",
          "[signal][phase-coordinator]") {
    MultichannelPhaseCoordinator coordinator;
    coordinator.prepare(1024, 3);

    const int bins = 513;
    std::vector<std::vector<std::complex<float>>> frames(
        3, std::vector<std::complex<float>>(static_cast<size_t>(bins)));
    auto fill = [&](double phase_shift) {
        for (int ch = 0; ch < 3; ++ch)
            for (int k = 0; k < bins; ++k) {
                const double mag = 1.0 / (1.0 + std::abs(k - 64));
                const double ph = 0.13 * k + 0.5 * ch + phase_shift;
                frames[static_cast<size_t>(ch)][static_cast<size_t>(k)] =
                    {static_cast<float>(mag * std::cos(ph)), static_cast<float>(mag * std::sin(ph))};
            }
    };
    std::complex<float>* ptrs[3] = {frames[0].data(), frames[1].data(), frames[2].data()};

    fill(0.0);
    coordinator.process_group(ptrs, bins, 256, 384);
    fill(0.7);
    auto before = frames; // pre-rotation copy of the second frame
    coordinator.process_group(ptrs, bins, 256, 384);

    // Per-bin rotation must be identical across channels: ratios
    // out/in agree between channels at every bin with signal.
    for (int k = 32; k < 96; ++k) {
        const auto r0 = frames[0][static_cast<size_t>(k)] / before[0][static_cast<size_t>(k)];
        for (int ch = 1; ch < 3; ++ch) {
            const auto rc = frames[static_cast<size_t>(ch)][static_cast<size_t>(k)]
                          / before[static_cast<size_t>(ch)][static_cast<size_t>(k)];
            REQUIRE_THAT(rc.real(), WithinAbs(r0.real(), 1e-4f));
            REQUIRE_THAT(rc.imag(), WithinAbs(r0.imag(), 1e-4f));
        }
    }
}

TEST_CASE("MultichannelPhaseCoordinator64 rotates double channel groups",
          "[signal][phase-coordinator][f64]") {
    MultichannelPhaseCoordinator64 coordinator;
    coordinator.prepare(1024, 2);

    constexpr int bins = 513;
    std::vector<std::complex<double>> left(static_cast<size_t>(bins));
    std::vector<std::complex<double>> right(static_cast<size_t>(bins));
    auto fill = [&](double shift) {
        for (int k = 0; k < bins; ++k) {
            const double ph = 0.02 * k + shift;
            left[static_cast<size_t>(k)] = {std::cos(ph), std::sin(ph)};
            right[static_cast<size_t>(k)] = {std::cos(ph + 0.4), std::sin(ph + 0.4)};
        }
    };
    std::complex<double>* frames[2] = {left.data(), right.data()};
    fill(0.0);
    coordinator.process_group(frames, bins, 256, 320);
    fill(0.3);
    coordinator.process_group(frames, bins, 256, 320, 0.25);
    for (int k = 0; k < bins; k += 31) {
        REQUIRE(std::isfinite(left[static_cast<size_t>(k)].real()));
        REQUIRE(std::isfinite(right[static_cast<size_t>(k)].imag()));
    }
}

TEST_CASE("RealtimePitchTimeProcessor64 processes double buffers",
          "[signal][realtime-pitch][f64]") {
    RealtimePitchTimeConfig config;
    config.quality = PitchTimeQuality::low_latency;
    config.channels = 1;
    config.max_block = 256;
    RealtimePitchTimeProcessor64 proc;
    proc.prepare(kSr, config);

    std::vector<double> in(4096);
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = 0.1 * std::sin(2.0 * kPi * 330.0 * static_cast<double>(i) / kSr);
    std::vector<double> out(in.size());
    const double* ip[1];
    double* op[1];
    for (int pos = 0; pos < static_cast<int>(in.size()); pos += 256) {
        ip[0] = in.data() + pos;
        op[0] = out.data() + pos;
        proc.process(ip, op, 256);
    }

    bool saw_signal = false;
    for (double v : out) {
        REQUIRE(std::isfinite(v));
        saw_signal = saw_signal || std::abs(v) > 1e-10;
    }
    REQUIRE(saw_signal);
}
