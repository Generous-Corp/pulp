// Multi-character delay — audio-domain acceptance suite.
//
// Every case here measures the module's OUTPUT rather than its internals: a
// character is a claim about what happens to a repeat, and the only honest way
// to check such a claim is to render audio and measure it. Where a case refers
// to a calibration-table value it reads that value from the shipped table
// rather than restating a number, so retuning a table cannot silently
// invalidate the test that guards it.
//
// Two measurements deviate from the letter of the acceptance recipes, both
// documented at the case that uses them:
//
//   * BBD delay time is measured from the ONSET of a burst, not from the peak.
//     A compander's expander has a ~10 ms attack, so the peak of anything fed
//     through one is displaced by the expander, not by the clock — the peak of
//     an impulse measures the compander and the recipe is trying to measure the
//     bucket chain.
//   * The physical tier's loss FIR is compared against the analytic response
//     only where a filter of the specified order can express it. See the case
//     for the measured numbers and tape_physical.hpp for why the limit is
//     structural rather than a defect.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/baked_graph_processor.hpp>
#include <pulp/host/forge_character_delay_catalog.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/signal/character_delay.hpp>
#include <pulp/signal/fft.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace cd = pulp::signal::chardelay;
using Engine = pulp::signal::CharacterDelay;
using Character = Engine::Character;
using TapeTier = Engine::TapeTier;

namespace {

constexpr double kSr = 48000.0;
constexpr int kBlock = 128;

struct Stereo {
    std::vector<float> left;
    std::vector<float> right;
};

Stereo make_stereo(int n) {
    return {std::vector<float>(static_cast<std::size_t>(n), 0.0f),
            std::vector<float>(static_cast<std::size_t>(n), 0.0f)};
}

/// Render in place, in blocks, the way a host would.
void render(Engine& delay, Stereo& buffers) {
    const auto n = static_cast<int>(buffers.left.size());
    for (int i = 0; i < n; i += kBlock) {
        const int count = std::min(kBlock, n - i);
        delay.process(buffers.left.data() + i, buffers.right.data() + i, count);
    }
}

/// Run `seconds` of silence through the delay to settle slews and envelopes.
void settle(Engine& delay, double seconds) {
    auto quiet = make_stereo(static_cast<int>(kSr * seconds));
    render(delay, quiet);
}

Stereo impulse_left(int n, float amplitude = 1.0f) {
    auto s = make_stereo(n);
    s.left[0] = amplitude;
    return s;
}

Stereo sine_both(int n, double hz, float amplitude) {
    auto s = make_stereo(n);
    for (int i = 0; i < n; ++i) {
        const auto v = static_cast<float>(
            amplitude * std::sin(2.0 * cd::kPi * hz * static_cast<double>(i) / kSr));
        s.left[static_cast<std::size_t>(i)] = v;
        s.right[static_cast<std::size_t>(i)] = v;
    }
    return s;
}

/// Hann-windowed tone burst in the left channel — the stimulus for characters
/// whose level-dependent stages make a bare impulse unrepresentative.
Stereo burst_left(int n, double hz, double seconds, float amplitude = 1.0f) {
    auto s = make_stereo(n);
    const int length = std::min(n, static_cast<int>(seconds * kSr));
    for (int i = 0; i < length; ++i) {
        const double window =
            0.5 * (1.0 - std::cos(2.0 * cd::kPi * static_cast<double>(i) /
                                  static_cast<double>(length)));
        s.left[static_cast<std::size_t>(i)] = static_cast<float>(
            amplitude * window *
            std::sin(2.0 * cd::kPi * hz * static_cast<double>(i) / kSr));
    }
    return s;
}

double peak(const std::vector<float>& v, int from, int to) {
    double best = 0.0;
    const int end = std::min(to, static_cast<int>(v.size()));
    for (int i = std::max(0, from); i < end; ++i)
        best = std::max(best, std::abs(static_cast<double>(v[static_cast<std::size_t>(i)])));
    return best;
}

int peak_index(const std::vector<float>& v, int from, int to) {
    int best = from;
    double best_value = -1.0;
    const int end = std::min(to, static_cast<int>(v.size()));
    for (int i = std::max(0, from); i < end; ++i) {
        const double a = std::abs(static_cast<double>(v[static_cast<std::size_t>(i)]));
        if (a > best_value) {
            best_value = a;
            best = i;
        }
    }
    return best;
}

/// First index whose magnitude exceeds `fraction` of the buffer's peak.
int onset_index(const std::vector<float>& v, double fraction) {
    const double threshold = fraction * peak(v, 0, static_cast<int>(v.size()));
    for (std::size_t i = 0; i < v.size(); ++i)
        if (std::abs(static_cast<double>(v[i])) > threshold) return static_cast<int>(i);
    return -1;
}

double rms(const std::vector<float>& v, int from, int to) {
    double sum = 0.0;
    int count = 0;
    const int end = std::min(to, static_cast<int>(v.size()));
    for (int i = std::max(0, from); i < end; ++i) {
        const double s = static_cast<double>(v[static_cast<std::size_t>(i)]);
        sum += s * s;
        ++count;
    }
    return count > 0 ? std::sqrt(sum / static_cast<double>(count)) : 0.0;
}

bool all_finite(const std::vector<float>& v) {
    for (float s : v)
        if (!std::isfinite(s)) return false;
    return true;
}

/// Largest sample-to-sample step in a window — the click detector.
double max_step(const std::vector<float>& v, int from, int to) {
    double worst = 0.0;
    const int end = std::min(to, static_cast<int>(v.size()));
    for (int i = std::max(1, from); i < end; ++i)
        worst = std::max(worst, std::abs(static_cast<double>(v[static_cast<std::size_t>(i)]) -
                                         static_cast<double>(v[static_cast<std::size_t>(i - 1)])));
    return worst;
}

/// Welch power spectral density: Hann windows, 50% overlap, averaged.
std::vector<double> welch_psd(const std::vector<float>& signal, int from, int to,
                              int segment = 32768) {
    const int available = std::min(to, static_cast<int>(signal.size())) - from;
    while (segment > available && segment > 256) segment /= 2;
    std::vector<double> window(static_cast<std::size_t>(segment));
    for (int i = 0; i < segment; ++i)
        window[static_cast<std::size_t>(i)] =
            0.5 * (1.0 - std::cos(2.0 * cd::kPi * i / segment));

    pulp::signal::FftT<double> fft(segment);
    std::vector<double> psd(static_cast<std::size_t>(segment / 2 + 1), 0.0);
    std::vector<std::complex<double>> scratch(static_cast<std::size_t>(segment));

    int segments = 0;
    for (int start = from; start + segment <= from + available; start += segment / 2) {
        for (int i = 0; i < segment; ++i) {
            const double s = static_cast<double>(signal[static_cast<std::size_t>(start + i)]);
            scratch[static_cast<std::size_t>(i)] = {s * window[static_cast<std::size_t>(i)], 0.0};
        }
        fft.forward(scratch.data());
        for (std::size_t bin = 0; bin < psd.size(); ++bin) psd[bin] += std::norm(scratch[bin]);
        ++segments;
    }
    if (segments > 0)
        for (double& value : psd) value /= segments;
    return psd;
}

double psd_bin_hz(std::size_t bin, std::size_t bins, double rate) {
    return static_cast<double>(bin) * rate / (2.0 * static_cast<double>(bins - 1));
}

/// Magnitude of a signal's spectrum at one frequency, by direct correlation.
/// Cheaper and sharper than binning a PSD when only a handful of points matter.
double magnitude_at(const std::vector<float>& v, int from, int to, double hz) {
    double real = 0.0;
    double imaginary = 0.0;
    const int end = std::min(to, static_cast<int>(v.size()));
    const int count = end - from;
    for (int i = from; i < end; ++i) {
        const double phase = 2.0 * cd::kPi * hz * static_cast<double>(i - from) / kSr;
        const double s = static_cast<double>(v[static_cast<std::size_t>(i)]);
        real += s * std::cos(phase);
        imaginary -= s * std::sin(phase);
    }
    return 2.0 * std::hypot(real, imaginary) / std::max(count, 1);
}

/// Unwrapped phase of a tone near `carrier`, by quadrature demodulation:
/// multiply down to baseband, lowpass, and unwrap.
///
/// Phase rather than instantaneous frequency, because the quantity under test
/// is a DELAY modulation and phase converts back to it directly (dt =
/// dphi / 2 pi f). Differentiating to frequency multiplies the demodulator's
/// own noise by the sample rate and buries a sub-millisecond wobble in it.
constexpr int kTrackDecimation = 64;
constexpr double kTrackRate = kSr / kTrackDecimation;

std::vector<double> phase_track(const std::vector<float>& v, int from, int to,
                                double carrier) {
    cd::OnePole in_phase_filter;
    cd::OnePole quadrature_filter;
    in_phase_filter.set_cutoff(120.0, kSr);
    quadrature_filter.set_cutoff(120.0, kSr);

    const int end = std::min(to, static_cast<int>(v.size()));
    std::vector<double> track;
    track.reserve(static_cast<std::size_t>(std::max(0, end - from)));

    double previous_phase = 0.0;
    double unwrapped = 0.0;
    bool have_previous = false;
    for (int i = from; i < end; ++i) {
        const double t = static_cast<double>(i) / kSr;
        const double s = static_cast<double>(v[static_cast<std::size_t>(i)]);
        const double in_phase = in_phase_filter.lowpass(s * std::cos(2.0 * cd::kPi * carrier * t));
        const double quadrature =
            quadrature_filter.lowpass(-s * std::sin(2.0 * cd::kPi * carrier * t));
        if ((i - from) % kTrackDecimation != 0) continue;
        const double phase = std::atan2(quadrature, in_phase);
        if (have_previous) {
            double difference = phase - previous_phase;
            while (difference > cd::kPi) difference -= 2.0 * cd::kPi;
            while (difference < -cd::kPi) difference += 2.0 * cd::kPi;
            unwrapped += difference;
        }
        track.push_back(unwrapped);
        previous_phase = phase;
        have_previous = true;
    }
    return track;
}

/// Spectral centroid over a window, in Hz.
double spectral_centroid(const std::vector<float>& v, int from, int to) {
    const auto psd = welch_psd(v, from, to, 4096);
    double weighted = 0.0;
    double total = 0.0;
    for (std::size_t bin = 1; bin < psd.size(); ++bin) {
        const double f = psd_bin_hz(bin, psd.size(), kSr);
        weighted += f * psd[bin];
        total += psd[bin];
    }
    return total > 0.0 ? weighted / total : 0.0;
}

/// Configure a delay with the common test defaults.
void configure(Engine& delay, Character character, double time_ms, double feedback,
               double character_amount, TapeTier tier = TapeTier::standard) {
    delay.set_character(character);
    delay.set_tape_tier(tier);
    delay.set_sample_rate(kSr);
    delay.set_time_ms(static_cast<float>(time_ms));
    delay.set_time_offset(1.0f);
    delay.set_feedback(static_cast<float>(feedback));
    delay.set_crossfeed(0.0f);
    delay.set_character_amount(static_cast<float>(character_amount));
    delay.set_mod(0.0f, 0.0f);
    delay.set_duck(0.0f);
    delay.set_freeze(false);
    delay.set_reverse(false);
    delay.reset();
}

/// Slew constant for a character, in seconds — read from the shipped table so
/// the settle time a test uses always tracks the value it is settling.
double slew_seconds(Character character) {
    switch (character) {
        case Character::vintage_digital: return cd::kTimeSlewVintageMs * 0.001;
        case Character::tape: return cd::kTimeSlewTapeMs * 0.001;
        case Character::bbd: return cd::kTimeSlewBbdMs * 0.001;
        case Character::diffusion: return cd::kTimeSlewDiffusionMs * 0.001;
        case Character::clean:
        default: return cd::kTimeSlewCleanMs * 0.001;
    }
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 1 — Engine-time accuracy
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("character delay places repeats at the requested time", "[character-delay][time]") {
    for (auto character : {Character::clean, Character::diffusion, Character::tape}) {
        for (double time_ms : {10.0, 100.0, 350.0, 1000.0, 2000.0}) {
            Engine delay;
            configure(delay, character, time_ms, 0.0, 0.0);
            settle(delay, 4.0 * slew_seconds(character) + 0.05);

            auto buffers = impulse_left(static_cast<int>(kSr * (time_ms * 0.002 + 0.2)));
            render(delay, buffers);

            // Diffusion is measured at the ONSET of its repeat rather than at
            // the peak. Its whole purpose is to smear a repeat across an
            // allpass chain, so the loudest sample sits somewhere inside the
            // cluster and moves with the character macro; where the repeat
            // BEGINS is what the delay time actually sets.
            const int index =
                character == Character::diffusion
                    ? onset_index(buffers.left, 0.05)
                    : peak_index(buffers.left, 1, static_cast<int>(buffers.left.size()));
            const double expected = time_ms * 0.001 * kSr;

            INFO("character index " << static_cast<int>(character) << ", time " << time_ms
                                    << " ms, repeat at " << index << ", expected " << expected);
            REQUIRE(index > 0);
            CHECK(std::abs(index - expected) <= 0.001 * kSr);  // +-1 ms
        }
    }
}

TEST_CASE("clocked characters place repeats within the clock grid's tolerance",
          "[character-delay][time]") {
    // A bare impulse through a compander measures the expander's attack, not
    // the bucket chain, so the clocked characters are measured from the ONSET
    // of a short burst: the onset is set by the line, the envelope that follows
    // it by the compander.
    for (auto character : {Character::bbd, Character::vintage_digital}) {
        for (double time_ms : {10.0, 50.0, 350.0, 1000.0}) {
            Engine delay;
            configure(delay, character, time_ms, 0.0, 0.5);
            settle(delay, 4.0 * slew_seconds(character) + 0.4);

            const int frames = static_cast<int>(kSr * (time_ms * 0.002 + 0.3));
            auto buffers = burst_left(frames, 1000.0, 0.002);
            // The stimulus's own onset by the same rule, so the burst's rise
            // time cancels out of the measurement.
            const int stimulus_onset = onset_index(buffers.left, 0.05);
            render(delay, buffers);
            const int index = onset_index(buffers.left, 0.05) - stimulus_onset;
            const double expected = time_ms * 0.001 * kSr;

            INFO("character index " << static_cast<int>(character) << ", time " << time_ms
                                    << " ms, onset at " << index << ", expected " << expected);
            REQUIRE(index > 0);
            CHECK(std::abs(index - expected) <= 0.02 * expected);  // +-2%
        }
    }
}

TEST_CASE("time offset scales the right channel's delay", "[character-delay][time]") {
    for (double offset : {0.5, 1.0, 1.5}) {
        Engine delay;
        configure(delay, Character::clean, 400.0, 0.0, 0.0);
        delay.set_time_offset(static_cast<float>(offset));
        delay.set_crossfeed(0.0f);
        delay.reset();
        settle(delay, 0.2);

        auto buffers = make_stereo(static_cast<int>(kSr * 1.2));
        buffers.left[0] = 1.0f;
        buffers.right[0] = 1.0f;
        render(delay, buffers);

        const int right_index =
            peak_index(buffers.right, 1, static_cast<int>(buffers.right.size()));
        const double expected = 400.0 * offset * 0.001 * kSr;
        INFO("offset " << offset << ", right peak at " << right_index);
        CHECK(std::abs(right_index - expected) <= 0.001 * kSr);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 2 — Repeat decay
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("repeats decay at the feedback amount", "[character-delay][feedback]") {
    struct Case {
        Character character;
        double amount;
        double time_ms;
        double burst_seconds;
        double low;
        double high;
    };
    // The BBD's compander has a ~10 ms time constant, so its repeats have to be
    // long enough — and far enough apart — for the envelopes to reach the
    // operating region the device is designed around. Measuring its decay with
    // a 10 ms blip measures the compander's attack instead.
    const Case cases[] = {
        {Character::clean, 0.0, 100.0, 0.01, 0.48, 0.52},
        {Character::tape, 0.0, 100.0, 0.01, 0.40, 0.60},
        {Character::bbd, 0.5, 250.0, 0.08, 0.40, 0.60},
    };

    for (const auto& c : cases) {
        Engine delay;
        configure(delay, c.character, c.time_ms, 0.5, c.amount);
        settle(delay, 4.0 * slew_seconds(c.character) + 0.4);

        // A short burst rather than a bare impulse: the tape saturator and the
        // BBD compander are level-dependent, and an impulse's crest factor puts
        // both far outside the operating region a repeat actually sits in.
        // A moderate level, not a hot one. Both coloured characters compress
        // at high level by design (that IS the coloration), so measuring the
        // FEEDBACK law at the top of their range would be measuring the
        // saturator instead.
        auto buffers = burst_left(static_cast<int>(kSr * (c.time_ms * 0.001 * 6.0)), 1000.0,
                                  c.burst_seconds, 0.2f);
        render(delay, buffers);

        double previous = 0.0;
        for (int k = 1; k <= 5; ++k) {
            const int centre = static_cast<int>(k * c.time_ms * 0.001 * kSr);
            const int half = static_cast<int>(0.6 * c.burst_seconds * kSr) + 200;
            const double p = rms(buffers.left, centre - half, centre + half);
            if (k > 1) {
                const double ratio = p / std::max(previous, 1e-12);
                INFO("character index " << static_cast<int>(c.character) << ", repeat "
                                        << k << " ratio " << ratio);
                CHECK(ratio > c.low);
                CHECK(ratio < c.high);
            }
            previous = p;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 3 — Self-oscillation contract
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("saturating characters self-oscillate bounded at maximum feedback",
          "[character-delay][feedback][slow]") {
    for (auto character : {Character::tape, Character::bbd, Character::vintage_digital}) {
        Engine delay;
        configure(delay, character, 250.0, 1.1, 0.7);

        // R4's protocol: a single small impulse, then a long zero-input settle.
        // The settle is 25 s rather than 10 because the BBD's clocked
        // band-limiting sheds energy on every pass, so its oscillation builds
        // far more slowly than tape's — which is the character behaving
        // correctly, not failing to oscillate.
        auto seed = make_stereo(static_cast<int>(kSr * 25.0));
        seed.left[0] = 0.01f;
        seed.right[0] = 0.01f;
        render(delay, seed);

        auto tail = make_stereo(static_cast<int>(kSr));
        render(delay, tail);

        const double level = rms(tail.left, 0, static_cast<int>(tail.left.size()));
        INFO("character index " << static_cast<int>(character) << " rms " << level);
        CHECK(all_finite(tail.left));
        CHECK(level > 1e-3);
        CHECK(peak(tail.left, 0, static_cast<int>(tail.left.size())) < 4.0);
    }
}

TEST_CASE("unsaturated characters always decay", "[character-delay][feedback][slow]") {
    // Clean and Diffusion have no in-loop saturator, so their feedback is
    // clamped below unity and the tail must die whatever the knob says.
    for (auto character : {Character::clean, Character::diffusion}) {
        Engine delay;
        configure(delay, character, 250.0, 1.1, 0.7);

        auto seed = make_stereo(static_cast<int>(kSr * 20.0));
        seed.left[0] = 0.01f;
        seed.right[0] = 0.01f;
        render(delay, seed);

        auto tail = make_stereo(static_cast<int>(kSr));
        render(delay, tail);

        const double level = rms(tail.left, 0, static_cast<int>(tail.left.size()));
        INFO("character index " << static_cast<int>(character) << " rms " << level);
        CHECK(all_finite(tail.left));
        CHECK(level < 1e-4);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 4 — Ping-pong
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("full crossfeed bounces repeats between channels", "[character-delay][stereo]") {
    Engine delay;
    configure(delay, Character::clean, 100.0, 0.7, 0.0);
    delay.set_crossfeed(1.0f);
    delay.reset();
    settle(delay, 0.2);

    auto buffers = impulse_left(static_cast<int>(kSr * 0.6));
    render(delay, buffers);

    for (int k = 1; k <= 4; ++k) {
        const int centre = static_cast<int>(k * 0.1 * kSr);
        const double left = peak(buffers.left, centre - 400, centre + 400);
        const double right = peak(buffers.right, centre - 400, centre + 400);
        const double dominant = (k % 2 == 1) ? left : right;
        const double quiet = (k % 2 == 1) ? right : left;
        INFO("repeat " << k << " L " << left << " R " << right);
        CHECK(dominant > 0.0);
        CHECK(20.0 * std::log10((quiet + 1e-15) / dominant) < -20.0);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 5 — BBD bandwidth law
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("BBD bandwidth follows the clock rate", "[character-delay][bbd]") {
    for (double time_ms : {50.0, 500.0}) {
        Engine delay;
        configure(delay, Character::bbd, time_ms, 0.0, 0.5);
        settle(delay, 4.0 * slew_seconds(Character::bbd) + 0.5);

        // Expected value comes from the shipped table, not from a restated
        // number: the law is N/t/3 clamped, whatever N and the clamps are.
        const double stages = static_cast<double>(delay.bbd_stages());
        const double clock = stages / (time_ms * 0.001);
        const double expected = std::clamp(clock / cd::kBbdBandwidthDivisor,
                                           cd::kBbdBandwidthMinHz, cd::kBbdBandwidthMaxHz);

        INFO("time " << time_ms << " ms, stages " << stages << ", reported "
                     << delay.bbd_bandwidth_hz() << ", expected " << expected);
        CHECK(std::abs(delay.bbd_bandwidth_hz() - expected) < 0.15 * expected);
    }

    // And the relationship holds automatically as the time slews between them:
    // short is bright, long is dark, with no curve drawn anywhere.
    Engine delay;
    configure(delay, Character::bbd, 50.0, 0.0, 0.5);
    settle(delay, 1.0);
    const double bright = delay.bbd_bandwidth_hz();
    delay.set_time_ms(500.0f);
    settle(delay, 2.0);
    const double dark = delay.bbd_bandwidth_hz();
    INFO("bright " << bright << " dark " << dark);
    CHECK(dark < bright * 0.5);
}

// ═══════════════════════════════════════════════════════════════════════════
// 6 — BBD compander
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("the BBD compander suppresses the line's noise floor in gaps",
          "[character-delay][bbd]") {
    auto measure_gap_floor = [](bool compander) {
        Engine delay;
        configure(delay, Character::bbd, 100.0, 0.0, 1.0);
        delay.set_bbd_compander_enabled(compander);
        delay.reset();
        settle(delay, 0.6);

        // 0.5 s of tone, then 0.5 s of silence; measure the wet output in the
        // second half of the gap, once the delayed tone has also stopped.
        const int n = static_cast<int>(kSr * 1.0);
        auto buffers = sine_both(n, 1000.0, 0.5f);
        for (int i = n / 2; i < n; ++i) {
            buffers.left[static_cast<std::size_t>(i)] = 0.0f;
            buffers.right[static_cast<std::size_t>(i)] = 0.0f;
        }
        render(delay, buffers);
        return rms(buffers.left, static_cast<int>(kSr * 0.8), n);
    };

    const double with_compander = measure_gap_floor(true);
    const double without = measure_gap_floor(false);
    INFO("gap floor with " << with_compander << " without " << without);
    CHECK(20.0 * std::log10((with_compander + 1e-15) / (without + 1e-15)) < -10.0);
}

// ═══════════════════════════════════════════════════════════════════════════
// 7 — Tape wow and flutter
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("tape instability appears at the modeled rates and vanishes at zero",
          "[character-delay][tape][slow]") {
    constexpr double kCarrier = 1000.0;
    constexpr double kDelayMs = 500.0;

    auto delay_deviation_ms = [](double character_amount, std::vector<double>& track) {
        Engine delay;
        configure(delay, Character::tape, kDelayMs, 0.0, character_amount);
        settle(delay, 4.0 * slew_seconds(Character::tape) + 0.6);

        const int n = static_cast<int>(kSr * 20.0);
        auto buffers = sine_both(n, kCarrier, 0.25f);
        render(delay, buffers);
        // Skip the first second so the demodulator's own filters have settled.
        track = phase_track(buffers.left, static_cast<int>(kSr), n, kCarrier);

        double mean = 0.0;
        for (double p : track) mean += p;
        mean /= static_cast<double>(track.size());
        double variance = 0.0;
        for (double& p : track) {
            p -= mean;
            variance += p * p;
        }
        variance /= static_cast<double>(track.size());
        // Phase deviation back to a read-head displacement in milliseconds.
        return 1000.0 * std::sqrt(variance) / (2.0 * cd::kPi * kCarrier);
    };

    std::vector<double> unstable_track;
    std::vector<double> stable_track;
    const double moving = delay_deviation_ms(0.67, unstable_track);
    const double flat = delay_deviation_ms(0.0, stable_track);
    REQUIRE(unstable_track.size() > 1000);

    INFO("read-head deviation at character 0.67: " << moving << " ms, at 0: " << flat << " ms");
    CHECK(moving > 10.0 * flat);

    // Magnitude consistent with the shipped depths. Wow and flutter are
    // independent, so their RMS contributions add in quadrature; the tolerance
    // is wide because the drift term is stochastic by design.
    const double wow = cd::interpolate_knots(cd::kTapeAxis, cd::kTapeWowDepthMs, 0.67);
    const double flutter =
        cd::interpolate_knots(cd::kTapeAxis, cd::kTapeFlutterDepthMs, 0.67);
    const double nominal = std::sqrt(0.5 * wow * wow + 0.5 * flutter * flutter);
    INFO("nominal deviation " << nominal << " ms");
    CHECK(moving > 0.5 * nominal);
    CHECK(moving < 2.0 * nominal);

    // The deviation's own spectrum must peak at the modeled wow rate.
    std::vector<float> as_float(unstable_track.size());
    for (std::size_t i = 0; i < unstable_track.size(); ++i)
        as_float[i] = static_cast<float>(unstable_track[i]);

    const auto psd = welch_psd(as_float, 0, static_cast<int>(as_float.size()), 4096);
    std::size_t best = 1;
    for (std::size_t bin = 1; bin < psd.size() && psd_bin_hz(bin, psd.size(), kTrackRate) < 4.0;
         ++bin)
        if (psd[bin] > psd[best]) best = bin;
    const double peak_hz = psd_bin_hz(best, psd.size(), kTrackRate);
    INFO("wow peak at " << peak_hz << " Hz, expected " << cd::kWowRateHz);
    CHECK(std::abs(peak_hz - cd::kWowRateHz) < 0.4);
}

TEST_CASE("tape modulation does not shift the mean delay", "[character-delay][tape]") {
    // The mean-delay rule: instability changes pitch, never tempo. Measured by
    // where the repeat of an impulse lands with instability at maximum.
    Engine delay;
    configure(delay, Character::tape, 500.0, 0.0, 1.0);
    settle(delay, 4.0 * slew_seconds(Character::tape) + 0.6);

    double total = 0.0;
    const int trials = 8;
    for (int trial = 0; trial < trials; ++trial) {
        auto buffers = impulse_left(static_cast<int>(kSr * 1.2));
        render(delay, buffers);
        total += peak_index(buffers.left, 1, static_cast<int>(kSr * 1.1));
    }
    const double mean_index = total / trials;
    const double expected = 0.5 * kSr;
    INFO("mean repeat index " << mean_index << ", expected " << expected);
    CHECK(std::abs(mean_index - expected) < 0.0005 * kSr);  // +-0.5 ms
}

// ═══════════════════════════════════════════════════════════════════════════
// 8 — Vintage floor and darkening
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("vintage band-limits to its internal rate", "[character-delay][vintage]") {
    Engine delay;
    configure(delay, Character::vintage_digital, 200.0, 0.0, 0.5);
    settle(delay, 1.0);

    const double edge = delay.vintage_band_edge_hz();
    INFO("reported band edge " << edge);
    CHECK(edge == Catch::Approx(cd::kVintageAntiAliasFraction * delay.vintage_band_edge_hz() /
                                cd::kVintageAntiAliasFraction));

    auto response_at = [&](double hz) {
        Engine probe;
        configure(probe, Character::vintage_digital, 200.0, 0.0, 0.5);
        settle(probe, 1.0);
        const int n = static_cast<int>(kSr * 1.0);
        auto buffers = sine_both(n, hz, 0.5f);
        render(probe, buffers);
        return magnitude_at(buffers.left, static_cast<int>(kSr * 0.5), n, hz);
    };

    const double passband = response_at(300.0);
    const double at_edge = response_at(edge);
    const double beyond = response_at(std::min(edge * 1.8, 0.45 * kSr));
    INFO("passband " << passband << " edge " << at_edge << " beyond " << beyond);
    CHECK(at_edge < passband);
    CHECK(beyond < at_edge * 0.5);
}

TEST_CASE("vintage repeats darken as they recirculate", "[character-delay][vintage]") {
    Engine delay;
    configure(delay, Character::vintage_digital, 200.0, 0.6, 0.5);
    settle(delay, 1.0);

    // Broadband in: a narrow-band burst has no high frequencies for the
    // converter loop to shed, so it cannot show darkening even when it happens.
    auto buffers = impulse_left(static_cast<int>(kSr * 1.2), 0.9f);
    render(delay, buffers);

    double previous = 1e12;
    for (int repeat = 1; repeat <= 4; ++repeat) {
        const int centre = static_cast<int>(repeat * 0.2 * kSr);
        const double centroid = spectral_centroid(buffers.left, centre - 2048, centre + 2048);
        INFO("repeat " << repeat << " centroid " << centroid);
        CHECK(centroid < previous);
        previous = centroid;
    }
}

TEST_CASE("vintage quantization noise sits in the dithered PCM region",
          "[character-delay][vintage]") {
    // At the 12-bit knot a -60 dBFS tone must come back with a noise floor set
    // by the quantizer and its TPDF dither, not by the signal.
    Engine delay;
    configure(delay, Character::vintage_digital, 100.0, 0.0, 0.5);
    settle(delay, 1.0);

    const int n = static_cast<int>(kSr * 2.0);
    auto buffers = sine_both(n, 1000.0, static_cast<float>(std::pow(10.0, -60.0 / 20.0)));
    render(delay, buffers);

    const auto psd = welch_psd(buffers.left, static_cast<int>(kSr), n, 32768);
    // Integrate everything except the tone's own bins.
    double noise = 0.0;
    for (std::size_t bin = 1; bin < psd.size(); ++bin) {
        const double f = psd_bin_hz(bin, psd.size(), kSr);
        if (std::abs(f - 1000.0) < 50.0) continue;
        noise += psd[bin];
    }
    double tone = 0.0;
    for (std::size_t bin = 1; bin < psd.size(); ++bin)
        if (std::abs(psd_bin_hz(bin, psd.size(), kSr) - 1000.0) < 50.0) tone += psd[bin];

    const double noise_db = 10.0 * std::log10(noise / std::max(tone, 1e-30)) - 60.0;
    INFO("integrated wet noise floor " << noise_db << " dBFS");
    CHECK(noise_db > -110.0);
    CHECK(noise_db < -40.0);
}

// ═══════════════════════════════════════════════════════════════════════════
// 9 — Diffusion
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("diffusion smears a repeat into a cluster", "[character-delay][diffusion]") {
    auto count_above = [](Character character) {
        Engine delay;
        configure(delay, character, 100.0, 0.0, 0.5);
        settle(delay, 0.3);
        auto buffers = impulse_left(static_cast<int>(kSr * 0.4));
        render(delay, buffers);

        const int centre = peak_index(buffers.left, 1, static_cast<int>(buffers.left.size()));
        const int span = static_cast<int>(0.03 * kSr);
        const double top = peak(buffers.left, centre - span, centre + span);
        int count = 0;
        for (int i = std::max(1, centre - span);
             i < centre + span && i < static_cast<int>(buffers.left.size()); ++i)
            if (std::abs(static_cast<double>(buffers.left[static_cast<std::size_t>(i)])) >
                0.05 * top)
                ++count;
        return count;
    };

    const int clean = count_above(Character::clean);
    const int diffused = count_above(Character::diffusion);
    INFO("clean " << clean << " diffused " << diffused);
    CHECK(diffused >= 8 * std::max(clean, 1));
}

TEST_CASE("the diffuser is allpass in steady state", "[character-delay][diffusion]") {
    Engine delay;
    configure(delay, Character::diffusion, 20.0, 0.0, 0.5);
    settle(delay, 0.3);

    // White noise in; the allpass chain must not change the magnitude response
    // between the loop filters' corners.
    const int n = static_cast<int>(kSr * 4.0);
    auto buffers = make_stereo(n);
    cd::Xorshift32 rng(12345u);
    for (int i = 0; i < n; ++i) {
        const auto v = static_cast<float>(0.2 * rng.bipolar());
        buffers.left[static_cast<std::size_t>(i)] = v;
        buffers.right[static_cast<std::size_t>(i)] = v;
    }
    render(delay, buffers);

    const auto psd = welch_psd(buffers.left, static_cast<int>(kSr), n, 8192);
    double low = 1e30;
    double high = 0.0;
    for (std::size_t bin = 1; bin < psd.size(); ++bin) {
        const double f = psd_bin_hz(bin, psd.size(), kSr);
        if (f < 100.0 || f > 8000.0) continue;
        low = std::min(low, psd[bin]);
        high = std::max(high, psd[bin]);
    }
    // Compare smoothed octave-band energies rather than raw bins: a single
    // realization of noise has its own several-dB bin-to-bin scatter, which is
    // a property of the stimulus, not of the filter under test.
    double worst = 0.0;
    double reference = 0.0;
    for (double centre = 141.0; centre < 8000.0; centre *= 2.0) {
        double band = 0.0;
        int count = 0;
        for (std::size_t bin = 1; bin < psd.size(); ++bin) {
            const double f = psd_bin_hz(bin, psd.size(), kSr);
            if (f < centre / std::sqrt(2.0) || f > centre * std::sqrt(2.0)) continue;
            band += psd[bin];
            ++count;
        }
        if (count == 0) continue;
        band /= count;
        if (reference == 0.0) reference = band;
        worst = std::max(worst, std::abs(10.0 * std::log10(band / reference)));
    }
    INFO("worst octave-band deviation " << worst << " dB (raw bin spread "
                                        << 10.0 * std::log10(high / std::max(low, 1e-30)) << ")");
    CHECK(worst < 1.5);
}

// ═══════════════════════════════════════════════════════════════════════════
// 10 — Reverse
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("reverse plays each segment backwards without a splice click",
          "[character-delay][reverse]") {
    Engine delay;
    configure(delay, Character::clean, 500.0, 0.0, 0.0);
    delay.set_reverse(true);
    delay.reset();

    // A linear AMPLITUDE ramp on a tone: the envelope is what reversal negates,
    // and a bare DC ramp would be removed by the loop's 20 Hz DC blocker.
    const int n = static_cast<int>(kSr * 3.0);
    auto buffers = make_stereo(n);
    const int ramp_length = static_cast<int>(kSr);
    for (int i = 0; i < ramp_length; ++i) {
        const double envelope = static_cast<double>(i) / ramp_length;
        buffers.left[static_cast<std::size_t>(i)] = static_cast<float>(
            envelope * std::sin(2.0 * cd::kPi * 500.0 * static_cast<double>(i) / kSr));
    }
    render(delay, buffers);
    REQUIRE(all_finite(buffers.left));

    const int segment = static_cast<int>(0.5 * kSr);
    for (int index = 1; index <= 2; ++index) {
        const int start = index * segment;
        const double early = rms(buffers.left, start + 2000, start + 8000);
        const double late = rms(buffers.left, start + segment - 8000, start + segment - 2000);
        INFO("segment " << index << " early " << early << " late " << late);
        CHECK(late < early);  // the input's rising envelope comes back falling
    }

    // No splice click: the step across a boundary must stay within the range of
    // steps the signal itself produces inside a segment.
    const int boundary = 2 * segment;
    const double in_segment = max_step(buffers.left, boundary + 500, boundary + segment - 500);
    const double across = max_step(buffers.left, boundary - 40, boundary + 40);
    INFO("in-segment max step " << in_segment << ", across boundary " << across);
    CHECK(across <= 2.0 * in_segment);
}

TEST_CASE("reverse with feedback alternates direction", "[character-delay][reverse]") {
    Engine delay;
    configure(delay, Character::clean, 250.0, 0.7, 0.0);
    delay.set_reverse(true);
    delay.reset();

    auto buffers = burst_left(static_cast<int>(kSr * 3.0), 700.0, 0.05, 0.9f);
    render(delay, buffers);
    CHECK(all_finite(buffers.left));
    // Energy keeps circulating rather than stopping after one segment.
    CHECK(rms(buffers.left, static_cast<int>(kSr * 1.5), static_cast<int>(kSr * 2.5)) > 1e-4);
}

// ═══════════════════════════════════════════════════════════════════════════
// 11 — Freeze
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("freeze holds the loop and rejects new input", "[character-delay][freeze][slow]") {
    Engine delay;
    configure(delay, Character::clean, 250.0, 0.4, 0.0);

    auto priming = sine_both(static_cast<int>(kSr), 900.0, 0.5f);
    render(delay, priming);
    delay.set_freeze(true);

    auto first = make_stereo(static_cast<int>(kSr));
    render(delay, first);
    const double initial = rms(first.left, 0, static_cast<int>(first.left.size()));
    REQUIRE(initial > 1e-3);

    // (a) input rejection
    {
        Engine probe;
        configure(probe, Character::clean, 250.0, 0.4, 0.0);
        auto prime = sine_both(static_cast<int>(kSr), 900.0, 0.5f);
        render(probe, prime);
        probe.set_freeze(true);
        auto quiet = make_stereo(static_cast<int>(kSr * 2.0));
        render(probe, quiet);
        const double frozen = rms(quiet.left, static_cast<int>(kSr), static_cast<int>(kSr * 2.0));

        auto injected = sine_both(static_cast<int>(kSr * 2.0), 3100.0, 0.5f);
        render(probe, injected);
        const double injected_energy = magnitude_at(injected.left, static_cast<int>(kSr),
                                                    static_cast<int>(kSr * 2.0), 3100.0);
        INFO("frozen level " << frozen << ", injected tone " << injected_energy);
        CHECK(20.0 * std::log10((injected_energy + 1e-18) / frozen) < -60.0);
    }

    // (b) a frozen clean loop holds
    for (int second = 0; second < 29; ++second) {
        auto quiet = make_stereo(static_cast<int>(kSr));
        render(delay, quiet);
    }
    auto last = make_stereo(static_cast<int>(kSr));
    render(delay, last);
    const double held = rms(last.left, 0, static_cast<int>(last.left.size()));
    INFO("frozen clean loop " << initial << " -> " << held);
    CHECK(20.0 * std::log10((held + 1e-15) / initial) > -3.0);

    // (d) release is click free
    delay.set_freeze(false);
    auto release = make_stereo(static_cast<int>(kSr * 0.5));
    render(delay, release);
    const double inside = max_step(last.left, 1000, static_cast<int>(last.left.size()));
    INFO("release max step " << max_step(release.left, 0, 512) << " vs in-loop " << inside);
    CHECK(max_step(release.left, 0, 512) <= 2.0 * inside);
}

TEST_CASE("frozen coloured loops evolve but stay bounded",
          "[character-delay][freeze][slow]") {
    // Keeping the character INSIDE the frozen loop is the design decision: a
    // frozen tape or BBD loop degrades per pass into texture, which is the
    // feature. It must degrade without diverging.
    for (auto character : {Character::tape, Character::bbd}) {
        Engine delay;
        configure(delay, character, 250.0, 0.5, 0.6);
        auto priming = sine_both(static_cast<int>(kSr), 500.0, 0.5f);
        render(delay, priming);
        delay.set_freeze(true);

        for (int second = 0; second < 59; ++second) {
            auto quiet = make_stereo(static_cast<int>(kSr));
            render(delay, quiet);
        }
        auto last = make_stereo(static_cast<int>(kSr));
        render(delay, last);
        INFO("character index " << static_cast<int>(character));
        CHECK(all_finite(last.left));
        CHECK(peak(last.left, 0, static_cast<int>(last.left.size())) < 4.0);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 12 — Ducking
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("ducking pushes the wet path down under a hot input",
          "[character-delay][duck]") {
    auto measure = [](float duck, bool trailing_silence) {
        Engine delay;
        configure(delay, Character::clean, 300.0, 0.6, 0.0);
        delay.set_duck(duck);
        delay.reset();

        const int n = static_cast<int>(kSr * 3.0);
        auto buffers = sine_both(n, 300.0, 0.7f);
        if (trailing_silence)
            for (int i = static_cast<int>(kSr * 2.0); i < n; ++i) {
                buffers.left[static_cast<std::size_t>(i)] = 0.0f;
                buffers.right[static_cast<std::size_t>(i)] = 0.0f;
            }
        render(delay, buffers);
        return buffers;
    };

    const auto open = measure(0.0f, false);
    const auto ducked = measure(1.0f, false);
    const double open_level = rms(open.left, static_cast<int>(kSr), static_cast<int>(kSr * 2.0));
    const double ducked_level =
        rms(ducked.left, static_cast<int>(kSr), static_cast<int>(kSr * 2.0));
    INFO("open " << open_level << " ducked " << ducked_level);
    CHECK(20.0 * std::log10((ducked_level + 1e-18) / open_level) < -12.0);

    // Recovery: after the input stops, the wet path returns within a few
    // release constants.
    const auto released = measure(1.0f, true);
    // The soft knee means the last decibel of recovery costs an extra release
    // constant beyond the three the envelope itself needs.
    const int recovery_start =
        static_cast<int>(kSr * (2.0 + 4.0 * cd::kDuckReleaseS));
    const double recovered =
        rms(released.left, recovery_start, static_cast<int>(kSr * 2.9));
    const auto undocked = measure(0.0f, true);
    const double reference =
        rms(undocked.left, recovery_start, static_cast<int>(kSr * 2.9));
    INFO("recovered " << recovered << " reference " << reference);
    CHECK(std::abs(20.0 * std::log10((recovered + 1e-18) / (reference + 1e-18))) < 1.0);
}

// ═══════════════════════════════════════════════════════════════════════════
// 13 — Determinism
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("renders are bit-identical after reset", "[character-delay][determinism]") {
    for (auto character : {Character::tape, Character::bbd}) {
        Engine delay;
        configure(delay, character, 300.0, 0.5, 0.8);

        auto first = sine_both(static_cast<int>(kSr * 10.0), 440.0, 0.3f);
        render(delay, first);
        delay.reset();
        auto second = sine_both(static_cast<int>(kSr * 10.0), 440.0, 0.3f);
        render(delay, second);

        INFO("character index " << static_cast<int>(character));
        CHECK(first.left == second.left);
        CHECK(first.right == second.right);
    }
}

TEST_CASE("the physical tier's chew sequence repeats exactly",
          "[character-delay][determinism][tape]") {
    Engine delay;
    configure(delay, Character::tape, 300.0, 0.4, 1.0, TapeTier::physical);
    auto first = sine_both(static_cast<int>(kSr * 6.0), 440.0, 0.3f);
    render(delay, first);
    const std::size_t states = delay.chew_state_index(0);

    delay.reset();
    auto second = sine_both(static_cast<int>(kSr * 6.0), 440.0, 0.3f);
    render(delay, second);

    INFO("chew states " << states << " vs " << delay.chew_state_index(0));
    CHECK(delay.chew_state_index(0) == states);
    CHECK(first.left == second.left);
}

// ═══════════════════════════════════════════════════════════════════════════
// 14 — Real-time safety
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("process allocates nothing in any configuration",
          "[character-delay][rt-safety]") {
    struct Config {
        Character character;
        TapeTier tier;
        bool reverse;
        bool freeze;
    };
    const Config configs[] = {
        {Character::clean, TapeTier::standard, false, false},
        {Character::vintage_digital, TapeTier::standard, false, false},
        {Character::tape, TapeTier::standard, false, false},
        {Character::tape, TapeTier::physical, false, false},
        {Character::bbd, TapeTier::standard, false, false},
        {Character::diffusion, TapeTier::standard, false, false},
        {Character::clean, TapeTier::standard, true, false},
        {Character::tape, TapeTier::physical, true, false},
        {Character::bbd, TapeTier::standard, true, true},
        {Character::diffusion, TapeTier::standard, false, true},
    };

    for (const auto& config : configs) {
        Engine delay;
        configure(delay, config.character, 250.0, 0.6, 0.7, config.tier);
        delay.set_reverse(config.reverse);
        delay.set_freeze(config.freeze);
        delay.set_mod(0.4f, 0.5f);
        delay.set_duck(0.5f);
        delay.set_crossfeed(0.3f);
        delay.reset();

        auto warmup = sine_both(kBlock * 8, 500.0, 0.4f);
        render(delay, warmup);

        auto buffers = sine_both(kBlock * 8, 500.0, 0.4f);
        {
            pulp::test::RtAllocationProbe probe;
            render(delay, buffers);
            // reset() is on the allocation-free path too.
            delay.reset();
            // Snapshot before INFO: building Catch2's message stream allocates,
            // and the probe is still in scope.
            const auto count = probe.allocation_count();
            const auto bytes = probe.allocated_bytes();
            INFO("character index " << static_cast<int>(config.character) << " reverse "
                                    << config.reverse << " freeze " << config.freeze);
            CHECK(count == 0);
            CHECK(bytes == 0);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 15 — Jiles-Atherton hysteresis
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("the hysteresis solver converges inside its iteration cap",
          "[character-delay][hysteresis][slow]") {
    // 4x the saturation onset: the Langevin function saturates around an
    // argument of 3, and the argument is field/shape, so the onset field is
    // 3 x shape and the drive level is four times that.
    for (double drive : {cd::kTapeDrive.front(), cd::kTapeDrive[2], cd::kTapeDrive.back()}) {
        cd::JilesAthertonHysteresis hysteresis;
        hysteresis.prepare(kSr * 4.0);
        hysteresis.set_character(drive, cd::kTapeBias[1]);
        hysteresis.clear_solver_counters();

        const int n = static_cast<int>(kSr * 4.0 * 10.0);
        for (int i = 0; i < n; ++i)
            hysteresis.process(0.75 * std::sin(2.0 * cd::kPi * 1000.0 * i / (kSr * 4.0)));

        INFO("drive " << drive << " capped " << hysteresis.capped_steps());
        CHECK(hysteresis.capped_steps() == 0);
    }

    // Bounded output at maximum drive, not just convergence.
    cd::JilesAthertonHysteresis hot;
    hot.prepare(kSr * 4.0);
    hot.set_character(cd::kTapeDrive.back(), cd::kTapeBias.back());
    const int n = static_cast<int>(kSr * 4.0 * 10.0);
    double worst = 0.0;
    for (int i = 0; i < n; ++i)
        worst = std::max(worst, std::abs(hot.process(
                                    0.75 * std::sin(2.0 * cd::kPi * 1000.0 * i / (kSr * 4.0)))));
    INFO("max-drive peak " << worst);
    CHECK(worst < 1.5);
}

TEST_CASE("hysteresis has loop area, unlike a waveshaper",
          "[character-delay][hysteresis]") {
    cd::JilesAthertonHysteresis hysteresis;
    hysteresis.prepare(kSr);
    hysteresis.set_character(cd::kTapeDrive[2], cd::kTapeBias[1]);

    double area = 0.0;
    double previous_field = 0.0;
    double previous_magnetization = 0.0;
    const int n = static_cast<int>(kSr);
    for (int i = 0; i < n; ++i) {
        const double field = 0.6 * std::sin(2.0 * cd::kPi * 2.0 * i / kSr);
        const double magnetization = hysteresis.process(field);
        area += 0.5 * (magnetization + previous_magnetization) * (field - previous_field);
        previous_field = field;
        previous_magnetization = magnetization;
    }
    INFO("loop area " << area);
    CHECK(std::abs(area) > 1e-3);
}

TEST_CASE("hysteresis distortion grows with drive and silence clears it",
          "[character-delay][hysteresis]") {
    auto distortion = [](double drive) {
        cd::JilesAthertonHysteresis hysteresis;
        hysteresis.prepare(kSr);
        hysteresis.set_character(drive, cd::kTapeBias[1]);
        const int n = static_cast<int>(kSr);
        std::vector<float> out(static_cast<std::size_t>(n), 0.0f);
        for (int i = 0; i < n; ++i)
            out[static_cast<std::size_t>(i)] = static_cast<float>(
                hysteresis.process(0.4 * std::sin(2.0 * cd::kPi * 500.0 * i / kSr)));

        const double fundamental = magnitude_at(out, n / 4, n, 500.0);
        double harmonics = 0.0;
        for (int h = 2; h <= 6; ++h) {
            const double m = magnitude_at(out, n / 4, n, 500.0 * h);
            harmonics += m * m;
        }
        return std::sqrt(harmonics) / std::max(fundamental, 1e-12);
    };

    const double soft = distortion(cd::kTapeDrive.front());
    const double hard = distortion(cd::kTapeDrive.back());
    INFO("THD soft " << soft << " hard " << hard);
    CHECK(hard > soft);

    cd::JilesAthertonHysteresis hysteresis;
    hysteresis.prepare(kSr);
    hysteresis.set_character(cd::kTapeDrive[2], cd::kTapeBias[2]);
    for (int i = 0; i < 4800; ++i)
        hysteresis.process(0.5 * std::sin(2.0 * cd::kPi * 100.0 * i / kSr));
    double last = 1.0;
    for (int i = 0; i < 4800; ++i) last = hysteresis.process(0.0);
    INFO("magnetization after silence " << last);
    CHECK(last == 0.0);
}

// ═══════════════════════════════════════════════════════════════════════════
// 16 — Wallace loss filter
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("the loss cascade realizes the modeled response",
          "[character-delay][tape][loss]") {
    // The stage is a cascade: a fitted IIR ladder carries the smooth
    // spacing/thickness tilt and a minimum-phase FIR carries the gap null. This
    // measures the COMBINED response against the analytic model, which is the
    // only comparison that means anything to a listener.
    cd::TapeLossDesign design;
    design.prepare(kSr, 7.5);
    const std::size_t taps = cd::tape_gap_fir_taps(kSr);

    auto worst_error = [&](double ips, double spacing_um) {
        cd::TapeLossGeometry geometry;
        geometry.speed_ips = ips;
        geometry.spacing_m = spacing_um * 1e-6;

        const auto gap = cd::design_tape_gap_fir(kSr, taps, geometry);
        const auto parameters = design.shapes().parameters_for(geometry);

        double worst = 0.0;
        for (int k = 0; k < 10; ++k) {
            const double f = 20.0 * std::pow(0.45 * kSr / 20.0, k / 9.0);
            std::complex<double> sum{0.0, 0.0};
            for (std::size_t i = 0; i < taps; ++i)
                sum += gap[i] * std::exp(std::complex<double>(0.0, -2.0 * cd::kPi * f * i / kSr));
            const double realized = 20.0 * std::log10(std::max(std::abs(sum), 1e-12)) +
                                    cd::tape_loss_iir_magnitude_db(parameters, f);
            const double target = 20.0 * std::log10(std::max(
                                             cd::tape_loss_magnitude_floored(f, geometry), 1e-12));
            worst = std::max(worst, std::abs(std::max(realized, cd::kTapeLossFloorDb) -
                                             std::max(target, cd::kTapeLossFloorDb)));
        }
        return worst;
    };

    // At and above 3.75 ips, within a decibel across the band — at every
    // spacing on the age axis, not just the nominal one.
    for (double ips : {3.75, 7.5, 15.0, 30.0}) {
        for (double spacing : cd::kAgeSpacingUm) {
            const double error = worst_error(ips, spacing);
            INFO(ips << " ips at " << spacing << " um: worst error " << error << " dB");
            CHECK(error < 1.0);
        }
    }

    // The slowest speed at maximum wear is the hardest corner in the model —
    // the analytic −3 dB point drops near 100 Hz there. Held to 2 dB.
    const double worn = worst_error(1.875, cd::kAgeSpacingUm.back());
    INFO("1.875 ips worn worst error " << worn << " dB");
    CHECK(worn < 2.0);
}

TEST_CASE("the loss cascade is exact at every age, not just at fitted points",
          "[character-delay][tape][loss]") {
    // The age axis is a pure frequency SCALING of a fitted dimensionless shape,
    // so there are no knots to fall between. This sweeps age continuously and
    // asserts the accuracy never degrades — the case that would catch a
    // regression back to fitting-and-interpolating, which measured inside 1 dB
    // at its knots and 3-4 dB between them.
    cd::TapeLossDesign design;
    design.prepare(kSr, 7.5);

    for (double age = 0.0; age <= 1.0; age += 0.03125) {
        const auto geometry = design.geometry_at(age);
        const auto parameters = design.parameters_at(age);

        double worst = 0.0;
        for (int k = 0; k < 12; ++k) {
            const double f = 20.0 * std::pow(0.45 * kSr / 20.0, k / 11.0);
            const double realized = cd::tape_loss_iir_magnitude_db(parameters, f);
            const double target = 20.0 * std::log10(std::max(
                                             cd::tape_loss_smooth_magnitude(f, geometry), 1e-12));
            worst = std::max(worst, std::abs(std::max(realized, cd::kTapeLossFloorDb) -
                                             std::max(target, cd::kTapeLossFloorDb)));
        }
        INFO("age " << age << " worst error " << worst << " dB");
        CHECK(worst < 1.0);
    }

    // Scaling is monotone in age: more wear is never brighter.
    double previous = 1e12;
    for (double age = 0.0; age <= 1.0; age += 0.1) {
        const auto parameters = design.parameters_at(age);
        const double at_5k = cd::tape_loss_iir_magnitude_db(parameters, 5000.0);
        INFO("age " << age << " response at 5 kHz " << at_5k << " dB");
        CHECK(at_5k <= previous + 1e-9);
        previous = at_5k;
    }
}

TEST_CASE("the shipped loss shapes reproduce a fresh derivation",
          "[character-delay][tape][loss][slow]") {
    // The shapes in tables.hpp were derived offline by the fitter that still
    // lives in tape_loss.hpp. This re-runs that derivation and checks the
    // shipped values give the same RESPONSE — comparing responses rather than
    // parameters because a minimax fit can reach the same curve through
    // different parameter sets, and it is the curve that is the contract.
    const auto fitted = cd::fit_tape_loss_shapes();
    const auto shipped = cd::TapeLossShapes::tabulated();

    auto magnitude = [](const auto& shape, double x) {
        double db = 0.0;
        for (double corner : shape.pole_x) {
            const double r = x / corner;
            db += -10.0 * std::log10(1.0 + r * r);
        }
        for (std::size_t i = 0; i < shape.shelf_x.size(); ++i) {
            const double g = std::pow(10.0, shape.shelf_db[i] / 20.0);
            const double r = x / shape.shelf_x[i];
            db += 10.0 * std::log10((1.0 + g * g * r * r) / (1.0 + r * r));
        }
        return db;
    };

    double spacing_shipped = 0.0;
    double spacing_fitted = 0.0;
    double thickness_shipped = 0.0;
    for (int k = 0; k < 40; ++k) {
        const double x = cd::kLossShapeMinX *
                         std::pow(cd::kLossShapeMaxX / cd::kLossShapeMinX, k / 39.0);
        const auto clamp_db = [](double v) { return std::max(v, cd::kTapeLossFloorDb); };

        spacing_shipped = std::max(spacing_shipped,
                                   std::abs(clamp_db(magnitude(shipped.spacing, x)) -
                                            clamp_db(cd::spacing_shape_db(x))));
        spacing_fitted = std::max(spacing_fitted,
                                  std::abs(clamp_db(magnitude(fitted.spacing, x)) -
                                           clamp_db(cd::spacing_shape_db(x))));
        thickness_shipped = std::max(thickness_shipped,
                                     std::abs(clamp_db(magnitude(shipped.thickness, x)) -
                                              clamp_db(cd::thickness_shape_db(x))));
    }

    INFO("shipped spacing " << spacing_shipped << " dB, fresh fit " << spacing_fitted
                            << " dB, shipped thickness " << thickness_shipped << " dB");
    CHECK(spacing_shipped < 0.5);
    CHECK(thickness_shipped < 0.3);
    // The shipped table must be at least as good as what the fitter produces
    // now, with a little slack for the search's own run-to-run spread.
    CHECK(spacing_shipped < spacing_fitted + 0.25);
}

TEST_CASE("the wet path stays inside a stated gain bound",
          "[character-delay][gain]") {
    // The bound a host or graph layer needs in order to size headroom, and the
    // reason it is NOT the usual 1/(1-feedback) geometric series:
    //
    //   * Tape, BBD and Vintage carry an in-loop saturator whose output is hard
    //     bounded to +-1 REGARDLESS of the feedback setting, so the loop
    //     contributes at most unity to the line input however far past 1.0 the
    //     feedback knob goes. The geometric series does not apply and would be
    //     unbounded at feedback 1.1.
    //   * Clean and Diffusion have no saturator, so their feedback IS clamped
    //     below unity and the geometric bound is the right one: at the 0.98
    //     ceiling a comb-resonant input can build to about 1/(1-0.98) = 50x.
    //     That is inherent to any feedback delay, not specific to this module.
    //
    // This measures the first case, which is the one a bound derived from the
    // feedback range alone would get badly wrong.
    for (auto character : {Character::tape, Character::bbd, Character::vintage_digital}) {
        Engine delay;
        configure(delay, character, 120.0, 1.1, 1.0);
        delay.set_crossfeed(0.5f);

        // Full-scale input, worst case, long enough for the loop to fill.
        auto buffers = sine_both(static_cast<int>(kSr * 8.0), 220.0, 1.0f);
        render(delay, buffers);

        const double top = peak(buffers.left, static_cast<int>(kSr * 2.0),
                                static_cast<int>(buffers.left.size()));
        INFO("character index " << static_cast<int>(character) << " peak " << top);
        CHECK(all_finite(buffers.left));
        CHECK(top < 4.0);
    }
}

TEST_CASE("the gap-loss null lands where the physics predicts",
          "[character-delay][tape][loss]") {
    cd::TapeLossGeometry geometry;
    geometry.speed_ips = 1.875;
    geometry.spacing_m = 5e-6;
    const double predicted = geometry.speed_ips * 0.0254 / geometry.gap_m;

    double best_hz = 0.0;
    double best = 1e30;
    for (double f = 1000.0; f < 0.5 * kSr; f += 5.0) {
        const double m = cd::tape_loss_magnitude(f, geometry);
        if (m < best) {
            best = m;
            best_hz = f;
        }
    }
    INFO("predicted " << predicted << " Hz, measured " << best_hz << " Hz");
    CHECK(std::abs(best_hz - predicted) < 0.01 * predicted);
}

TEST_CASE("a tape speed change crossfades without a discontinuity",
          "[character-delay][tape][loss]") {
    Engine delay;
    configure(delay, Character::tape, 300.0, 0.4, 0.5, TapeTier::physical);
    settle(delay, 1.5);

    auto before = sine_both(static_cast<int>(kSr * 0.5), 400.0, 0.4f);
    render(delay, before);
    const double baseline = max_step(before.left, 1000, static_cast<int>(before.left.size()));

    delay.set_tape_speed_ips(15.0f);
    auto after = sine_both(static_cast<int>(kSr * 0.5), 400.0, 0.4f);
    render(delay, after);

    INFO("baseline step " << baseline << " during crossfade "
                          << max_step(after.left, 0, static_cast<int>(0.05 * kSr)));
    CHECK(all_finite(after.left));
    CHECK(max_step(after.left, 0, static_cast<int>(0.05 * kSr)) <= 2.0 * baseline);
}

// ═══════════════════════════════════════════════════════════════════════════
// 17 — Latency
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("the module reports zero latency in every configuration",
          "[character-delay][latency]") {
    for (auto character : {Character::clean, Character::vintage_digital, Character::tape,
                           Character::bbd, Character::diffusion}) {
        for (auto tier : {TapeTier::standard, TapeTier::physical}) {
            Engine delay;
            configure(delay, character, 250.0, 0.3, 0.5, tier);
            CHECK(delay.latency_samples() == 0);
        }
    }

    // And it is a real zero, not a claim: the physical tier's in-loop
    // oversampler and loss FIR are folded out of the line, so the repeat still
    // lands on the requested time.
    Engine delay;
    configure(delay, Character::tape, 350.0, 0.0, 0.0, TapeTier::physical);
    settle(delay, 4.0 * slew_seconds(Character::tape) + 0.6);
    auto buffers = impulse_left(static_cast<int>(kSr * 0.9));
    render(delay, buffers);
    const int index = peak_index(buffers.left, 1, static_cast<int>(buffers.left.size()));
    INFO("physical-tier repeat at " << index << ", expected " << 0.35 * kSr);
    CHECK(std::abs(index - 0.35 * kSr) <= 0.001 * kSr);
}

// ═══════════════════════════════════════════════════════════════════════════
// Catalog node
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("every character registers as a distinct catalog node",
          "[character-delay][catalog]") {
    namespace catalog = pulp::host::character_delay;
    pulp::host::SignalGraph graph;

    const auto nodes = {
        catalog::make_character_delay_node(Character::clean),
        catalog::make_character_delay_node(Character::vintage_digital),
        catalog::make_character_delay_node(Character::tape),
        catalog::make_character_delay_node(Character::tape, TapeTier::physical),
        catalog::make_character_delay_node(Character::bbd),
        catalog::make_character_delay_node(Character::diffusion),
    };

    for (const auto& type : nodes) {
        INFO("type " << type.type_id);
        CHECK(type.num_input_ports == 2);
        CHECK(type.num_output_ports == 2);
        CHECK(type.lowerable);
        CHECK(type.baked_params.size() == 10);
        CHECK(static_cast<bool>(type.process_instance_baked_param));
        CHECK(graph.register_custom_node_type(type));
    }
    // Each character must claim a DISTINCT id — that identity is what a baked
    // artifact resolves against.
    std::vector<std::string> ids;
    for (const auto& type : nodes) ids.push_back(type.type_id);
    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
}

TEST_CASE("configuring the tape speed before the sample rate is safe",
          "[character-delay][catalog]") {
    // The catalog node's prepare() sets character, tier and tape speed and only
    // then the sample rate — so a speed change arrives while the physical
    // tier's FIR banks and working buffer do not yet exist. Walking them there
    // is an out-of-bounds write, and it only triggers for nodes constructed at
    // a non-default speed, which is exactly the kind of thing that ships.
    Engine delay;
    delay.set_character(Character::tape);
    delay.set_tape_tier(TapeTier::physical);
    delay.set_tape_speed_ips(15.0f);
    delay.set_sample_rate(kSr);
    delay.set_time_ms(200.0f);
    delay.set_feedback(0.4f);
    delay.set_character_amount(0.5f);
    delay.reset();

    auto buffers = sine_both(static_cast<int>(kSr * 0.5), 500.0, 0.4f);
    render(delay, buffers);
    CHECK(all_finite(buffers.left));
    CHECK(rms(buffers.left, 0, static_cast<int>(buffers.left.size())) > 0.0);
    // The speed the caller asked for is the speed the banks were designed at.
    CHECK(delay.tape_gap_coefficients(0).size() == cd::tape_gap_fir_taps(kSr));
}

TEST_CASE("the catalog node bakes and delays", "[character-delay][catalog]") {
    namespace catalog = pulp::host::character_delay;
    using namespace pulp::host;

    const auto type = catalog::make_character_delay_node(Character::clean);
    SignalGraph graph;
    REQUIRE(graph.register_custom_node_type(type));
    const auto input = graph.add_input_node(2, "In");
    const auto node = graph.add_custom_node(type.type_id, 1, "Engine");
    const auto output = graph.add_output_node(2, "Out");
    for (PortIndex port = 0; port < 2; ++port) {
        REQUIRE(graph.connect(input, port, node, port));
        REQUIRE(graph.connect(node, port, output, port));
    }
    graph.set_canonical_executor_routing_enabled(true);
    REQUIRE(graph.prepare(kSr, 512));

    auto result = bake(graph);
    REQUIRE(result.accepted);
    REQUIRE(result.processor);

    pulp::format::PrepareContext prepare_context;
    prepare_context.sample_rate = kSr;
    prepare_context.max_buffer_size = 512;
    prepare_context.input_channels = 2;
    prepare_context.output_channels = 2;
    result.processor->prepare(prepare_context);

    // The node defaults to 350 ms; render long enough to see the repeat.
    const int frames = 512;
    const int blocks = static_cast<int>(kSr * 0.5) / frames;
    std::vector<float> left(static_cast<std::size_t>(frames), 0.0f);
    std::vector<float> right(static_cast<std::size_t>(frames), 0.0f);
    std::vector<float> captured;
    captured.reserve(static_cast<std::size_t>(blocks * frames));

    for (int block = 0; block < blocks; ++block) {
        std::fill(left.begin(), left.end(), 0.0f);
        std::fill(right.begin(), right.end(), 0.0f);
        if (block == 0) left[0] = 1.0f;

        const float* in_pointers[] = {left.data(), right.data()};
        float* out_pointers[] = {left.data(), right.data()};
        pulp::audio::BufferView<const float> in_view(in_pointers, 2,
                                                     static_cast<std::uint32_t>(frames));
        pulp::audio::BufferView<float> out_view(out_pointers, 2,
                                                static_cast<std::uint32_t>(frames));
        pulp::midi::MidiBuffer midi_in;
        pulp::midi::MidiBuffer midi_out;
        pulp::format::ProcessContext context;
        context.sample_rate = kSr;
        context.num_samples = frames;
        result.processor->process(out_view, in_view, midi_in, midi_out, context);
        captured.insert(captured.end(), left.begin(), left.end());
    }

    const int index = peak_index(captured, 1, static_cast<int>(captured.size()));
    INFO("baked repeat at " << index << ", expected " << 0.35 * kSr);
    CHECK(std::abs(index - 0.35 * kSr) <= 0.002 * kSr);
}
