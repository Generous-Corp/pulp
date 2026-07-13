// TransientPhasePolicy / FreezeHold / PitchedFeedbackDelay — Phase-4
// spectral-primitive tests.
//
// Acceptance gates from the design records: transient detector fires on
// broadband onsets and never on steady tones, phase reset improves click
// crest under stretch, freeze engages without allocation or clicks and
// never mutes on insufficient history, held audio stays stable and
// remains pitch-controllable, rapid toggling is safe, delay echo spacing
// is exact in ms and tempo-sync modes, the minimum delay is computed
// from the in-loop processor's latency, feedback stays bounded at the
// stability clamp, and freeze gates feedback recirculation.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/pitched_feedback_delay.hpp>
#include <pulp/signal/realtime_pitch_time_processor.hpp>
#include <pulp/signal/transient_phase_policy.hpp>
#include <atomic>
#include <cmath>
#include <complex>
#include <random>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

extern std::atomic<long> g_alloc_count; // sentinel in test_spectral_frame_engine.cpp

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSr = 48000.0;

std::vector<float> sine(double freq, double amp, int length) {
    std::vector<float> v(static_cast<size_t>(length));
    for (int i = 0; i < length; ++i)
        v[static_cast<size_t>(i)] =
            static_cast<float>(amp * std::sin(2.0 * kPi * freq * i / kSr));
    return v;
}

std::vector<float> run_mono(RealtimePitchTimeProcessor& proc, const std::vector<float>& in,
                            int block, int set_frozen_at = -1, int release_at = -1) {
    std::vector<float> out(in.size(), 0.0f);
    const float* ip[1];
    float* op[1];
    for (int pos = 0; pos < static_cast<int>(in.size()); pos += block) {
        if (set_frozen_at >= 0 && pos >= set_frozen_at) proc.set_frozen(true);
        if (release_at >= 0 && pos >= release_at) proc.set_frozen(false);
        const int n = std::min(block, static_cast<int>(in.size()) - pos);
        ip[0] = in.data() + pos;
        op[0] = out.data() + pos;
        proc.process(ip, op, n);
    }
    return out;
}

double window_rms_db(const std::vector<float>& x, int start, int length) {
    double acc = 0.0;
    for (int i = start; i < start + length && i < static_cast<int>(x.size()); ++i)
        acc += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
    return 10.0 * std::log10(acc / length + 1e-300);
}

double peak_hz(const std::vector<float>& x, int start, int n) {
    Fft fft(n);
    std::vector<std::complex<float>> buf(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float w = 0.5f - 0.5f * static_cast<float>(std::cos(2.0 * kPi * i / n));
        buf[static_cast<size_t>(i)] = {x[static_cast<size_t>(start + i)] * w, 0.0f};
    }
    fft.forward(buf.data());
    int kmax = 1;
    double mmax = 0.0;
    for (int k = 1; k < n / 2; ++k)
        if (std::abs(buf[static_cast<size_t>(k)]) > mmax) {
            mmax = std::abs(buf[static_cast<size_t>(k)]);
            kmax = k;
        }
    return kmax * kSr / n;
}

} // namespace

// ── TransientPhasePolicy ────────────────────────────────────────────────────

TEST_CASE("TransientPhasePolicy fires on broadband onsets, not steady tones",
          "[signal][transient]") {
    TransientPhasePolicy::Config config;
    config.fft_size = 1024;
    TransientPhasePolicy policy;
    policy.prepare(config);

    const int bins = 513;
    std::vector<std::complex<float>> frame(static_cast<size_t>(bins));
    std::complex<float>* frames[1] = {frame.data()};

    auto fill_tone = [&](float amp) {
        for (int k = 0; k < bins; ++k) {
            const float mag = amp / (1.0f + std::abs(k - 40));
            frame[static_cast<size_t>(k)] = {mag, 0.0f};
        }
    };
    auto fill_burst = [&] {
        for (int k = 0; k < bins; ++k)
            frame[static_cast<size_t>(k)] = {0.5f, 0.0f};
    };

    // Warm up on the steady tone: no detections allowed after history fills.
    int steady_fires = 0;
    for (int f = 0; f < 40; ++f) {
        fill_tone(1.0f);
        if (f > 12 && policy.analyze(frames, 1, bins) > 0.0f) ++steady_fires;
        else if (f <= 12) policy.analyze(frames, 1, bins);
    }
    REQUIRE(steady_fires == 0);

    // Broadband burst: must fire.
    fill_burst();
    REQUIRE(policy.analyze(frames, 1, bins) > 0.0f);

    // Back to the tone: settles again.
    int post_fires = 0;
    for (int f = 0; f < 40; ++f) {
        fill_tone(1.0f);
        if (f > 12 && policy.analyze(frames, 1, bins) > 0.0f) ++post_fires;
        else if (f <= 12) policy.analyze(frames, 1, bins);
    }
    REQUIRE(post_fires == 0);
}

TEST_CASE("TransientPhasePolicy64 fires on double broadband onsets",
          "[signal][transient][f64]") {
    TransientPhasePolicy64::Config config;
    config.fft_size = 1024;
    TransientPhasePolicy64 policy;
    policy.prepare(config);

    constexpr int bins = 513;
    std::vector<std::complex<double>> frame(static_cast<size_t>(bins));
    const std::complex<double>* frames[1] = {frame.data()};
    for (int f = 0; f < 16; ++f) {
        for (auto& bin : frame) bin = {0.001, 0.0};
        (void)policy.analyze(frames, 1, bins);
    }
    for (auto& bin : frame) bin = {0.5, 0.0};
    REQUIRE(policy.analyze(frames, 1, bins) > 0.0);
}

TEST_CASE("Transient preservation keeps a stretched impulse peaky",
          "[signal][transient]") {
    // A single loud isolated impulse is a maximally unambiguous transient: the
    // spectral-flux detector fires far above its median-relative threshold, so
    // the detect/phase-reset decision is identical across architectures,
    // optimization levels, and SIMD targets — the assertion measures the
    // feature, not floating-point luck. With phase reset on (transient
    // preservation) the impulse stays sharp under time-stretch; with it off the
    // phase vocoder smears it across frames, lowering the peak-to-RMS crest.
    auto crest_db = [](const std::vector<float>& x) {
        float peak = 0.0f;
        for (float v : x) peak = std::max(peak, std::abs(v));
        double rms = 0.0;
        for (float v : x) rms += static_cast<double>(v) * v;
        rms = std::sqrt(rms / static_cast<double>(x.size()));
        return 20.0 * std::log10((peak + 1e-30) / (rms + 1e-30));
    };

    auto stretch_impulse = [&](bool preserve) {
        RealtimePitchTimeConfig config;
        config.quality = PitchTimeQuality::quality;
        config.mode = PitchTimeMode::time_stretch;
        config.transient_preservation = preserve;
        RealtimePitchTimeProcessor proc;
        proc.prepare(kSr, config);
        proc.set_time_ratio(1.5f);

        std::vector<float> in(96000, 0.0f);
        in[24000] = 1.0f;  // one strong isolated impulse in silence
        const float* ip[1];
        std::vector<float> collected;
        std::vector<float> chunk(1024);
        float* cp[1] = {chunk.data()};
        for (int pos = 0; pos < 96000; pos += 480) {
            ip[0] = in.data() + pos;
            proc.feed(ip, 480);
            while (proc.available_stretched() >= 1024) {
                proc.read_stretched(cp, 1024);
                collected.insert(collected.end(), chunk.begin(), chunk.end());
            }
        }
        return crest_db(collected);
    };

    const double crest_on = stretch_impulse(true);
    const double crest_off = stretch_impulse(false);
    INFO("crest on " << crest_on << " dB, off " << crest_off << " dB");
    // Preservation keeps the impulse clearly peakier. The measured margin is
    // ~4.4 dB (x86_64) / ~4.9 dB (arm64); assert a conservative 2 dB so the
    // test is a real behavioral guarantee with wide cross-architecture headroom
    // rather than a rounding-noise coin flip.
    REQUIRE(crest_on > crest_off + 2.0);
}

// ── FreezeHold (through the processor) ──────────────────────────────────────

TEST_CASE("FreezeHold latches a steady tone and holds it stably",
          "[signal][freeze]") {
    RealtimePitchTimeProcessor proc;
    RealtimePitchTimeConfig config;
    config.quality = PitchTimeQuality::quality;
    proc.prepare(kSr, config);
    proc.set_pitch_semitones(0.0f);

    auto in = sine(440.0, 0.5, 480000); // 10 s
    // Engage at 2 s; input keeps playing (hold should ignore it).
    for (size_t i = 192000; i < in.size(); ++i)
        in[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 440.0 * i / kSr));
    auto out = run_mono(proc, in, 480, /*set_frozen_at=*/96000);

    REQUIRE(proc.is_frozen());

    // Hold stability: 100 ms RMS windows from 3 s to 10 s within 1 dB.
    double lo = 1e9, hi = -1e9;
    for (int start = 144000; start + 4800 < 480000; start += 4800) {
        const double rms = window_rms_db(out, start, 4800);
        lo = std::min(lo, rms);
        hi = std::max(hi, rms);
    }
    INFO("hold RMS spread: " << hi - lo << " dB");
    REQUIRE(hi - lo < 1.0);

    // Held spectrum stays at the latched tone.
    REQUIRE_THAT(peak_hz(out, 240000, 65536), WithinAbs(440.0, 2.0));

    // No click after the vocoder turn-on transient (the analysis window
    // edge zeroes the first few reconstructable samples at stream start;
    // engage/release transitions ARE included in this scan).
    float max_step = 0.0f;
    for (int i = proc.latency_samples() + proc.fft_size() + 1; i < 480000; ++i)
        max_step = std::max(max_step,
                            std::abs(out[static_cast<size_t>(i)] - out[static_cast<size_t>(i - 1)]));
    INFO("max step: " << max_step);
    REQUIRE(max_step < 0.2f);
}

TEST_CASE("FreezeHold engage/release transitions are level-flat (no dip)",
          "[signal][freeze]") {
    // The user-audible gate for the transition handling: engaging or
    // releasing the hold over a steady tone must not notch the level. Two
    // past defects both produced a ~50 ms, 4-5 dB dip ("ding") right at the
    // edge: (1) phases advanced one hop BEFORE the first mixed hold frame,
    // breaking the phase alignment the linear engage fade relies on; and
    // (2) a linear release fade over the (decorrelated, jittered) hold,
    // where only an equal-power law is power-flat.
    RealtimePitchTimeProcessor proc;
    RealtimePitchTimeConfig config;
    config.quality = PitchTimeQuality::quality;
    proc.prepare(kSr, config);

    auto in = sine(440.0, 0.5, 336000);  // 7 s, tone throughout
    auto out = run_mono(proc, in, 480, /*set_frozen_at=*/96000,
                        /*release_at=*/192000);

    // 5 ms RMS windows across each transition (+-0.5 s around the toggle,
    // generous enough to cover the pipeline latency offset). Flat means
    // every window within 2 dB of the region's mean - the dips this pins
    // against measured -4.6 dB (engage) and -5.1 dB (release).
    auto scan = [&](int from, int to) {
        const int win = 240;  // 5 ms
        double mean = 0.0;
        int count = 0;
        for (int s = from; s + win <= to; s += win) {
            mean += window_rms_db(out, s, win);
            ++count;
        }
        mean /= count;
        double worst = 0.0;
        for (int s = from; s + win <= to; s += win)
            worst = std::max(worst, std::abs(window_rms_db(out, s, win) - mean));
        return worst;
    };
    const double engage_dev = scan(72000, 144000);
    const double release_dev = scan(168000, 240000);
    INFO("worst 5 ms deviation: engage " << engage_dev << " dB, release "
                                         << release_dev << " dB");
    REQUIRE(engage_dev < 2.0);
    REQUIRE(release_dev < 2.0);
}

TEST_CASE("FreezeHold never mutes when engaged before history fills",
          "[signal][freeze]") {
    RealtimePitchTimeProcessor proc;
    RealtimePitchTimeConfig config;
    config.quality = PitchTimeQuality::quality;
    proc.prepare(kSr, config);
    proc.set_frozen(true); // engage immediately, zero history

    auto in = sine(330.0, 0.5, 192000);
    auto out = run_mono(proc, in, 480);

    // Every 100 ms window after the pipeline latency carries signal.
    for (int start = proc.latency_samples() + 4800; start + 4800 < 192000; start += 4800) {
        INFO("window at " << start);
        REQUIRE(window_rms_db(out, start, 4800) > -40.0);
    }
    REQUIRE(proc.is_frozen()); // latched once the capture filled
}

TEST_CASE("FreezeHold survives rapid toggling", "[signal][freeze]") {
    RealtimePitchTimeProcessor proc;
    RealtimePitchTimeConfig config;
    config.quality = PitchTimeQuality::quality;
    proc.prepare(kSr, config);

    auto in = sine(440.0, 0.5, 240000);
    std::vector<float> out(in.size(), 0.0f);
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> toggle_after(1, 20);
    const float* ip[1];
    float* op[1];
    int next_toggle = 10;
    bool frozen = false;
    for (int pos = 0, blocks = 0; pos < 240000; pos += 480, ++blocks) {
        if (blocks >= next_toggle) {
            frozen = !frozen;
            proc.set_frozen(frozen);
            next_toggle = blocks + toggle_after(rng);
        }
        ip[0] = in.data() + pos;
        op[0] = out.data() + pos;
        proc.process(ip, op, 480);
    }
    for (float v : out) REQUIRE(std::isfinite(v));
}

TEST_CASE("FreezeHold held audio remains pitch-controllable", "[signal][freeze]") {
    RealtimePitchTimeProcessor proc;
    RealtimePitchTimeConfig config;
    config.quality = PitchTimeQuality::quality;
    proc.prepare(kSr, config);
    proc.set_pitch_semitones(0.0f);

    auto in = sine(440.0, 0.5, 480000);
    std::vector<float> out(in.size(), 0.0f);
    const float* ip[1];
    float* op[1];
    for (int pos = 0; pos < 480000; pos += 480) {
        if (pos >= 96000) proc.set_frozen(true);
        if (pos >= 192000) proc.set_pitch_semitones(7.0f); // shift the HOLD
        ip[0] = in.data() + pos;
        op[0] = out.data() + pos;
        proc.process(ip, op, 480);
    }

    const double held = peak_hz(out, 144000, 32768);
    const double shifted = peak_hz(out, 384000, 32768);
    INFO("held " << held << " Hz, shifted " << shifted << " Hz");
    REQUIRE_THAT(held, WithinAbs(440.0, 3.0));
    REQUIRE_THAT(shifted, WithinAbs(440.0 * std::exp2(7.0 / 12.0), 6.0));
}

// ── PitchedFeedbackDelay ────────────────────────────────────────────────────

TEST_CASE("PitchedFeedbackDelay echo spacing matches ms and sync settings",
          "[signal][pitched-delay]") {
    PitchedFeedbackDelay delay;
    PitchedFeedbackDelay::Config config;
    config.max_block = 48000; // the block size this test drives (asserted in process())
    delay.prepare(kSr, config);
    delay.set_feedback(0.5f);
    delay.set_delay_ms(250.0f); // 12000 samples

    std::vector<float> in(48000, 0.0f), out(48000, 0.0f);
    in[100] = 1.0f;
    const float* ip[1] = {in.data()};
    float* op[1] = {out.data()};
    delay.process(ip, op, 48000);

    auto local_peak = [&](int around) {
        int best = around - 64;
        for (int i = around - 64; i < around + 64; ++i)
            if (std::abs(out[static_cast<size_t>(i)])
                > std::abs(out[static_cast<size_t>(best)]))
                best = i;
        return best;
    };
    REQUIRE(std::abs(local_peak(100 + 12000) - (100 + 12000)) <= 1);
    REQUIRE(std::abs(local_peak(100 + 24000) - (100 + 24000)) <= 1);
    const float first = std::abs(out[static_cast<size_t>(local_peak(100 + 12000))]);
    const float second = std::abs(out[static_cast<size_t>(local_peak(100 + 24000))]);
    REQUIRE_THAT(second / first, WithinAbs(0.5f, 0.05f));

    // Tempo sync: 0.5 beats at 120 bpm = 250 ms — identical spacing.
    PitchedFeedbackDelay synced;
    synced.prepare(kSr, config);
    synced.set_feedback(0.0f);
    synced.set_delay_sync(120.0, 0.5);
    std::vector<float> out2(48000, 0.0f);
    float* op2[1] = {out2.data()};
    delay.reset();
    synced.process(ip, op2, 48000);
    int best = 0;
    for (int i = 0; i < 48000; ++i)
        if (std::abs(out2[static_cast<size_t>(i)]) > std::abs(out2[static_cast<size_t>(best)]))
            best = i;
    REQUIRE(std::abs(best - (100 + 12000)) <= 1);
}

TEST_CASE("PitchedFeedbackDelay64 processes double delay lines",
          "[signal][pitched-delay][f64]") {
    PitchedFeedbackDelay64 delay;
    PitchedFeedbackDelay64::Config config;
    config.max_block = 4096;
    delay.prepare(kSr, config);
    delay.set_feedback(0.0f);
    delay.set_delay_ms(10.0f);

    std::vector<double> in(4096, 0.0), out(4096, 0.0);
    in[32] = 1.0;
    const double* ip[1] = {in.data()};
    double* op[1] = {out.data()};
    delay.process(ip, op, 4096);

    int best = 0;
    for (int i = 0; i < 4096; ++i)
        if (std::abs(out[static_cast<size_t>(i)]) > std::abs(out[static_cast<size_t>(best)]))
            best = i;
    REQUIRE(std::abs(best - (32 + 480)) <= 1);
}

namespace {

// Fake in-loop processor: a fixed delay line reporting its own latency,
// optionally frozen.
class FakeLoop : public FeedbackLoopProcessor {
public:
    explicit FakeLoop(int latency) : latency_(latency), buf_(static_cast<size_t>(latency), 0.0f) {}
    int loop_latency_samples() const override { return latency_; }
    bool loop_is_frozen() const override { return frozen_; }
    void loop_process(const float* const* in, float* const* out, int n) override {
        for (int i = 0; i < n; ++i) {
            const float delayed = buf_[static_cast<size_t>(pos_)];
            buf_[static_cast<size_t>(pos_)] = in[0][i];
            pos_ = (pos_ + 1) % latency_;
            out[0][i] = delayed;
        }
    }
    bool frozen_ = false;

private:
    int latency_;
    std::vector<float> buf_;
    int pos_ = 0;
};

} // namespace

TEST_CASE("PitchedFeedbackDelay computes min delay from the loop processor's latency",
          "[signal][pitched-delay]") {
    PitchedFeedbackDelay delay;
    PitchedFeedbackDelay::Config config;
    delay.prepare(kSr, config);
    FakeLoop loop(100);
    delay.set_loop_processor(&loop);
    REQUIRE(delay.min_delay_samples() == 101);

    // Requesting below the floor clamps: echoes land at 101 samples.
    delay.set_delay_ms(1.0f); // 48 samples — below the floor
    delay.set_feedback(0.0f);
    std::vector<float> in(4096, 0.0f), out(4096, 0.0f);
    in[50] = 1.0f;
    const float* ip[1] = {in.data()};
    float* op[1] = {out.data()};
    delay.process(ip, op, 4096);
    int best = 0;
    for (int i = 0; i < 4096; ++i)
        if (std::abs(out[static_cast<size_t>(i)]) > std::abs(out[static_cast<size_t>(best)]))
            best = i;
    REQUIRE(std::abs(best - (50 + 101)) <= 1);
}

TEST_CASE("PitchedFeedbackDelay stays bounded at maximum feedback",
          "[signal][pitched-delay]") {
    PitchedFeedbackDelay delay;
    PitchedFeedbackDelay::Config config;
    delay.prepare(kSr, config);
    delay.set_delay_ms(40.0f);
    delay.set_feedback(1.5f); // clamps to 0.99

    std::mt19937 rng(7);
    std::uniform_real_distribution<float> dist(-0.1f, 0.1f);
    std::vector<float> in(480000), out(480000);
    for (auto& v : in) v = dist(rng);
    const float* ip[1];
    float* op[1];
    for (int pos = 0; pos < 480000; pos += 480) {
        ip[0] = in.data() + pos;
        op[0] = out.data() + pos;
        delay.process(ip, op, 480);
    }
    float peak = 0.0f;
    for (float v : out) {
        REQUIRE(std::isfinite(v));
        peak = std::max(peak, std::abs(v));
    }
    // Geometric series bound: in_peak / (1 - 0.99) = 10.
    INFO("peak " << peak);
    REQUIRE(peak < 12.0f);
}

TEST_CASE("PitchedFeedbackDelay gates feedback while the loop is frozen",
          "[signal][pitched-delay]") {
    PitchedFeedbackDelay delay;
    PitchedFeedbackDelay::Config config;
    delay.prepare(kSr, config);
    FakeLoop loop(64);
    delay.set_loop_processor(&loop);
    delay.set_delay_ms(50.0f); // 2400 samples
    delay.set_feedback(0.9f);

    // Recirculate an impulse for 2 s, then freeze the loop.
    std::vector<float> in(192000, 0.0f), out(192000, 0.0f);
    in[100] = 1.0f;
    const float* ip[1];
    float* op[1];
    for (int pos = 0; pos < 192000; pos += 480) {
        if (pos >= 96000) loop.frozen_ = true;
        ip[0] = in.data() + pos;
        op[0] = out.data() + pos;
        delay.process(ip, op, 480);
    }
    // Echo energy decays once frozen: compare 0.5 s windows.
    const double before = window_rms_db(out, 72000, 24000);
    const double after = window_rms_db(out, 160000, 24000);
    INFO("before " << before << " dB, after " << after << " dB");
    REQUIRE(after < before - 20.0);
}

TEST_CASE("Phase-4 primitives allocate nothing after prepare",
          "[signal][freeze][pitched-delay]") {
    RealtimePitchTimeProcessor proc;
    RealtimePitchTimeConfig pconfig;
    pconfig.quality = PitchTimeQuality::low_latency;
    proc.prepare(kSr, pconfig);
    proc.set_frozen(true);

    PitchedFeedbackDelay delay;
    PitchedFeedbackDelay::Config dconfig;
    delay.prepare(kSr, dconfig);
    delay.set_feedback(0.7f);
    delay.set_delay_ms(120.0f);

    auto in = sine(440.0, 0.5, 8192);
    std::vector<float> mid(8192, 0.0f), out(8192, 0.0f);
    const float* ip[1] = {in.data()};
    float* mp[1] = {mid.data()};
    float* op[1] = {out.data()};

    proc.process(ip, mp, 4096);  // warm-up (latch included)
    const float* mp_const[1] = {mid.data()};
    delay.process(mp_const, op, 4096);

    const long before = g_alloc_count.load();
    proc.process(ip, mp, 4096);
    delay.process(mp_const, op, 4096);
    REQUIRE(g_alloc_count.load() == before);
}
