#pragma once

/// @file wavetable.hpp
/// Wavetable oscillator with band-switching + bank morphing.
///
/// `Wavetable` plays back a stack of single-cycle, pre-bandlimited
/// tables. At each block the table whose Nyquist budget covers the
/// current playback frequency is selected; when the selection
/// changes, the oscillator crossfades between the previous and new
/// table across a short window so the band switch is click-free.
///
/// `WavetableBank` morphs across N `Wavetable`s — useful for
/// "wavetable evolution" patches where a 0..1 position knob sweeps
/// between several base waveforms.
///
/// Built-in factories (`make_saw`, `make_square`, `make_triangle`)
/// generate fully bandlimited stacks across the audible range using
/// direct harmonic synthesis: each band caps harmonics so the top
/// harmonic at that band's playback ceiling stays at or below Nyquist
/// (`sample_rate / 2`). `make_sine` is the degenerate single-band case.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace pulp::signal {

/// One single-cycle wavetable plus the maximum playback frequency it
/// remains bandlimited at. Tables in a `Wavetable` are kept sorted
/// by `max_frequency_hz` ascending.
template <typename SampleType = float>
struct WavetableEntryT {
    std::vector<SampleType> samples;      ///< one period of bandlimited audio
    SampleType max_frequency_hz = SampleType{22050.0f}; ///< upper bound where this table stays alias-free
};

using WavetableEntry = WavetableEntryT<float>;
using WavetableEntry64 = WavetableEntryT<double>;

/// Wavetable oscillator with band-switching.
///
/// RT contract: constructors and factory helpers allocate table storage and
/// must run off the audio thread. `set_sample_rate()`, `set_frequency()`,
/// `reset()`, and `next()` allocate no memory after construction.
///
/// Construction: supply a pre-sorted list of bands (lowest
/// `max_frequency_hz` first). `set_frequency` selects the smallest
/// band whose budget covers the new frequency. When the selection
/// changes, `next()` crossfades between the previous and new band
/// across `kCrossfadeSamples` samples — short enough to be inaudible,
/// long enough to keep transitions click-free.
template <typename SampleType = float>
class WavetableT {
public:
    /// Length of the band-switch crossfade in samples. ~2.7 ms at
    /// 48 kHz — well below the cycle of even the lowest audible
    /// frequency, so it does not smear transients.
    static constexpr std::size_t kCrossfadeSamples = 128;

    WavetableT() = default;

    /// Construct from a list of bands. The list is sorted by
    /// `max_frequency_hz` ascending internally; entries with empty
    /// `samples` are rejected.
    explicit WavetableT(std::vector<WavetableEntryT<SampleType>> bands) {
        bands_.reserve(bands.size());
        for (auto& b : bands) {
            if (!b.samples.empty()) bands_.push_back(std::move(b));
        }
        std::sort(bands_.begin(), bands_.end(),
                  [](const WavetableEntryT<SampleType>& a,
                     const WavetableEntryT<SampleType>& b) {
                      return a.max_frequency_hz < b.max_frequency_hz;
                  });
        target_band_ = select_band_for(frequency_);
        crossfade_source_ = target_band_;
    }

    void set_sample_rate(SampleType sr) {
        if (sr > SampleType{0.0f}) sample_rate_ = sr;
    }
    void set_frequency(SampleType hz) {
        if (hz <= SampleType{0.0f}) return;
        frequency_ = hz;
        const int new_band = select_band_for(frequency_);
        if (new_band != target_band_) {
            // Begin a crossfade. If a previous fade is still in flight,
            // capture its current source + target + remaining samples
            // so the new fade can blend FROM the actually-audible mix
            // rather than jumping back to the previous target. Rapid
            // retunes must preserve continuity so the crossfade does
            // not introduce the click it is meant to prevent.
            if (crossfade_samples_remaining_ > 0) {
                in_flight_source_ = crossfade_source_;
                in_flight_target_ = target_band_;
                in_flight_remaining_ = crossfade_samples_remaining_;
                has_in_flight_ = true;
            } else {
                has_in_flight_ = false;
            }
            crossfade_source_ = target_band_;
            target_band_ = new_band;
            crossfade_samples_remaining_ = kCrossfadeSamples;
        }
    }

    void reset() {
        phase_ = 0.0f;
        crossfade_samples_remaining_ = 0;
        crossfade_source_ = target_band_;
        has_in_flight_ = false;
        in_flight_remaining_ = 0;
    }

    /// Generate the next sample. Returns 0 for an empty wavetable
    /// (allocation-safe default rather than UB).
    SampleType next() {
        if (bands_.empty()) return SampleType{0.0f};

        const SampleType new_sample = sample_band(target_band_, phase_);
        SampleType out = new_sample;
        if (crossfade_samples_remaining_ > 0) {
            const SampleType t = SampleType{1.0f} -
                (static_cast<SampleType>(crossfade_samples_remaining_) /
                 static_cast<SampleType>(kCrossfadeSamples));
            // Source for the new fade. If a previous fade was still
            // in flight when this one started, sample the in-flight
            // blend at its progress ratio so the new fade begins from
            // the actually-audible mix instead of the in-flight target.
            SampleType source_sample;
            if (has_in_flight_ && in_flight_remaining_ > 0) {
                const SampleType old_t = SampleType{1.0f}
                    - (static_cast<SampleType>(in_flight_remaining_)
                       / static_cast<SampleType>(kCrossfadeSamples));
                const SampleType old_src = sample_band(in_flight_source_, phase_);
                const SampleType old_tgt = sample_band(in_flight_target_, phase_);
                source_sample = old_src * (SampleType{1.0f} - old_t) + old_tgt * old_t;
                --in_flight_remaining_;
                if (in_flight_remaining_ == 0) has_in_flight_ = false;
            } else {
                source_sample = sample_band(crossfade_source_, phase_);
                has_in_flight_ = false;
            }
            out = source_sample * (SampleType{1.0f} - t) + new_sample * t;
            --crossfade_samples_remaining_;
        }

        const SampleType dt = frequency_ / sample_rate_;
        phase_ += dt;
        while (phase_ >= SampleType{1.0f}) phase_ -= SampleType{1.0f};
        while (phase_ < SampleType{0.0f}) phase_ += SampleType{1.0f};
        return out;
    }

    std::size_t band_count() const { return bands_.size(); }
    int current_band() const { return target_band_; }
    bool is_crossfading() const { return crossfade_samples_remaining_ > 0; }

    /// Compose a stack of harmonic-based bands for one of the four
    /// classical shapes (sine, saw, square, triangle). The result is
    /// fully bandlimited across the audible range: each band carries
    /// only harmonics whose frequency at the band's ceiling stays
    /// below half the assumed reference sample rate.
    static inline WavetableT make_sine(
        std::size_t table_length = 2048,
        SampleType reference_sample_rate = SampleType{48000.0f});
    static inline WavetableT make_saw(
        std::size_t bands = 10,
        std::size_t table_length = 2048,
        SampleType reference_sample_rate = SampleType{48000.0f});
    static inline WavetableT make_square(
        std::size_t bands = 10,
        std::size_t table_length = 2048,
        SampleType reference_sample_rate = SampleType{48000.0f});
    static inline WavetableT make_triangle(
        std::size_t bands = 10,
        std::size_t table_length = 2048,
        SampleType reference_sample_rate = SampleType{48000.0f});

private:
    int select_band_for(SampleType hz) const {
        if (bands_.empty()) return 0;
        for (std::size_t i = 0; i < bands_.size(); ++i) {
            if (hz <= bands_[i].max_frequency_hz) return static_cast<int>(i);
        }
        return static_cast<int>(bands_.size() - 1);
    }
    SampleType sample_band(int band, SampleType phase) const {
        if (band < 0 || static_cast<std::size_t>(band) >= bands_.size())
            return SampleType{0.0f};
        const auto& table = bands_[band].samples;
        if (table.empty()) return SampleType{0.0f};
        const SampleType n = static_cast<SampleType>(table.size());
        const SampleType pos = phase * n;
        const std::size_t i0 = static_cast<std::size_t>(std::floor(pos)) % table.size();
        const std::size_t i1 = (i0 + 1) % table.size();
        const SampleType frac = pos - std::floor(pos);
        return table[i0] * (SampleType{1.0f} - frac) + table[i1] * frac;
    }

    std::vector<WavetableEntryT<SampleType>> bands_;
    SampleType sample_rate_ = SampleType{48000.0f};
    SampleType frequency_ = SampleType{440.0f};
    SampleType phase_ = SampleType{0.0f};
    int target_band_ = 0;
    int crossfade_source_ = 0;
    std::size_t crossfade_samples_remaining_ = 0;
    // Captured in-flight fade state so rapid retunes blend FROM the
    // actually-audible mix.
    bool has_in_flight_ = false;
    int in_flight_source_ = 0;
    int in_flight_target_ = 0;
    std::size_t in_flight_remaining_ = 0;
};

/// Linear-interpolated morph across N `Wavetable`s. Position 0..1
/// selects the wavetable: 0 = first, 1 = last, with linear
/// interpolation between adjacent entries.
///
/// RT contract: construction and vector ownership changes allocate. Setters and
/// `next()` allocate no memory once the bank is built.
template <typename SampleType = float>
class WavetableBankT {
public:
    WavetableBankT() = default;
    explicit WavetableBankT(std::vector<WavetableT<SampleType>> waveforms)
        : tables_(std::move(waveforms)) {}

    void set_sample_rate(SampleType sr) {
        for (auto& t : tables_) t.set_sample_rate(sr);
    }
    void set_frequency(SampleType hz) {
        for (auto& t : tables_) t.set_frequency(hz);
    }
    void set_position(SampleType pos) {
        position_ = std::clamp(pos, SampleType{0.0f}, SampleType{1.0f});
    }

    SampleType next() {
        if (tables_.empty()) return SampleType{0.0f};
        if (tables_.size() == 1) return tables_.front().next();

        const SampleType scaled =
            position_ * static_cast<SampleType>(tables_.size() - 1);
        const std::size_t lo = static_cast<std::size_t>(std::floor(scaled));
        const std::size_t hi = std::min(lo + 1, tables_.size() - 1);
        const SampleType frac = scaled - static_cast<SampleType>(lo);

        // Advance every table at the same frequency so morphing does
        // not introduce phase wobble between adjacent waveforms.
        SampleType lo_sample = SampleType{0.0f};
        SampleType hi_sample = SampleType{0.0f};
        for (std::size_t i = 0; i < tables_.size(); ++i) {
            const SampleType s = tables_[i].next();
            if (i == lo) lo_sample = s;
            if (i == hi) hi_sample = s;
        }
        return lo_sample * (SampleType{1.0f} - frac) + hi_sample * frac;
    }

    void reset() {
        for (auto& t : tables_) t.reset();
    }

    std::size_t size() const { return tables_.size(); }

private:
    std::vector<WavetableT<SampleType>> tables_;
    SampleType position_ = SampleType{0.0f};
};

using Wavetable = WavetableT<float>;
using Wavetable64 = WavetableT<double>;
using WavetableBank = WavetableBankT<float>;
using WavetableBank64 = WavetableBankT<double>;

// ── Factory implementations ────────────────────────────────────────────────

namespace detail {

template <typename SampleType>
constexpr SampleType kWavetableTwoPi =
    static_cast<SampleType>(6.283185307179586476925286766559L);

template <typename SampleType, typename HarmonicAmp>
inline std::vector<SampleType> generate_wavetable(std::size_t length,
                                                  std::size_t max_harmonic,
                                                  HarmonicAmp&& harmonic_amp) {
    std::vector<SampleType> samples(length, SampleType{0.0f});
    if (length == 0 || max_harmonic == 0) return samples;
    for (std::size_t i = 0; i < length; ++i) {
        const SampleType phase =
            static_cast<SampleType>(i) / static_cast<SampleType>(length);
        SampleType s = SampleType{0.0f};
        for (std::size_t k = 1; k <= max_harmonic; ++k) {
            const SampleType amp = harmonic_amp(k);
            if (amp == SampleType{0.0f}) continue;
            s += amp * std::sin(kWavetableTwoPi<SampleType> *
                                static_cast<SampleType>(k) * phase);
        }
        samples[i] = s;
    }
    SampleType peak = SampleType{0.0f};
    for (SampleType s : samples) peak = std::max(peak, std::fabs(s));
    if (peak > SampleType{0.0f}) {
        const SampleType inv = SampleType{1.0f} / peak;
        for (SampleType& s : samples) s *= inv;
    }
    return samples;
}

template <typename SampleType, typename HarmonicAmp>
inline std::vector<WavetableEntryT<SampleType>> build_wavetable_band_stack(
        std::size_t num_bands,
        std::size_t table_length,
        SampleType reference_sample_rate,
        HarmonicAmp&& harmonic_amp) {
    if (num_bands == 0) return {};
    // Non-positive sample rates produce clamped_ceiling <= 0 and then
    // a NaN/-inf in the std::floor -> size_t cast (UB). Public factories
    // accept arbitrary inputs, so short-circuit on invalid sample rates here.
    if (!(reference_sample_rate > SampleType{0.0f})) return {};
    const SampleType nyquist = reference_sample_rate * SampleType{0.5f};
    constexpr SampleType kBaseFreq = SampleType{20.0f};
    std::vector<WavetableEntryT<SampleType>> bands;
    bands.reserve(num_bands);
    const SampleType ratio =
        std::pow(nyquist / kBaseFreq,
                 SampleType{1.0f} / static_cast<SampleType>(num_bands));
    SampleType ceiling = kBaseFreq;
    for (std::size_t b = 0; b < num_bands; ++b) {
        ceiling *= ratio;
        const SampleType clamped_ceiling = std::min(ceiling, nyquist);
        std::size_t max_harmonic = static_cast<std::size_t>(
            std::floor(nyquist / clamped_ceiling));
        if (max_harmonic < 1) max_harmonic = 1;
        bands.push_back(WavetableEntryT<SampleType>{
            generate_wavetable<SampleType>(table_length, max_harmonic,
                                           harmonic_amp),
            clamped_ceiling,
        });
    }
    return bands;
}

} // namespace detail

template <typename SampleType>
inline WavetableT<SampleType> WavetableT<SampleType>::make_sine(
        std::size_t table_length,
        SampleType reference_sample_rate) {
    // Guard against non-positive sample rates so the returned Wavetable has a
    // well-defined empty band stack rather than a band with a negative ceiling.
    if (!(reference_sample_rate > SampleType{0.0f})) return WavetableT();
    std::vector<WavetableEntryT<SampleType>> bands;
    bands.push_back(WavetableEntryT<SampleType>{
        detail::generate_wavetable<SampleType>(
            table_length, /*max_harmonic=*/1,
            [](std::size_t k) {
                return k == 1 ? SampleType{1.0f} : SampleType{0.0f};
            }),
        reference_sample_rate * SampleType{0.5f},
    });
    return WavetableT(std::move(bands));
}

template <typename SampleType>
inline WavetableT<SampleType> WavetableT<SampleType>::make_saw(
        std::size_t bands,
        std::size_t table_length,
        SampleType reference_sample_rate) {
    return WavetableT(detail::build_wavetable_band_stack<SampleType>(
        bands, table_length, reference_sample_rate,
        [](std::size_t k) {
            return SampleType{1.0f} / static_cast<SampleType>(k);
        }));
}

template <typename SampleType>
inline WavetableT<SampleType> WavetableT<SampleType>::make_square(
        std::size_t bands,
        std::size_t table_length,
        SampleType reference_sample_rate) {
    return WavetableT(detail::build_wavetable_band_stack<SampleType>(
        bands, table_length, reference_sample_rate,
        [](std::size_t k) {
            return (k % 2 == 1)
                ? SampleType{1.0f} / static_cast<SampleType>(k)
                : SampleType{0.0f};
        }));
}

template <typename SampleType>
inline WavetableT<SampleType> WavetableT<SampleType>::make_triangle(
        std::size_t bands,
        std::size_t table_length,
        SampleType reference_sample_rate) {
    return WavetableT(detail::build_wavetable_band_stack<SampleType>(
        bands, table_length, reference_sample_rate,
        [](std::size_t k) {
            if (k % 2 == 0) return SampleType{0.0f};
            const SampleType kk = static_cast<SampleType>(k);
            const SampleType sign = ((k - 1) / 2) % 2 == 0
                ? SampleType{1.0f}
                : SampleType{-1.0f};
            return sign / (kk * kk);
        }));
}

} // namespace pulp::signal
