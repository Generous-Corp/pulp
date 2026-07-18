#pragma once

/// @file wt.hpp
/// OSC-WT (modern tier): a wavetable oscillator that plays a set of single-cycle
/// tables with band-limited band-switching and a smooth scan across the set. It
/// is a thin osc-module front-end over the shipped `WavetableBankT` /
/// `WavetableT` engine (`signal/wavetable.hpp`) — the composition engine, not a
/// reimplementation.
///
/// ── The decided architecture ──────────────────────────────────────────────
///
/// The wavetable-architecture question was settled by measurement, not
/// assertion: at a real band boundary the existing band-switch-crossfade is at
/// least as alias-clean as mip-map+interpolate and click-free at the sample
/// level, so the modern tier EXTENDS `WavetableT` rather than forking a new
/// mip-map engine. This class is what that decision looks like in code: it holds
/// a `WavetableBankT<double>`, drives it per-sample, and adds only the two things
/// the bank does not already own — the osc-module per-sample frequency contract
/// and a scan smoother.
///
///   * **Band-switching** is `WavetableT`'s: each `WavetableT` selects the band
///     whose Nyquist budget covers the current frequency and crossfades across
///     `kCrossfadeSamples` (128) samples when the selection changes, so crossing
///     a band ceiling is click-free.
///   * **Scan / morph** is `WavetableBankT`'s: a 0..1 position linearly
///     interpolates between adjacent tables in the set. This front-end SLEWS the
///     position with a one-pole so a stepped (block-rate) scan control does not
///     zipper — the modern tier is the clean one. (The intentional stepped-scan
///     grit of the lo-fi tier is a separate engine, not this.)
///
/// ── Per-sample frequency, the osc-module way ──────────────────────────────
///
/// `next(increment)` takes the per-sample phase step (frequency ÷ sample rate),
/// matching `VaOscillator` / `VcoOscillator` / the DCO front-end, so per-sample
/// pitch modulation composes without an API change. The increment is converted
/// to a frequency (`increment · sample_rate`) and handed to the bank, which owns
/// the phase clock and the band selection. Playback is for positive frequencies
/// — standard wavetable readout; through-zero FM is the VA/VCO's domain, not the
/// wavetable engine's (`WavetableT` wraps its phase forward only).
///
/// RT contract: `set_wavetable_set` allocates and rebuilds the bank, so it runs
/// off the audio thread; `prepare` re-arms the bank (no allocation) and is
/// conventionally a setup-time call too. `set_position`, `set_scan_time_ms`,
/// `reset`, and `next` allocate nothing, lock nothing, and perform no I/O.
/// `double` throughout, matching the osc modules; a `float` caller narrows once
/// on store.

#include <pulp/signal/wavetable.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace pulp::signal::osc {

/// Modern wavetable oscillator over a set of band-limited `WavetableT`s.
class WtOscillator {
public:
    /// Default scan slew time (ms): the one-pole time constant that turns a
    /// stepped position control into a smooth sweep. Short enough to feel
    /// immediate, long enough that a full 0→1 jump does not click.
    static constexpr double kDefaultScanTimeMs = 5.0;

    /// Install the table set (off the audio thread). Each entry is a fully
    /// band-limited `WavetableT`; the set is morphed across by `set_position`.
    /// One entry is a plain single-wavetable oscillator (no scan). The band
    /// ceilings are read from the first table so `band_max_frequency_hz` can
    /// report them without exposing the bank's internals.
    void set_wavetable_set(std::vector<Wavetable64> tables) {
        ceilings_.clear();
        if (!tables.empty()) {
            const Wavetable64& first = tables.front();
            ceilings_.reserve(first.band_count());
            for (std::size_t b = 0; b < first.band_count(); ++b)
                ceilings_.push_back(first.band_max_frequency_hz(b));
        }
        table_count_ = tables.size();
        bank_ = WavetableBank64(std::move(tables));
        bank_.set_sample_rate(sample_rate_);
        // The fresh bank starts on its default band with nothing audible to fade
        // FROM, so the next frequency must SNAP to its band, not crossfade up from
        // the default — otherwise swapping the wavetable set mid-note replays the
        // aliased band-switch onset that `reset()`'s snap exists to prevent.
        pending_band_snap_ = true;
        update_scan_coeff();
    }

    /// Set the sample rate. Call before `next` is meaningful; safe to call again
    /// on a rate change (it re-derives the scan coefficient and re-arms the bank).
    void prepare(double sample_rate) {
        if (sample_rate > 0.0) sample_rate_ = sample_rate;
        bank_.set_sample_rate(sample_rate_);
        update_scan_coeff();
    }

    /// Reset phase and crossfade state to the start of the cycle, and snap the
    /// scan position to its target (a reset render starts settled, not mid-slew).
    /// The band also starts settled: the first `next` after this snaps straight to
    /// its band rather than crossfading up from the default band, so a fresh voice
    /// has no aliased band-switch onset.
    ///
    /// No phase-seed parameter: the wavetable engine resets to phase 0 and exposes
    /// no seed, and — unlike the discontinuity-corrected shapes, where a clean
    /// start phase matters — a wavetable is smooth everywhere, so there is no
    /// phase worth seeding to. Offering a parameter the engine cannot honor would
    /// be the misleading option.
    void reset() {
        bank_.reset();
        pending_band_snap_ = true;
        scan_position_ = scan_target_;
        bank_.set_position(scan_position_);
    }

    /// Target scan position across the table set, 0..1 (0 = first table, 1 =
    /// last). Clamped. The audible position slews toward this target at
    /// `scan_time_ms`, so a block-rate step does not zipper.
    void set_position(double pos) {
        scan_target_ = std::clamp(pos, 0.0, 1.0);
    }
    /// The audible (slewed) scan position — what the bank is actually morphing at.
    double position() const { return scan_position_; }

    /// Scan slew time, in ms. 0 makes the scan instantaneous (the position jumps
    /// to its target every sample — the stepped path the modern tier avoids by
    /// default). Larger is a slower, smoother sweep.
    void set_scan_time_ms(double ms) {
        scan_time_ms_ = ms > 0.0 ? ms : 0.0;
        update_scan_coeff();
    }
    double scan_time_ms() const { return scan_time_ms_; }

    /// Generate one sample and advance by `increment` cycles (frequency ÷ sample
    /// rate). The increment drives the bank's frequency — hence its band
    /// selection and crossfade — and the scan position is advanced one slew step
    /// toward its target first, so the morph is smooth under a stepped control.
    double next(double increment) {
        const double frequency = increment * sample_rate_;
        if (frequency > 0.0) {
            // The first frequency after construction/reset snaps to its band with
            // no crossfade: a fresh voice has no previously audible band to fade
            // FROM, so a crossfade here would play the default band's harmonics at
            // the new pitch for 128 samples — an aliased onset on every note-on.
            // Every later frequency crossfades as normal (a fade between two real
            // playing bands).
            if (pending_band_snap_) {
                bank_.set_frequency_immediate(frequency);
                pending_band_snap_ = false;
            } else {
                bank_.set_frequency(frequency);
            }
        }

        // One-pole slew of the scan position toward the target. At coeff 1
        // (scan_time_ms == 0) this is an exact jump; otherwise it is a smooth,
        // click-free approach.
        scan_position_ += (scan_target_ - scan_position_) * scan_coeff_;
        bank_.set_position(scan_position_);

        return bank_.next();
    }

    /// Number of bands in the table set's readout (from the first table).
    std::size_t band_count() const { return ceilings_.size(); }
    /// Upper alias-free playback frequency of band `i` (its ceiling). 0 for an
    /// out-of-range index or an empty set.
    double band_max_frequency_hz(std::size_t i) const {
        return i < ceilings_.size() ? ceilings_[i] : 0.0;
    }
    /// Number of tables the scan morphs across.
    std::size_t table_count() const { return table_count_; }

private:
    /// One-pole coefficient for a `scan_time_ms_` time constant at the current
    /// rate. `1 - exp(-1 / (tau · fs))`; a zero time constant is an exact jump
    /// (coeff 1), which is why the branch is explicit rather than a 1/0.
    void update_scan_coeff() {
        if (scan_time_ms_ <= 0.0 || sample_rate_ <= 0.0) {
            scan_coeff_ = 1.0;
            return;
        }
        const double tau_samples = scan_time_ms_ * 0.001 * sample_rate_;
        scan_coeff_ = 1.0 - std::exp(-1.0 / tau_samples);
    }

    WavetableBank64 bank_;
    std::vector<double> ceilings_;
    std::size_t table_count_ = 0;

    double sample_rate_ = 48000.0;
    double scan_target_ = 0.0;
    double scan_position_ = 0.0;
    double scan_time_ms_ = kDefaultScanTimeMs;
    double scan_coeff_ = 1.0;
    bool pending_band_snap_ = true;
};

} // namespace pulp::signal::osc
