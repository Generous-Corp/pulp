#pragma once

// Shared deterministic render and measurement fixture for the character-delay
// engine suites. Assertions stay in the owning test translation units so this
// support layer only measures facts and never owns acceptance policy.

#include <pulp/signal/character_delay.hpp>
#include <pulp/signal/fft.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace pulp::test::character_delay {

namespace cd = pulp::signal::chardelay;
using Engine = pulp::signal::CharacterDelay;
using Character = Engine::Character;
using TapeTier = Engine::TapeTier;

inline constexpr double kSr = 48000.0;
inline constexpr int kBlock = 128;

struct Stereo {
    std::vector<float> left;
    std::vector<float> right;
};

inline Stereo make_stereo(int n) {
    return {std::vector<float>(static_cast<std::size_t>(n), 0.0f),
            std::vector<float>(static_cast<std::size_t>(n), 0.0f)};
}

/// Render in place, in blocks, the way a host would.
inline void render(Engine& delay, Stereo& buffers) {
    const auto n = static_cast<int>(buffers.left.size());
    for (int i = 0; i < n; i += kBlock) {
        const int count = std::min(kBlock, n - i);
        delay.process(buffers.left.data() + i, buffers.right.data() + i, count);
    }
}

/// Run `seconds` of silence through the delay to settle slews and envelopes.
inline void settle(Engine& delay, double seconds) {
    auto quiet = make_stereo(static_cast<int>(kSr * seconds));
    render(delay, quiet);
}

inline Stereo impulse_left(int n, float amplitude = 1.0f) {
    auto s = make_stereo(n);
    s.left[0] = amplitude;
    return s;
}

inline Stereo sine_both(int n, double hz, float amplitude) {
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
inline Stereo burst_left(int n, double hz, double seconds, float amplitude = 1.0f) {
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

inline double peak(const std::vector<float>& v, int from, int to) {
    double best = 0.0;
    const int end = std::min(to, static_cast<int>(v.size()));
    for (int i = std::max(0, from); i < end; ++i)
        best = std::max(best, std::abs(static_cast<double>(v[static_cast<std::size_t>(i)])));
    return best;
}

inline int peak_index(const std::vector<float>& v, int from, int to) {
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
inline int onset_index(const std::vector<float>& v, double fraction) {
    const double threshold = fraction * peak(v, 0, static_cast<int>(v.size()));
    for (std::size_t i = 0; i < v.size(); ++i)
        if (std::abs(static_cast<double>(v[i])) > threshold) return static_cast<int>(i);
    return -1;
}

inline double rms(const std::vector<float>& v, int from, int to) {
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

inline bool all_finite(const std::vector<float>& v) {
    for (float s : v)
        if (!std::isfinite(s)) return false;
    return true;
}

/// Largest sample-to-sample step in a window — the click detector.
inline double max_step(const std::vector<float>& v, int from, int to) {
    double worst = 0.0;
    const int end = std::min(to, static_cast<int>(v.size()));
    for (int i = std::max(1, from); i < end; ++i)
        worst = std::max(worst, std::abs(static_cast<double>(v[static_cast<std::size_t>(i)]) -
                                         static_cast<double>(v[static_cast<std::size_t>(i - 1)])));
    return worst;
}

/// Welch power spectral density: Hann windows, 50% overlap, averaged.
inline std::vector<double> welch_psd(const std::vector<float>& signal, int from, int to,
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

inline double psd_bin_hz(std::size_t bin, std::size_t bins, double rate) {
    return static_cast<double>(bin) * rate / (2.0 * static_cast<double>(bins - 1));
}

/// Magnitude of a signal's spectrum at one frequency, by direct correlation.
/// Cheaper and sharper than binning a PSD when only a handful of points matter.
inline double magnitude_at(const std::vector<float>& v, int from, int to, double hz) {
    double real = 0.0;
    double imaginary = 0.0;
    const int end = std::min(to, static_cast<int>(v.size()));
    const int count = end - from;
    for (int i = from; i < end; ++i) {
        const double phase =
            2.0 * cd::kPi * hz * static_cast<double>(i - from) / kSr;
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
inline constexpr int kTrackDecimation = 64;
inline constexpr double kTrackRate = kSr / kTrackDecimation;

inline std::vector<double> phase_track(const std::vector<float>& v, int from, int to,
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
inline double spectral_centroid(const std::vector<float>& v, int from, int to) {
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

inline void configure(Engine& delay, Character character, double time_ms, double feedback,
               double character_amount, TapeTier tier = TapeTier::standard);
inline double slew_seconds(Character character);

/// Configure a delay with the common test defaults.
inline void configure(Engine& delay, Character character, double time_ms, double feedback,
               double character_amount, TapeTier tier) {
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
inline double slew_seconds(Character character) {
    switch (character) {
        case Character::vintage_digital: return cd::kTimeSlewVintageMs * 0.001;
        case Character::tape: return cd::kTimeSlewTapeMs * 0.001;
        case Character::bbd: return cd::kTimeSlewBbdMs * 0.001;
        case Character::diffusion: return cd::kTimeSlewDiffusionMs * 0.001;
        case Character::clean:
        default: return cd::kTimeSlewCleanMs * 0.001;
    }
}

}  // namespace pulp::test::character_delay
