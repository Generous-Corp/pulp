#pragma once

/// @file wt_lofi.hpp
/// OSC-WT (lo-fi tier): a dedicated variable-clock zero-order-hold wavetable
/// oscillator. It plays a raw short single-cycle table with NEAREST/ZOH lookup
/// at a playback clock that tracks pitch (`fs_play = f0 · L`), so its
/// spectral-image ladder rides at `n · L · f0` and moves with the note. That
/// pitch-tracking image ladder IS the lo-fi sound — and it is exactly what a
/// fixed-rate, band-limited, linearly-interpolated wavetable engine cannot
/// produce — which is why this is a separate engine, not a mode of `WtOscillator`
/// / `WavetableT`.
///
/// ── Why a dedicated engine, decided by measurement ────────────────────────
///
/// The wavetable-architecture question was settled by rendering both candidates
/// and comparing each spectrum to the analytic variable-clock-ZOH image ladder:
///
///   * A variable-clock ZOH reader over a raw table reproduces the image lines
///     at `(nL ± m)·f0` to ~0.001 dB of the analytic sinc model, and the ladder
///     tracks pitch and table length `L`.
///   * `WavetableT` does NOT: its `sample_band()` always LINEAR-interpolates —
///     which alone suppresses the first image by ~42 dB — and its factory tables
///     are band-limited at build time, which removes the ladder entirely. There
///     is no way to parameterize `WavetableT` into a lo-fi reader without
///     dropping the very things that make it `WavetableT`.
///
/// So the modern tier extends `WavetableT` (`wt.hpp`) and the lo-fi tier is this
/// separate class. The two tiers deliberately diverge on every axis the modern
/// tier cleans up: ZOH vs interpolation, raw table vs band-limited, and a HARD
/// (stepped) scan vs the modern tier's slewed, zipper-free scan.
///
/// ── The five lo-fi mechanisms, each a measurable property ─────────────────
///
///   1. **Variable-clock ZOH images.** The table is clocked at `fs_play = f0·L`
///      and held (zero-order, no interpolation), so the baseband line `m·f0` is
///      replicated at `(nL ± m)·f0`, each weighted by the ZOH `sinc(π f/fs_play)`
///      envelope. Because `fs_play` scales with pitch, the images track the note
///      — the signature that separates this from a fixed-rate table.
///   2. **8-bit table quantization → grit.** The stored table (not the output
///      stream) is optionally quantized to `bit_depth` bits, undithered, about
///      zero. For a symmetric table this preserves half-wave symmetry, so the
///      grit is ODD-harmonic (h3, h5, …; even harmonics absent) with an
///      aggregate SNR near the ideal `6.02·N + 1.76` (≈ 49.9 dB at 8 bits) spread
///      across those odd lines — not a single spur, and not white.
///   3. **A real reconstruction stage.** Naive point-sampling of the staircase at
///      the host rate folds an authentic-but-supra-Nyquist image back IN-BAND
///      (audible), which is a modeling artifact, not the instrument's sound. So
///      the reader oversamples the staircase, applies a reconstruction lowpass,
///      and decimates — preserving the sub-Nyquist images the original passed
///      while not folding in the ultrasonic ones it removed. This stage is
///      load-bearing (it moves that in-band fold from ~−54 dBc to below −77 dBc),
///      not an afterthought; `set_reconstruction(false)` exposes the raw naive
///      path for A/B and for callers who want the rawer grit.
///   4. **Hard (stepped) wave-scan.** `set_scan` selects the nearest table in the
///      set with NO interpolation and NO slew, so crossing a table boundary steps
///      the output instantaneously — the classic wavetable "zipper." That step is
///      the faithful lo-fi behavior (contrast `WtOscillator`, which slews it
///      smooth); a mode-aware click gate must null it against the intended
///      stepped reference rather than flag it.
///   5. **Short-table harmonic ceiling.** A length-`L` table represents at most
///      `L/2` harmonics, so low notes are intrinsically darker and high notes
///      fold — a property of `L`, reproduced by playing the raw table rather than
///      a band-limited one.
///
/// ── No factory content ────────────────────────────────────────────────────
///
/// This engine ships NO wavetable data. The caller supplies the raw table(s);
/// the model reproduces the playback engine's character, never its wave content.
///
/// ── RT contract ────────────────────────────────────────────────────────────
///
/// `set_tables`, `set_oversample_factor`, `set_reconstruction`, and `prepare`
/// allocate / rebuild the reconstruction filter and run off the audio thread.
/// `set_scan`, `reset`, and `next` allocate nothing, lock nothing, and perform
/// no I/O. `double` throughout the reader; the table can be lower precision but
/// is stored as `double` after quantization.

#include <pulp/signal/windowed_sinc_design.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace pulp::signal::osc {

/// Dedicated variable-clock ZOH wavetable oscillator (the lo-fi tier).
class LofiWtOscillator {
public:
    /// Default stored-table resolution. 8 bits is the lo-fi grit; pass
    /// `kFullPrecision` for an ungritted table (the tests use it so the ZOH
    /// images are the only spuria).
    static constexpr int kDefaultBitDepth = 8;
    /// Sentinel bit depth: store the table at full `double` precision (no
    /// quantization). Any depth `<= 0` or `>= 53` is treated as full precision.
    static constexpr int kFullPrecision = 0;

    /// Default oversample factor for the reconstruction stage. Chosen so the
    /// residual modeling-alias floor sits comfortably below −77 dBc in-band; see
    /// the reconstruction discussion above.
    static constexpr int kDefaultOversampleFactor = 16;
    /// Reconstruction-filter stopband depth (dB) handed to the Kaiser design.
    static constexpr double kReconstructionStopbandDb = 96.0;

    /// Install the raw single-cycle table set (off the audio thread), returning
    /// whether the set was accepted. Every table must share one length `L`: a
    /// mismatched set (tables of differing length) is REJECTED — the call is a
    /// no-op that leaves the previous set intact and returns false — rather than
    /// truncated to the shortest common length, because truncating a table plays a
    /// fraction of its cycle as a full cycle (a corrupted, off-pitch waveform). An
    /// empty set is accepted and leaves the oscillator silent. Each accepted table
    /// is copied and, when `bit_depth` is not `kFullPrecision`, quantized about
    /// zero to `bit_depth` bits — on the stored table data, which is where the
    /// lo-fi grit lives.
    bool set_tables(std::vector<std::vector<double>> tables,
                    int bit_depth = kDefaultBitDepth) {
        std::size_t length = tables.empty() ? 0 : tables.front().size();
        for (const auto& t : tables) {
            if (t.size() != length) return false; // Mismatched: reject atomically.
        }

        bit_depth_ = bit_depth;
        tables_.clear();
        table_length_ = length;
        if (table_length_ > 0) {
            tables_.reserve(tables.size());
            for (auto& src : tables) {
                std::vector<double> table(table_length_, 0.0);
                for (std::size_t i = 0; i < table_length_; ++i)
                    table[i] = quantize(src[i], bit_depth_);
                tables_.push_back(std::move(table));
            }
        }
        current_table_ = std::min<int>(current_table_,
                                       tables_.empty() ? 0
                                       : static_cast<int>(tables_.size()) - 1);
        return true;
    }

    /// Set the sample rate. Safe to call again on a rate change.
    void prepare(double sample_rate) {
        if (sample_rate > 0.0) sample_rate_ = sample_rate;
    }

    /// Select the table nearest scan position `pos` (0 = first, 1 = last),
    /// clamped. HARD select: no interpolation across the scan dimension and no
    /// slew, so a scan sweep STEPS between adjacent tables — the faithful lo-fi
    /// zipper. Contrast `WtOscillator::set_position`, which slews.
    void set_scan(double pos) {
        if (tables_.size() < 2) { current_table_ = 0; return; }
        const double clamped = std::clamp(pos, 0.0, 1.0);
        const double scaled = clamped * static_cast<double>(tables_.size() - 1);
        current_table_ = static_cast<int>(std::lround(scaled));
        current_table_ = std::clamp(current_table_, 0,
                                    static_cast<int>(tables_.size()) - 1);
    }

    /// Enable (default) or disable the oversample → reconstruction → decimate
    /// stage. Disabled = raw naive point-sampling at the host rate, which folds
    /// an audible supra-Nyquist image in-band; exposed for A/B and for the rawer
    /// grit. Rebuilds the filter (off the audio thread).
    void set_reconstruction(bool enabled) {
        reconstruction_ = enabled;
        rebuild_filter();
    }

    /// Set the reconstruction oversample factor (>= 1; 1 disables the stage even
    /// when reconstruction is enabled). Rebuilds the filter (off the audio
    /// thread).
    void set_oversample_factor(int factor) {
        oversample_ = factor > 1 ? factor : 1;
        rebuild_filter();
    }

    /// Reset the playback phase to the start of the cycle and clear the
    /// reconstruction filter state.
    void reset() {
        table_phase_ = 0.0;
        std::fill(ring_.begin(), ring_.end(), 0.0);
        ring_pos_ = 0;
    }

    /// Generate one sample and advance by `increment` cycles (frequency ÷ sample
    /// rate). The table-phase clock advances by `increment · L` table samples per
    /// host sample — i.e. the table is clocked at `fs_play = f0 · L` — so the ZOH
    /// images ride at `n · L · f0` and track pitch. Playback is for non-negative
    /// increments.
    double next(double increment) {
        if (tables_.empty() || table_length_ == 0) return 0.0;
        const double step = increment * static_cast<double>(table_length_);

        if (!reconstruction_active()) {
            const double s = sample_zoh();
            advance_phase(step);
            return s;
        }

        // Oversample the staircase, push each fine sample through the
        // reconstruction FIR, and keep one decimated sample. Only the retained
        // output's convolution is computed (filter-then-decimate), so the cost is
        // one dot product per host sample regardless of the oversample factor.
        const double sub_step = step / static_cast<double>(oversample_);
        double out = 0.0;
        for (int m = 0; m < oversample_; ++m) {
            push_ring(sample_zoh());
            advance_phase(sub_step);
            if (m == oversample_ - 1) out = convolve_ring();
        }
        return out;
    }

    std::size_t table_length() const { return table_length_; }
    std::size_t table_count() const { return tables_.size(); }
    int current_table() const { return current_table_; }
    int bit_depth() const { return bit_depth_; }
    /// Read-only view of a stored table — the actual (post-quantization) sample
    /// data the reader plays. Exposed so a test can grade the engine's own
    /// quantization directly rather than re-deriving it from a private copy of the
    /// quantizer (which would pass even if the engine's quantizer drifted). Empty
    /// span for an out-of-range index.
    std::span<const double> stored_table(std::size_t i) const {
        return i < tables_.size() ? std::span<const double>(tables_[i])
                                  : std::span<const double>();
    }
    /// The configured sample rate. The reader itself is rate-agnostic — it works
    /// in per-sample increment terms and designs the reconstruction filter in
    /// frequency normalized to the oversample rate — so this is retained only for
    /// module-convention parity and host reporting.
    double sample_rate() const { return sample_rate_; }
    bool reconstruction_enabled() const { return reconstruction_; }
    int oversample_factor() const { return oversample_; }

private:
    /// Symmetric mid-tread quantizer about zero: `round(x · levels) / levels`
    /// with `levels = 2^(bits-1) − 1` (127 at 8 bits). Odd-symmetric — `q(−x) =
    /// −q(x)` — so a symmetric table keeps its half-wave symmetry and the grit
    /// stays odd-harmonic. Full precision (no-op) for a sentinel depth.
    static double quantize(double x, int bits) {
        if (bits <= 0 || bits >= 53) return x;
        const double levels = std::ldexp(1.0, bits - 1) - 1.0;
        return std::round(x * levels) / levels;
    }

    bool reconstruction_active() const {
        return reconstruction_ && oversample_ > 1 && !coeffs_.empty();
    }

    /// ZOH (nearest/floor) lookup of the current table at the current phase — no
    /// interpolation. `table_phase_` is kept in `[0, L)`, so the floor index is
    /// in range.
    double sample_zoh() const {
        const auto& table = tables_[static_cast<std::size_t>(current_table_)];
        std::size_t idx = static_cast<std::size_t>(table_phase_);
        if (idx >= table_length_) idx = table_length_ - 1; // guard fp edge only.
        return table[idx];
    }

    void advance_phase(double step) {
        table_phase_ += step;
        const double L = static_cast<double>(table_length_);
        if (table_phase_ >= L || table_phase_ < 0.0)
            table_phase_ -= L * std::floor(table_phase_ / L);
        if (table_phase_ >= L) table_phase_ = 0.0; // fp snap at the boundary.
    }

    void push_ring(double x) {
        ring_[ring_pos_] = x;
        ring_pos_ = ring_pos_ + 1 == ring_.size() ? 0 : ring_pos_ + 1;
    }

    double convolve_ring() const {
        const std::size_t n = coeffs_.size();
        double acc = 0.0;
        std::size_t idx = ring_pos_ == 0 ? n - 1 : ring_pos_ - 1; // newest sample.
        for (std::size_t j = 0; j < n; ++j) {
            acc += coeffs_[j] * ring_[idx];
            idx = idx == 0 ? n - 1 : idx - 1;
        }
        return acc;
    }

    /// Design the reconstruction lowpass at the oversampled rate. The −6 dB
    /// corner sits at the host Nyquist and the transition band straddles it, so
    /// authentic sub-Nyquist images pass (with a gentle near-Nyquist rolloff, as
    /// a real analog reconstruction filter has) while content above the host
    /// Nyquist reaches the stopband before decimation folds it back in-band. DC
    /// gain 1 (the design normalizes the tap sum), so relative image levels are
    /// unchanged in the passband. The transition is expressed as a fraction of
    /// the host Nyquist, so its absolute width stays fixed as the oversample
    /// factor changes and the tap count scales linearly with the factor.
    void rebuild_filter() {
        coeffs_.clear();
        ring_.clear();
        ring_pos_ = 0;
        if (!reconstruction_ || oversample_ <= 1) return;

        const double host_nyquist_norm = 0.5 / static_cast<double>(oversample_);
        const double cutoff = host_nyquist_norm;
        const double transition = 0.20 * host_nyquist_norm;
        const double beta = kaiser_beta_for_stopband(kReconstructionStopbandDb);
        const std::size_t taps =
            kaiser_length_for_transition(kReconstructionStopbandDb, transition);
        coeffs_ = design_windowed_sinc(taps, cutoff, beta);
        ring_.assign(coeffs_.size(), 0.0);
    }

    std::vector<std::vector<double>> tables_;
    std::size_t table_length_ = 0;
    int bit_depth_ = kDefaultBitDepth;
    int current_table_ = 0;

    double sample_rate_ = 48000.0;
    double table_phase_ = 0.0;

    bool reconstruction_ = true;
    int oversample_ = kDefaultOversampleFactor;
    std::vector<double> coeffs_;
    std::vector<double> ring_;
    std::size_t ring_pos_ = 0;
};

} // namespace pulp::signal::osc
