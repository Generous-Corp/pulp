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
/// Built-in factories (`make_sine`, `make_saw`, `make_square`,
/// `make_triangle`) generate fully bandlimited stacks across the
/// audible range using direct harmonic synthesis: each band caps
/// harmonics at `sample_rate / 2 / max_freq` so alias energy stays
/// below the band's playback ceiling.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace pulp::signal {

/// One single-cycle wavetable plus the maximum playback frequency it
/// remains bandlimited at. Tables in a `Wavetable` are kept sorted
/// by `max_frequency_hz` ascending.
struct WavetableEntry {
    std::vector<float> samples;          ///< one period of bandlimited audio
    float max_frequency_hz = 22050.0f;   ///< upper bound where this table stays alias-free
};

/// Wavetable oscillator with band-switching.
///
/// Construction: supply a pre-sorted list of bands (lowest
/// `max_frequency_hz` first). `set_frequency` selects the smallest
/// band whose budget covers the new frequency. When the selection
/// changes, `next()` crossfades between the previous and new band
/// across `kCrossfadeSamples` samples — short enough to be inaudible,
/// long enough to keep transitions click-free.
class Wavetable {
public:
    /// Length of the band-switch crossfade in samples. ~3 ms at
    /// 48 kHz — well below the cycle of even the lowest audible
    /// frequency, so it does not smear transients.
    static constexpr std::size_t kCrossfadeSamples = 128;

    Wavetable() = default;

    /// Construct from a list of bands. The list is sorted by
    /// `max_frequency_hz` ascending internally; entries with empty
    /// `samples` are rejected.
    explicit Wavetable(std::vector<WavetableEntry> bands) {
        bands_.reserve(bands.size());
        for (auto& b : bands) {
            if (!b.samples.empty()) bands_.push_back(std::move(b));
        }
        std::sort(bands_.begin(), bands_.end(),
                  [](const WavetableEntry& a, const WavetableEntry& b) {
                      return a.max_frequency_hz < b.max_frequency_hz;
                  });
        target_band_ = select_band_for(frequency_);
        crossfade_source_ = target_band_;
    }

    void set_sample_rate(float sr) {
        if (sr > 0.0f) sample_rate_ = sr;
    }
    void set_frequency(float hz) {
        if (hz <= 0.0f) return;
        frequency_ = hz;
        const int new_band = select_band_for(frequency_);
        if (new_band != target_band_) {
            // Begin a crossfade. If a previous fade is still in flight,
            // capture its current source + target + remaining samples
            // so the new fade can blend FROM the actually-audible mix
            // rather than jumping back to the previous target (Codex P1
            // on #2865 — rapid retunes lost continuity, producing the
            // click the crossfade was meant to prevent).
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
    float next() {
        if (bands_.empty()) return 0.0f;

        const float new_sample = sample_band(target_band_, phase_);
        float out = new_sample;
        if (crossfade_samples_remaining_ > 0) {
            const float t = 1.0f - (static_cast<float>(crossfade_samples_remaining_)
                                    / static_cast<float>(kCrossfadeSamples));
            // Source for the new fade. If a previous fade was still
            // in flight when this one started, sample the in-flight
            // blend at its progress ratio so the new fade begins from
            // the actually-audible mix instead of the in-flight target
            // (Codex P1 on #2865).
            float source_sample;
            if (has_in_flight_ && in_flight_remaining_ > 0) {
                const float old_t = 1.0f
                    - (static_cast<float>(in_flight_remaining_)
                       / static_cast<float>(kCrossfadeSamples));
                const float old_src = sample_band(in_flight_source_, phase_);
                const float old_tgt = sample_band(in_flight_target_, phase_);
                source_sample = old_src * (1.0f - old_t) + old_tgt * old_t;
                --in_flight_remaining_;
                if (in_flight_remaining_ == 0) has_in_flight_ = false;
            } else {
                source_sample = sample_band(crossfade_source_, phase_);
                has_in_flight_ = false;
            }
            out = source_sample * (1.0f - t) + new_sample * t;
            --crossfade_samples_remaining_;
        }

        const float dt = frequency_ / sample_rate_;
        phase_ += dt;
        while (phase_ >= 1.0f) phase_ -= 1.0f;
        while (phase_ < 0.0f) phase_ += 1.0f;
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
    static inline Wavetable make_sine(std::size_t table_length = 2048,
                                       float reference_sample_rate = 48000.0f);
    static inline Wavetable make_saw(std::size_t bands = 10,
                                      std::size_t table_length = 2048,
                                      float reference_sample_rate = 48000.0f);
    static inline Wavetable make_square(std::size_t bands = 10,
                                         std::size_t table_length = 2048,
                                         float reference_sample_rate = 48000.0f);
    static inline Wavetable make_triangle(std::size_t bands = 10,
                                           std::size_t table_length = 2048,
                                           float reference_sample_rate = 48000.0f);

private:
    int select_band_for(float hz) const {
        if (bands_.empty()) return 0;
        for (std::size_t i = 0; i < bands_.size(); ++i) {
            if (hz <= bands_[i].max_frequency_hz) return static_cast<int>(i);
        }
        return static_cast<int>(bands_.size() - 1);
    }
    float sample_band(int band, float phase) const {
        if (band < 0 || static_cast<std::size_t>(band) >= bands_.size()) return 0.0f;
        const auto& table = bands_[band].samples;
        if (table.empty()) return 0.0f;
        const float n = static_cast<float>(table.size());
        const float pos = phase * n;
        const std::size_t i0 = static_cast<std::size_t>(std::floor(pos)) % table.size();
        const std::size_t i1 = (i0 + 1) % table.size();
        const float frac = pos - std::floor(pos);
        return table[i0] * (1.0f - frac) + table[i1] * frac;
    }

    std::vector<WavetableEntry> bands_;
    float sample_rate_ = 48000.0f;
    float frequency_ = 440.0f;
    float phase_ = 0.0f;
    int target_band_ = 0;
    int crossfade_source_ = 0;
    std::size_t crossfade_samples_remaining_ = 0;
    // Captured in-flight fade state so rapid retunes blend FROM the
    // actually-audible mix (Codex P1 on #2865).
    bool has_in_flight_ = false;
    int in_flight_source_ = 0;
    int in_flight_target_ = 0;
    std::size_t in_flight_remaining_ = 0;
};

/// Linear-interpolated morph across N `Wavetable`s. Position 0..1
/// selects the wavetable: 0 = first, 1 = last, with linear
/// interpolation between adjacent entries.
class WavetableBank {
public:
    WavetableBank() = default;
    explicit WavetableBank(std::vector<Wavetable> waveforms)
        : tables_(std::move(waveforms)) {}

    void set_sample_rate(float sr) {
        for (auto& t : tables_) t.set_sample_rate(sr);
    }
    void set_frequency(float hz) {
        for (auto& t : tables_) t.set_frequency(hz);
    }
    void set_position(float pos) {
        position_ = std::clamp(pos, 0.0f, 1.0f);
    }

    float next() {
        if (tables_.empty()) return 0.0f;
        if (tables_.size() == 1) return tables_.front().next();

        const float scaled = position_ * static_cast<float>(tables_.size() - 1);
        const std::size_t lo = static_cast<std::size_t>(std::floor(scaled));
        const std::size_t hi = std::min(lo + 1, tables_.size() - 1);
        const float frac = scaled - static_cast<float>(lo);

        // Advance every table at the same frequency so morphing does
        // not introduce phase wobble between adjacent waveforms.
        float lo_sample = 0.0f;
        float hi_sample = 0.0f;
        for (std::size_t i = 0; i < tables_.size(); ++i) {
            const float s = tables_[i].next();
            if (i == lo) lo_sample = s;
            if (i == hi) hi_sample = s;
        }
        return lo_sample * (1.0f - frac) + hi_sample * frac;
    }

    void reset() {
        for (auto& t : tables_) t.reset();
    }

    std::size_t size() const { return tables_.size(); }

private:
    std::vector<Wavetable> tables_;
    float position_ = 0.0f;
};

// ── Factory implementations ────────────────────────────────────────────────

namespace detail {

constexpr float kWavetableTwoPi = 6.283185307179586476925286766559f;

template <typename HarmonicAmp>
inline std::vector<float> generate_wavetable(std::size_t length,
                                              std::size_t max_harmonic,
                                              HarmonicAmp&& harmonic_amp) {
    std::vector<float> samples(length, 0.0f);
    if (length == 0 || max_harmonic == 0) return samples;
    for (std::size_t i = 0; i < length; ++i) {
        const float phase = static_cast<float>(i) / static_cast<float>(length);
        float s = 0.0f;
        for (std::size_t k = 1; k <= max_harmonic; ++k) {
            const float amp = harmonic_amp(k);
            if (amp == 0.0f) continue;
            s += amp * std::sin(kWavetableTwoPi * static_cast<float>(k) * phase);
        }
        samples[i] = s;
    }
    float peak = 0.0f;
    for (float s : samples) peak = std::max(peak, std::fabs(s));
    if (peak > 0.0f) {
        const float inv = 1.0f / peak;
        for (float& s : samples) s *= inv;
    }
    return samples;
}

template <typename HarmonicAmp>
inline std::vector<WavetableEntry> build_wavetable_band_stack(
        std::size_t num_bands,
        std::size_t table_length,
        float reference_sample_rate,
        HarmonicAmp&& harmonic_amp) {
    if (num_bands == 0) return {};
    // Codex P2 on #2865 — non-positive sample rates produce
    // clamped_ceiling <= 0 and then a NaN/-inf in the std::floor →
    // size_t cast (UB). Public factories accept arbitrary inputs, so
    // short-circuit on invalid sample rates here.
    if (!(reference_sample_rate > 0.0f)) return {};
    const float nyquist = reference_sample_rate * 0.5f;
    constexpr float kBaseFreq = 20.0f;
    std::vector<WavetableEntry> bands;
    bands.reserve(num_bands);
    const float ratio = std::pow(nyquist / kBaseFreq,
                                  1.0f / static_cast<float>(num_bands));
    float ceiling = kBaseFreq;
    for (std::size_t b = 0; b < num_bands; ++b) {
        ceiling *= ratio;
        const float clamped_ceiling = std::min(ceiling, nyquist);
        std::size_t max_harmonic = static_cast<std::size_t>(
            std::floor(nyquist / clamped_ceiling));
        if (max_harmonic < 1) max_harmonic = 1;
        bands.push_back(WavetableEntry{
            generate_wavetable(table_length, max_harmonic, harmonic_amp),
            clamped_ceiling,
        });
    }
    return bands;
}

} // namespace detail

inline Wavetable Wavetable::make_sine(std::size_t table_length,
                                       float reference_sample_rate) {
    // Codex P2 on #2865 — guard against non-positive sample rates so
    // the returned Wavetable has a well-defined (empty) band stack
    // rather than a band with a negative ceiling.
    if (!(reference_sample_rate > 0.0f)) return Wavetable();
    std::vector<WavetableEntry> bands;
    bands.push_back(WavetableEntry{
        detail::generate_wavetable(table_length, /*max_harmonic=*/1,
                                    [](std::size_t k) {
                                        return k == 1 ? 1.0f : 0.0f;
                                    }),
        reference_sample_rate * 0.5f,
    });
    return Wavetable(std::move(bands));
}

inline Wavetable Wavetable::make_saw(std::size_t bands,
                                      std::size_t table_length,
                                      float reference_sample_rate) {
    return Wavetable(detail::build_wavetable_band_stack(
        bands, table_length, reference_sample_rate,
        [](std::size_t k) { return 1.0f / static_cast<float>(k); }));
}

inline Wavetable Wavetable::make_square(std::size_t bands,
                                         std::size_t table_length,
                                         float reference_sample_rate) {
    return Wavetable(detail::build_wavetable_band_stack(
        bands, table_length, reference_sample_rate,
        [](std::size_t k) {
            return (k % 2 == 1) ? 1.0f / static_cast<float>(k) : 0.0f;
        }));
}

inline Wavetable Wavetable::make_triangle(std::size_t bands,
                                           std::size_t table_length,
                                           float reference_sample_rate) {
    return Wavetable(detail::build_wavetable_band_stack(
        bands, table_length, reference_sample_rate,
        [](std::size_t k) {
            if (k % 2 == 0) return 0.0f;
            const float kk = static_cast<float>(k);
            const float sign = ((k - 1) / 2) % 2 == 0 ? 1.0f : -1.0f;
            return sign / (kk * kk);
        }));
}

} // namespace pulp::signal
