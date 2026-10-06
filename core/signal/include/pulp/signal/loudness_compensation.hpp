#pragma once

/// @file loudness_compensation.hpp
/// Loudness make-up for a static (or slowly edited) spectral shape, weighted
/// by the long-term spectrum of the material going through it.
///
/// This header depends on Pulp and the standard library only; it has no
/// product-specific types in its public interface. Its focused tests link
/// Pulp::signal and Catch2 only.
///
/// THE MODEL. A shaping effect's output is a blend of a WET leg (the shaped
/// signal, w, through the response H) and a DRY leg (d) at a mix m:
///
///   y = m H w + (1 - m) d
///
/// With long-term, K-weighted per-bin spectra Pww = <|W|^2>, Pdd = <|D|^2>
/// and the cross-spectrum Pwd = <W conj(D)>, the output power per bin is
///
///   m^2 |H|^2 Pww + (1-m)^2 Pdd + 2 m (1-m) Re(H Pwd)
///
/// and the make-up is the ratio of the flat (H = 1) output to this, summed
/// over bins: -10 log10(sum out / sum flat), clamped. When the wet leg IS the
/// dry leg (an ordinary effect) all three spectra are one P and this is
/// sum P |m H + 1 - m|^2 / sum P; when the wet leg is unrelated to the dry one
/// (a freeze or a hold playing while the input moves on) the cross term
/// vanishes and the two legs add as powers. Nothing has to be told which case
/// it is in: the cross-spectrum measures it. The phase of H matters only
/// through the cross term, i.e. only below 100 % mix.
///
/// THE PIPELINE, exposed stage by stage:
///
///   1. k_weighting_power()         |K(f)|^2 of BS.1770 at a frequency.
///   2. LongTermSpectrum            stateful, RT-safe: Hann/FFT frames on a
///                                  fixed sample grid, K-weighted,
///                                  energy-weighted slow (3 s) and fast (0.4 s)
///                                  averages of Pww (and, tracking a dry leg,
///                                  Pdd and Pwd); an absolute silence gate; a
///                                  level-drop rule; restart-from-fast on a
///                                  change of material; a warm start from a
///                                  saved SpectrumBands; a cold start from a
///                                  caller-supplied prior whose say decays.
///      MaterialChangeDetector      when the fast and slow make-up disagree.
///   3. MinimumPhaseResponse        the minimum-phase response with a given
///                                  magnitude (folded cepstrum), for a
///                                  realisation that designs minimum phase.
///   4. blend_makeup_gain_db()      PURE: the model above.
///      makeup_gain_db()            PURE: the single-leg, real-response form.
///      band_makeup_gain_db()       PURE: the same over bands, weighted by any
///      SpectrumCdf                 cumulative weighting.
///   5. MakeupTarget                stateful: an edit moves the target at once;
///                                  the material moves it with a slew that is
///                                  slow near the target and proportional to
///                                  the distance far from it.
///
/// RT CONTRACT. prepare() allocates (control thread). Everything else --
/// reset(), push(), MinimumPhaseResponse::compute(), the pure functions,
/// SpectrumCdf::assign() into a prepared size, MakeupTarget -- is
/// allocation-free, lock-free and reads no clock. LongTermSpectrum advances
/// on a frame grid counted in samples from prepare()/reset(), so its state
/// after N samples does not depend on how the N samples were chunked.

#include <pulp/signal/biquad.hpp>
#include <pulp/signal/fft.hpp>
#include <pulp/signal/frequency_response.hpp>
#include <pulp/signal/multi_channel_meter.hpp>
#include <pulp/signal/windowing.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pulp::signal {

// SPECTR-RENDER-PATH BEGIN -- every stage below runs on an audio thread
// (prepare() allocates, on the control thread, but reads no clock either), so
// the whole unit is held to the render-path rule: no clock, sleep, thread or
// lock (tools/ci/check_render_path_clock.py).

/// Every tunable of the stateful stages.
struct LoudnessCompensationConfig {
    /// Time constant of the material spectrum, seconds. <= 0: no smoothing
    /// (each frame replaces the estimate).
    double time_constant_seconds = 3.0;
    /// How long the prior keeps a say once material is heard: its weight
    /// falls linearly from 1 to 0 over this much audible material, so the
    /// estimate starts AT the prior and is the material's own after it. Short
    /// and finite on purpose: a prior with energy where the material has none
    /// dominates any boost there for as long as any of it lingers (an
    /// exponential fade left 20 dB of error for seconds).
    double prior_seconds = 0.5;
    /// Frames below this K-weighted loudness (LUFS-equivalent over the frame,
    /// channels summed as BS.1770 sums them) are not material: the estimate
    /// holds through them.
    double gate_lufs = -60.0;
    /// Time constant of the FAST estimate, seconds: the change detector's,
    /// and what the slow estimate restarts from when the material changes.
    double fast_time_constant_seconds = 0.4;
    /// Level-drop rule: when no audible frame in the last
    /// level_drop_window_seconds came within level_drop_db of the slow
    /// estimate's level, the estimates' LEVEL is rescaled to the recent one
    /// so quieter material is not outweighed by the louder past. It never
    /// restarts the spectrum (a decay is not a change of material). <= 0: off.
    double level_drop_db = 10.0;
    double level_drop_window_seconds = 4.0;
    /// The same for the live (dry) leg, which has no change detector.
    double dry_level_drop_window_seconds = 0.6;
    /// Frames are weighted by their power relative to a running average of
    /// frame power over this long (see LongTermSpectrum's frame_): energy
    /// weighting within a groove, not across a swell or a fade. <= 0: each
    /// frame weighs the same.
    double local_level_seconds = 2.0;
    double level_weight_exponent = 1.0;
    /// Time constant of the slow estimate while the material alternates
    /// between two remembered states (see LongTermSpectrum::merge_with_previous).
    double alternating_time_constant_seconds = 8.0;
    /// How long the material before the last change is remembered.
    double memory_seconds = 12.0;
    /// Slowest rate the material moves the make-up target, dB/s (<= 0: no
    /// limit) -- and the proportional speed-up for a far target (see
    /// MakeupTarget::follow).
    double material_slew_db_per_second = 6.0;
    double material_slew_rate_per_second = 6.0;
    double material_slew_max_db_per_second = 80.0;
    /// The proportional speed-up counts only the distance beyond this, so a
    /// groove's wobble around the target stays on the slow floor.
    double material_slew_knee_db = 0.0;
    /// Change detector (MaterialChangeDetector): the fast and slow estimates'
    /// make-up for the current shape disagree by more than this for this many
    /// audible frames.
    double change_threshold_db = 4.0;
    double change_seconds = 1.0;
    double change_huge_db = 12.0;
    double change_huge_seconds = 0.5;
    /// Quieter after louder (MaterialChangeDetector::drop_threshold_db): a
    /// gap above change_drop_threshold_db counts while the level is at least
    /// change_drop_level_db below its slow average, for material whose level
    /// does not fall faster than change_max_decay_db_per_second. <= 0: off.
    double change_drop_threshold_db = 0.0;
    double change_drop_level_db = 3.0;
    double change_max_decay_db_per_second = 6.0;
    double change_drop_min_loud_seconds = 3.0;
    /// Make-up range.
    float max_cut_db = 24.0f;
    float max_boost_db = 24.0f;
    /// false: every bin weighs the same (ignores the material and K). Exists
    /// as a measurable negative control, not as a mode.
    bool weight_by_material = true;
    /// true: push() takes a dry leg too, and the estimator keeps Pdd and Pwd.
    bool track_dry_leg = false;
    /// Analysis frame length, seconds; the FFT size is the next power of two
    /// at or above it (4096 at 44.1/48 kHz). Hop is half a frame.
    double frame_seconds = 0.08;
    /// > 0: use exactly this FFT size instead (a power of two), so the
    /// estimate shares a bin grid with a response sampled on it.
    int fft_size = 0;
};

struct MakeupLimits {
    float max_cut_db = 24.0f;
    float max_boost_db = 24.0f;
};

/// The long-term spectra of the two legs on one bin grid, K-weighted, on a
/// common scale. Single-leg estimates have dd and wd_re equal to ww and
/// wd_im zero.
struct LegSpectra {
    std::span<const double> ww;
    std::span<const double> dd;
    std::span<const double> wd_re;
    std::span<const double> wd_im;
};

// ── 1. K-weighting ──────────────────────────────────────────────────────────

/// |K(f)|^2 for BS.1770 K-weighting at @p sample_rate, through Pulp's own
/// K-weighting design and biquad response evaluator. 1 where the rate cannot
/// represent the design.
[[nodiscard]] inline double k_weighting_power(double hz,
                                              const pulp::signal::KWeightingCoefficients& k,
                                              double sample_rate) noexcept {
    if (!k.valid || !(sample_rate > 0.0))
        return 1.0;
    const auto to_biquad = [](const pulp::signal::LoudnessBiquadCoefficients& c) {
        return pulp::signal::BiquadCoefficientsT<double>{c.b0, c.b1, c.b2, c.a1, c.a2};
    };
    const double omega = pulp::signal::angular_frequency(hz, sample_rate);
    const double m = pulp::signal::section_magnitude(to_biquad(k.shelf), omega) *
                     pulp::signal::section_magnitude(to_biquad(k.high_pass), omega);
    return m * m;
}

// ── 4. The pure make-up ────────────────────────────────────────────────────

[[nodiscard]] inline float clamp_makeup_db_(double ratio, MakeupLimits limits) noexcept {
    if (!(ratio > 0.0))
        return limits.max_boost_db;
    return static_cast<float>(std::clamp(-10.0 * std::log10(ratio),
                                         -static_cast<double>(limits.max_cut_db),
                                         static_cast<double>(limits.max_boost_db)));
}

/// Make-up, dB, for the two-leg model in the file comment: @p response is
/// the wet leg's complex response per bin, @p s the legs' spectra on the same
/// grid, @p mix the wet share. 0 dB when there is nothing to measure; the
/// boost limit when the response removes everything.
[[nodiscard]] inline float blend_makeup_gain_db(std::span<const std::complex<double>> response,
                                                const LegSpectra& s, double mix,
                                                MakeupLimits limits) noexcept {
    const std::size_t n =
        std::min({response.size(), s.ww.size(), s.dd.size(), s.wd_re.size(), s.wd_im.size()});
    const double m = std::clamp(mix, 0.0, 1.0);
    const double a = m * m, b = (1.0 - m) * (1.0 - m), c = 2.0 * m * (1.0 - m);
    double out = 0.0, flat = 0.0;
    for (std::size_t k = 0; k < n; ++k) {
        const auto h = response[k];
        out += a * std::norm(h) * s.ww[k] + b * s.dd[k] +
               c * (h.real() * s.wd_re[k] - h.imag() * s.wd_im[k]);
        flat += a * s.ww[k] + b * s.dd[k] + c * s.wd_re[k];
    }
    if (!(flat > 0.0))
        return 0.0f;
    return clamp_makeup_db_(std::max(0.0, out) / flat, limits);
}

/// The single-leg, real-response form: per-bin power response |H|^2 against
/// a per-bin power spectrum on the same grid. A flat response is 0 dB.
[[nodiscard]] inline float makeup_gain_db(std::span<const double> response_power,
                                          std::span<const double> spectrum_power,
                                          MakeupLimits limits) noexcept {
    const std::size_t n = std::min(response_power.size(), spectrum_power.size());
    double total = 0.0, energy = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double w = spectrum_power[i];
        if (!(w > 0.0))
            continue;
        total += w;
        energy += w * std::max(0.0, response_power[i]);
    }
    if (!(total > 0.0))
        return 0.0f;
    return clamp_makeup_db_(energy / total, limits);
}

/// One band of a band-wise response: its frequency span and power gain.
struct BandPowerGain {
    double lo_hz = 0.0;
    double hi_hz = 0.0;
    double power_gain = 1.0;
};

/// The single-leg form over bands, each weighted by `weight(lo_hz, hi_hz)`
/// -- the material's energy between two frequencies (SpectrumCdf::weight, or
/// any reference weighting).
template <typename WeightFn>
[[nodiscard]] inline float band_makeup_gain_db(std::span<const BandPowerGain> bands,
                                               WeightFn&& weight, MakeupLimits limits) noexcept {
    double total = 0.0, energy = 0.0;
    for (const auto& band : bands) {
        const double w = weight(band.lo_hz, band.hi_hz);
        if (!(w > 0.0))
            continue;
        total += w;
        energy += w * std::max(0.0, band.power_gain);
    }
    if (!(total > 0.0))
        return 0.0f;
    return clamp_makeup_db_(energy / total, limits);
}

/// The normalised cumulative of a power spectrum on a uniform bin grid. Bin k
/// spans [(k - 1/2) df, (k + 1/2) df]; the energy is linear inside a bin.
class SpectrumCdf {
  public:
    /// Control thread: size for @p bins.
    void prepare(std::size_t bins) {
        cumulative_.assign(bins + 1, 0.0);
    }

    /// Allocation-free once prepared for at least power.size() bins.
    void assign(std::span<const double> power, double bin_hz) noexcept {
        bin_hz_ = bin_hz > 0.0 ? bin_hz : 1.0;
        bins_ = std::min(power.size(), cumulative_.empty() ? 0 : cumulative_.size() - 1);
        if (bins_ == 0)
            return;
        cumulative_[0] = 0.0;
        double sum = 0.0;
        for (std::size_t k = 0; k < bins_; ++k) {
            sum += std::max(0.0, power[k]);
            cumulative_[k + 1] = sum;
        }
        if (sum > 0.0)
            for (std::size_t k = 0; k <= bins_; ++k)
                cumulative_[k] /= sum;
    }

    /// Energy between two frequencies; the whole spectrum weighs 1.
    [[nodiscard]] double weight(double lo_hz, double hi_hz) const noexcept {
        if (bins_ == 0 || !(hi_hz > lo_hz))
            return 0.0;
        return at_(hi_hz) - at_(lo_hz);
    }

  private:
    [[nodiscard]] double at_(double hz) const noexcept {
        const double x = std::max(0.0, hz / bin_hz_ + 0.5);
        const auto i = static_cast<std::size_t>(std::floor(x));
        if (i >= bins_)
            return cumulative_[bins_];
        const double t = x - static_cast<double>(i);
        return cumulative_[i] + t * (cumulative_[i + 1] - cumulative_[i]);
    }

    std::vector<double> cumulative_;
    std::size_t bins_ = 0;
    double bin_hz_ = 1.0;
};

// ── 3. Minimum-phase response ───────────────────────────────────────────────

/// The minimum-phase response with a given magnitude on an FFT grid, by the
/// folded real cepstrum: ln|H| -> cepstrum -> fold onto positive quefrency ->
/// exp(FFT). The magnitude is floored at @p floor first, as a minimum-phase
/// designer floors it.
class MinimumPhaseResponse {
  public:
    /// Control thread; allocates. @p fft_size: the grid (bins = N/2 + 1).
    void prepare(int fft_size) {
        n_ = fft_size;
        fft_ = std::make_unique<pulp::signal::FftT<float>>(n_);
        work_.assign(static_cast<std::size_t>(n_), {});
    }
    [[nodiscard]] bool prepared() const noexcept {
        return fft_ && fft_->ready();
    }

    /// @p magnitude: N/2 + 1 linear magnitudes. @p out: N/2 + 1 values.
    bool compute(std::span<const double> magnitude, double floor,
                 std::span<std::complex<double>> out) noexcept {
        const auto bins = static_cast<std::size_t>(n_ / 2 + 1);
        if (!prepared() || magnitude.size() < bins || out.size() < bins)
            return false;
        const double lo = floor > 0.0 ? floor : 1.0e-12;
        for (std::size_t k = 0; k < bins; ++k)
            work_[k] = {static_cast<float>(std::log(std::max(lo, magnitude[k]))), 0.0f};
        for (std::size_t k = bins; k < static_cast<std::size_t>(n_); ++k)
            work_[k] = work_[static_cast<std::size_t>(n_) - k];
        fft_->inverse(work_.data()); // real, even cepstrum
        const auto half = static_cast<std::size_t>(n_ / 2);
        for (std::size_t q = 1; q < half; ++q) {
            work_[q] = {2.0f * work_[q].real(), 0.0f};
            work_[static_cast<std::size_t>(n_) - q] = {0.0f, 0.0f};
        }
        work_[0] = {work_[0].real(), 0.0f};
        work_[half] = {work_[half].real(), 0.0f};
        fft_->forward(work_.data()); // ln|H| + j phase
        for (std::size_t k = 0; k < bins; ++k)
            out[k] = std::exp(std::complex<double>(work_[k].real(), work_[k].imag()));
        return true;
    }

  private:
    int n_ = 0;
    std::unique_ptr<pulp::signal::FftT<float>> fft_;
    std::vector<std::complex<float>> work_;
};

// ── 2. The long-term spectrum ──────────────────────────────────────────────

/// A band-compressed, rate-independent copy of a LongTermSpectrum's estimate,
/// for saving with a session and restoring warm. kBands log-spaced bands from
/// 10 Hz to 24 kHz (the first band reaches down to DC, the last up to
/// Nyquist). Powers are normalised so the wet leg sums to 1; `level_ms` is
/// the wet leg's K-weighted mean square, which restores the level the
/// level-drop rule compares against.
struct SpectrumBands {
    static constexpr int kBands = 160;
    static constexpr double kLowHz = 10.0;
    static constexpr double kHighHz = 24000.0;
    bool valid = false;
    double level_ms = 0.0;
    std::array<float, kBands> ww{}, dd{}, re{}, im{};

    [[nodiscard]] static int band_of(double hz) noexcept {
        if (!(hz > kLowHz))
            return 0;
        const double x =
            std::log(hz / kLowHz) / std::log(kHighHz / kLowHz) * static_cast<double>(kBands);
        return std::clamp(static_cast<int>(std::floor(x)), 0, kBands - 1);
    }
    [[nodiscard]] static double centre_hz(int band) noexcept {
        return kLowHz * std::pow(kHighHz / kLowHz,
                                 (static_cast<double>(band) + 0.5) / static_cast<double>(kBands));
    }
};

/// Detects a genuine change of material from the gap between the make-up the
/// fast and the slow estimates give the current shape. The gap must persist:
/// a leaky count -- up one for each audible frame above `threshold_db`, down
/// two for each below -- must reach `frames` (`huge_frames` while the gap is
/// above `huge_db`, for a change nobody could miss). So material that is
/// itself uneven (a groove: a kick frame, then a hat frame) still confirms,
/// and a passing moment -- a hit, a fill, part of a decay -- drains away and
/// is forgotten. Pure bookkeeping; deterministic.
class MaterialChangeDetector {
  public:
    double threshold_db = 4.0;
    int frames = 12;
    double huge_db = 12.0;
    int huge_frames = 6;
    int holdoff_frames = 6;
    /// Quieter after louder. When the frames' level has fallen at least
    /// drop_level_db below its slow average, a smaller gap -- drop_threshold_db
    /// -- counts as a change too: a quieter section after a loud one is a new
    /// part, and the energy-weighted estimate is slowest to give the louder
    /// past up exactly then. A decay (a hit's tail, a fade) also falls in
    /// level, so this path confirms only material whose level is steady over
    /// the run: its fitted slope must not fall faster than max_decay_db_per_frame.
    /// drop_threshold_db <= 0 turns the path off.
    double drop_threshold_db = 0.0;
    double drop_level_db = 3.0;
    double max_decay_db_per_frame = 0.0;
    /// ... and only after louder material that lasted: the level must have
    /// stayed within half of drop_level_db of (or above) its slow average for
    /// this many frames before it fell, so a short loud burst in a fast
    /// alternation is left to the alternation memory.
    int drop_min_loud_frames = 0;
    /// Per-frame coefficients of the fast and slow level averages (power).
    double level_fast_alpha = 0.3;
    double level_slow_alpha = 0.03;

    enum class Event { none, run_started, run_broken, confirmed };

    void reset() noexcept {
        run_ = 0;
        huge_run_ = 0;
        holdoff_ = 0;
        level_fast_ = level_slow_ = 0.0;
        close_run_ = last_close_run_ = level_frames_ = 0;
        clear_fit_();
    }
    [[nodiscard]] int run() const noexcept {
        return run_;
    }
    /// The fast level's fall below the slow one, dB (>= 0 while quieter).
    [[nodiscard]] double level_drop_db() const noexcept {
        return level_fast_ > 0.0 && level_slow_ > 0.0 ? 10.0 * std::log10(level_slow_ / level_fast_)
                                                      : 0.0;
    }
    /// Whether the last confirmation came through the quieter-after-louder path.
    [[nodiscard]] bool last_was_drop() const noexcept {
        return last_drop_;
    }

    /// One audible frame: the make-up the fast and slow shapes give, and
    /// (optional) the frame's loudness, for the quieter-after-louder path.
    Event observe(float fast_db, float slow_db, double frame_lufs = kNoLevel) noexcept {
        const bool levelled = frame_lufs > kNoLevel;
        if (levelled) {
            const double p = std::pow(10.0, frame_lufs / 10.0);
            level_fast_ =
                level_fast_ > 0.0 ? level_fast_ + level_fast_alpha * (p - level_fast_) : p;
            level_slow_ =
                level_slow_ > 0.0 ? level_slow_ + level_slow_alpha * (p - level_slow_) : p;
            // The slow level means something only once it spans its own
            // time constant (about drop_min_loud_frames).
            if (++level_frames_ <= drop_min_loud_frames) {
            } else if (level_drop_db() < 0.5 * drop_level_db) {
                ++close_run_;
            } else if (close_run_ > 0) {
                last_close_run_ = close_run_;
                close_run_ = 0;
            }
        }
        if (holdoff_ > 0) {
            --holdoff_;
            return end_run_();
        }
        const double gap = std::abs(static_cast<double>(fast_db) - slow_db);
        const bool dropped = levelled && drop_threshold_db > 0.0 &&
                             level_drop_db() >= drop_level_db &&
                             last_close_run_ >= drop_min_loud_frames;
        const double threshold = dropped ? std::min(threshold_db, drop_threshold_db) : threshold_db;
        if (!(gap > threshold)) {
            if (run_ == 0)
                return Event::none;
            run_ = std::max(0, run_ - 2);
            huge_run_ = std::max(0, huge_run_ - 2);
            if (run_ == 0)
                clear_fit_();
            return run_ == 0 ? Event::run_broken : Event::none;
        }
        const bool started = run_ == 0;
        if (started)
            clear_fit_();
        ++run_;
        if (levelled)
            fit_(frame_lufs);
        huge_run_ = gap > huge_db ? huge_run_ + 1 : std::max(0, huge_run_ - 2);
        if (run_ >= frames || huge_run_ >= huge_frames) {
            // Below the ordinary threshold only through the quieter path,
            // and only for steady material (not a decay).
            const bool via_drop = !(gap > threshold_db) && huge_run_ < huge_frames;
            if (via_drop && slope_() < -max_decay_db_per_frame) {
                // A decay: keep the run, never confirm it on this path.
                run_ = frames - 1;
                return Event::none;
            }
            run_ = 0;
            huge_run_ = 0;
            clear_fit_();
            holdoff_ = holdoff_frames;
            last_drop_ = via_drop;
            return Event::confirmed;
        }
        return started ? Event::run_started : Event::none;
    }
    void hold_off() noexcept {
        end_run_();
        holdoff_ = holdoff_frames;
    }

    static constexpr double kNoLevel = -1.0e300;

  private:
    Event end_run_() noexcept {
        const bool had = run_ > 0;
        run_ = 0;
        huge_run_ = 0;
        clear_fit_();
        return had ? Event::run_broken : Event::none;
    }
    void clear_fit_() noexcept {
        fit_n_ = 0;
        fit_x_ = fit_y_ = fit_xx_ = fit_xy_ = 0.0;
    }
    void fit_(double y) noexcept {
        const double x = static_cast<double>(fit_n_++);
        fit_x_ += x;
        fit_y_ += y;
        fit_xx_ += x * x;
        fit_xy_ += x * y;
    }
    // Least-squares slope of the run's frame loudness, dB per frame.
    [[nodiscard]] double slope_() const noexcept {
        if (fit_n_ < 3)
            return 0.0;
        const double n = static_cast<double>(fit_n_);
        const double den = n * fit_xx_ - fit_x_ * fit_x_;
        return den > 0.0 ? (n * fit_xy_ - fit_x_ * fit_y_) / den : 0.0;
    }
    int run_ = 0;
    int huge_run_ = 0;
    int holdoff_ = 0;
    double level_fast_ = 0.0, level_slow_ = 0.0;
    int close_run_ = 0, last_close_run_ = 0, level_frames_ = 0;
    int fit_n_ = 0;
    double fit_x_ = 0.0, fit_y_ = 0.0, fit_xx_ = 0.0, fit_xy_ = 0.0;
    bool last_drop_ = false;
};

class LongTermSpectrum {
  public:
    /// Control thread; allocates. @p prior: per-bin power on this estimator's
    /// grid (see bins_for()), any scale; empty means flat. The estimate IS the
    /// prior until the first audible frame (both legs, coherent).
    void prepare(double sample_rate, int channels, const LoudnessCompensationConfig& config,
                 std::span<const double> prior = {}) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
        channels_ = std::max(1, channels);
        config_ = config;
        fft_size_ = fft_size_for(sample_rate_, config);
        hop_ = fft_size_ / 2;
        bins_ = fft_size_ / 2 + 1;
        bin_hz_ = sample_rate_ / static_cast<double>(fft_size_);
        const auto nb = static_cast<std::size_t>(bins_);
        fft_ = std::make_unique<pulp::signal::FftT<float>>(fft_size_);
        window_ = pulp::signal::WindowFunction::generate<float>(
            fft_size_, pulp::signal::WindowFunction::Type::hann);
        const int legs = config.track_dry_leg ? 2 : 1;
        history_.assign(static_cast<std::size_t>(legs * channels_ * fft_size_), 0.0f);
        frame_in_.assign(static_cast<std::size_t>(fft_size_), 0.0f);
        wet_spectrum_.assign(static_cast<std::size_t>(fft_size_), {});
        dry_spectrum_.assign(static_cast<std::size_t>(fft_size_), {});
        for (auto* v : {&frame_ww_, &frame_dd_, &frame_re_, &frame_im_, &k_power_, &prior_})
            v->assign(nb, 0.0);
        slow_.assign(nb);
        fast_.assign(nb);
        fast_shape_.assign(nb, 0.0);
        slow_shape_.assign(nb, 0.0);
        candidate_.assign(nb);
        previous_.assign(nb);
        previous_shape_.assign(nb, 0.0);
        arrived_shape_.assign(nb, 0.0);
        alt_a_shape_.assign(nb, 0.0);
        alt_b_shape_.assign(nb, 0.0);
        size_published_();
        flat_.assign(nb, 1.0);
        zero_.assign(nb, 0.0);
        cdf_.prepare(nb);
        double window_energy = 0.0;
        for (const float w : window_)
            window_energy += static_cast<double>(w) * w;
        // Parseval over the half spectrum (FftT::forward_real is unit-scaled):
        // mean square of the windowed frame = 2 sum_half |X|^2 / (N sum w^2).
        power_to_mean_square_ =
            window_energy > 0.0 ? 2.0 / (static_cast<double>(fft_size_) * window_energy) : 0.0;
        const auto k = pulp::signal::k_weighting_coefficients(sample_rate_);
        for (int b = 0; b < bins_; ++b)
            k_power_[static_cast<std::size_t>(b)] =
                k_weighting_power(static_cast<double>(b) * bin_hz_, k, sample_rate_);
        double total = 0.0;
        if (prior.size() >= nb)
            for (std::size_t b = 0; b < nb; ++b)
                total += std::max(0.0, prior[b]);
        for (std::size_t b = 0; b < nb; ++b)
            prior_[b] =
                total > 0.0 ? std::max(0.0, prior[b]) / total : 1.0 / static_cast<double>(nb);
        const double hop_seconds = static_cast<double>(hop_) / sample_rate_;
        alpha_ = config.time_constant_seconds > 0.0
                     ? 1.0 - std::exp(-hop_seconds / config.time_constant_seconds)
                     : 1.0;
        fast_alpha_ = config.fast_time_constant_seconds > 0.0
                          ? 1.0 - std::exp(-hop_seconds / config.fast_time_constant_seconds)
                          : 1.0;
        prior_step_ = config.prior_seconds > 0.0 ? hop_seconds / config.prior_seconds : 1.0;
        local_alpha_ = config.local_level_seconds > 0.0
                           ? 1.0 - std::exp(-hop_seconds / config.local_level_seconds)
                           : 1.0;
        alt_alpha_ = config.alternating_time_constant_seconds > 0.0
                         ? 1.0 - std::exp(-hop_seconds / config.alternating_time_constant_seconds)
                         : alpha_;
        memory_frames_ = static_cast<std::int64_t>(std::ceil(config.memory_seconds / hop_seconds));
        geometry_rate_ = sample_rate_;
        geometry_channels_ = channels_;
        level_drop_frames_ =
            std::clamp(static_cast<int>(std::ceil(config.level_drop_window_seconds / hop_seconds)),
                       1, kMaxRecent);
        dry_drop_frames_ = std::clamp(
            static_cast<int>(std::ceil(config.dry_level_drop_window_seconds / hop_seconds)), 1,
            kMaxRecent);
        prepared_ = fft_ && fft_->ready() && static_cast<int>(window_.size()) == fft_size_;
        reset();
    }

    /// The FFT size prepare() picks for a rate and config.
    [[nodiscard]] static int fft_size_for(double sample_rate,
                                          const LoudnessCompensationConfig& config) noexcept {
        if (config.fft_size > 0) {
            int n = 64;
            while (n < config.fft_size && n < 65536)
                n *= 2;
            return n;
        }
        int n = 1024;
        while (static_cast<double>(n) < config.frame_seconds * sample_rate && n < 65536)
            n *= 2;
        return n;
    }
    [[nodiscard]] static int bins_for(double sample_rate,
                                      const LoudnessCompensationConfig& config) noexcept {
        return fft_size_for(sample_rate, config) / 2 + 1;
    }

    /// Cold start: back to the prior, and the frame grid restarts at the next
    /// sample.
    void reset() noexcept {
        slow_.clear();
        fast_.clear();
        slow_frames_ = 0;
        fast_frames_ = 0;
        shape_frames_ = 0;
        forget_memory_();
        candidate_frames_ = 0;
        candidate_active_ = false;
        std::fill(fast_shape_.begin(), fast_shape_.end(), 0.0);
        std::fill(slow_shape_.begin(), slow_shape_.end(), 0.0);
        prior_weight_ = 1.0;
        prior_fade_scale_ = 1.0;
        level_local_ = 0.0;
        observed_frames_ = 0;
        gated_frames_ = 0;
        level_drops_ = 0;
        restarts_ = 0;
        recent_count_ = 0;
        recent_pos_ = 0;
        drop_run_ = 0;
        settle_frames_ = 0;
        restart_grid();
        publish_();
    }

    /// A transport jump or host reset: the frame grid restarts at the next
    /// sample (so how the stream is chopped still cannot matter), and the
    /// estimate -- what the material sounds like -- is kept.
    void restart_grid() noexcept {
        std::fill(history_.begin(), history_.end(), 0.0f);
        write_pos_ = 0;
        filled_ = 0;
        hop_pos_ = 0;
    }

    /// A detector run began: collect the would-be new material's own energy
    /// average, so a confirmed change starts from what has been heard since,
    /// not from zero. The frames whose windows still reach back across the
    /// change (one window from the frame that began the run) are left out:
    /// energy-weighted, a little of a louder past would bias it for seconds.
    void begin_candidate() noexcept {
        candidate_.clear();
        candidate_frames_ = 0;
        candidate_active_ = true;
        candidate_skip_ = fft_size_ / std::max(1, hop_) - 1;
    }
    /// The run broke: that was a passing moment, not new material.
    void cancel_candidate() noexcept {
        candidate_active_ = false;
        candidate_frames_ = 0;
    }

    /// The material changed. Both estimates start again from the candidate
    /// (the new material's energy average since the change was first seen),
    /// or empty when there is none, and go on as running means. Energy
    /// weighting is why: seeded from anything that still holds the old
    /// material, a louder past would outweigh quieter new material for
    /// seconds. The material before is remembered (merge_with_previous), and
    /// the detector's slow shape is re-armed on the new material.
    void restart() noexcept {
        if (slow_frames_ > 0) {
            previous_.copy_from(slow_);
            std::copy(slow_shape_.begin(), slow_shape_.end(), previous_shape_.begin());
            previous_valid_ = true;
            frames_since_previous_ = 0;
        }
        alternating_ = false;
        if (candidate_active_ && candidate_frames_ > 0) {
            slow_.copy_from(candidate_);
            fast_.copy_from(candidate_);
            slow_frames_ = candidate_frames_;
            fast_frames_ = std::min(candidate_frames_, fast_equivalent_frames_());
        } else {
            slow_.clear();
            fast_.clear();
            slow_frames_ = 0;
            fast_frames_ = 0;
        }
        cancel_candidate();
        std::copy(fast_shape_.begin(), fast_shape_.end(), slow_shape_.begin());
        std::copy(fast_shape_.begin(), fast_shape_.end(), arrived_shape_.begin());
        arrived_valid_ = true;
        frames_since_arrival_ = 0;
        rearm_shapes_();
        recent_count_ = 0;
        drop_run_ = 0;
        ++restarts_;
        if (slow_frames_ > 0)
            publish_();
    }

    /// The material went back to what it was before the last change: it
    /// alternates (a kick-only bar and a full one, a hit and its tail). The
    /// slow estimate becomes the mix of both and averages over
    /// alternating_time_constant_seconds from here, so the gain holds steady
    /// instead of chasing each half.
    void merge_with_previous() noexcept {
        if (!previous_valid_)
            return;
        std::copy(previous_shape_.begin(), previous_shape_.end(), alt_a_shape_.begin());
        std::copy(slow_shape_.begin(), slow_shape_.end(), alt_b_shape_.begin());
        const auto mix = [](std::vector<double>& a, const std::vector<double>& b) {
            for (std::size_t k = 0; k < a.size(); ++k)
                a[k] = 0.5 * (a[k] + b[k]);
        };
        mix(slow_.ww, previous_.ww);
        mix(slow_.dd, previous_.dd);
        mix(slow_.re, previous_.re);
        mix(slow_.im, previous_.im);
        for (std::size_t k = 0; k < slow_shape_.size(); ++k)
            slow_shape_[k] = 0.5 * (alt_a_shape_[k] + alt_b_shape_[k]);
        slow_frames_ = std::max<std::int64_t>(
            slow_frames_, static_cast<std::int64_t>(std::ceil(1.0 / std::max(alpha_, 1e-9))));
        alternating_ = true;
        previous_valid_ = false;
        cancel_candidate();
        ++merges_;
        publish_();
    }
    /// The material came back to what the LAST restart went to, after
    /// something else in between that never confirmed a change (it alternates,
    /// and the slow estimate already holds both halves): stop chasing it --
    /// keep the estimate, average over alternating_time_constant_seconds, and
    /// remember both halves.
    void settle_alternation() noexcept {
        std::copy(arrived_shape_.begin(), arrived_shape_.end(), alt_a_shape_.begin());
        std::copy(slow_shape_.begin(), slow_shape_.end(), alt_b_shape_.begin());
        alternating_ = true;
        arrived_valid_ = false;
        previous_valid_ = false;
        cancel_candidate();
        ++merges_;
    }
    [[nodiscard]] bool has_arrived() const noexcept {
        return arrived_valid_;
    }
    [[nodiscard]] LegSpectra arrived_shape_legs() const noexcept {
        if (!config_.weight_by_material)
            return {flat_, flat_, flat_, zero_};
        return {arrived_shape_, arrived_shape_, arrived_shape_, zero_};
    }
    [[nodiscard]] bool has_previous() const noexcept {
        return previous_valid_;
    }
    [[nodiscard]] bool alternating() const noexcept {
        return alternating_;
    }
    [[nodiscard]] LegSpectra previous_shape_legs() const noexcept {
        if (!config_.weight_by_material)
            return {flat_, flat_, flat_, zero_};
        return {previous_shape_, previous_shape_, previous_shape_, zero_};
    }
    /// The two remembered halves of an alternation (0: the earlier, 1: the later).
    [[nodiscard]] LegSpectra alternate_shape_legs(int which) const noexcept {
        if (!config_.weight_by_material)
            return {flat_, flat_, flat_, zero_};
        const auto& v = which == 0 ? alt_a_shape_ : alt_b_shape_;
        return {v, v, v, zero_};
    }
    [[nodiscard]] std::uint64_t merges() const noexcept {
        return merges_;
    }
    /// The estimator has the geometry prepare() would give these arguments.
    [[nodiscard]] bool prepared_for(double sample_rate, int channels,
                                    const LoudnessCompensationConfig& config) const noexcept {
        return prepared_ && geometry_rate_ == sample_rate &&
               geometry_channels_ == std::max(1, channels) &&
               fft_size_ == fft_size_for(sample_rate, config);
    }
    /// Audible frames averaged since the last restart (or cold start).
    [[nodiscard]] std::int64_t frames_since_restart() const noexcept {
        return slow_frames_;
    }

    /// Freeze released: the wet leg becomes the live input again, whose
    /// spectrum the dry leg kept warm through the hold. Both estimates take
    /// the FAST dry estimate (the live input as it is now, not as it was
    /// when the hold began), and the slow one goes on as a running mean.
    void wet_becomes_dry() noexcept {
        if (!config_.track_dry_leg)
            return;
        std::copy(fast_.dd.begin(), fast_.dd.end(), fast_.ww.begin());
        std::copy(fast_.dd.begin(), fast_.dd.end(), fast_.re.begin());
        std::fill(fast_.im.begin(), fast_.im.end(), 0.0);
        slow_.copy_from(fast_);
        slow_frames_ = std::max<std::int64_t>(1, fast_equivalent_frames_());
        {
            const double t = fast_.total();
            for (int b = 0; b < bins_; ++b) {
                const auto i = static_cast<std::size_t>(b);
                slow_shape_[i] = fast_shape_[i] = t > 0.0 ? fast_.ww[i] / t : 0.0;
            }
        }
        rearm_shapes_();
        forget_memory_();
        cancel_candidate();
        recent_count_ = 0;
        // The next frames' windows still hold the hold's last moments: they
        // describe neither leg, so they wait.
        settle_frames_ = fft_size_ / std::max(1, hop_);
        publish_();
    }

    /// Freeze engaged: the wet leg is now a held past, unrelated to the live
    /// input from here on.
    void legs_uncorrelated() noexcept {
        if (!config_.track_dry_leg)
            return;
        for (auto* e : {&slow_, &fast_}) {
            std::fill(e->re.begin(), e->re.end(), 0.0);
            std::fill(e->im.begin(), e->im.end(), 0.0);
        }
        publish_();
    }

    /// Feed planar samples of the wet leg and, when tracking it, the dry leg
    /// (nullptr: the dry leg is the wet one). `on_frame(samples_consumed)`
    /// runs after every frame the call completes (audible or gated), with the
    /// estimate already updated.
    template <typename OnFrame>
    void push(const float* const* wet, const float* const* dry, int channels, int num_samples,
              OnFrame&& on_frame) noexcept {
        if (!prepared_ || wet == nullptr || num_samples <= 0)
            return;
        const int count = std::min(channels, channels_);
        const bool two = config_.track_dry_leg;
        int done = 0;
        while (done < num_samples) {
            const int chunk = std::min(num_samples - done, hop_ - hop_pos_);
            for (int leg = 0; leg < (two ? 2 : 1); ++leg) {
                const float* const* x = leg == 0 ? wet : (dry != nullptr ? dry : wet);
                for (int ch = 0; ch < count; ++ch)
                    record_(ring_(leg, ch), x[ch], done, chunk);
            }
            write_pos_ = (write_pos_ + chunk) % fft_size_;
            filled_ = std::min(fft_size_, filled_ + chunk);
            hop_pos_ += chunk;
            done += chunk;
            if (hop_pos_ == hop_) {
                hop_pos_ = 0;
                if (filled_ == fft_size_) {
                    frame_();
                    on_frame(done);
                }
            }
        }
    }
    template <typename OnFrame>
    void push(const float* const* x, int channels, int num_samples, OnFrame&& on_frame) noexcept {
        push(x, nullptr, channels, num_samples, on_frame);
    }
    void push(const float* const* x, int channels, int num_samples) noexcept {
        push(x, nullptr, channels, num_samples, [](int) {});
    }

    // ── session state ──────────────────────────────────────────────────────
    /// The slow estimate, band-compressed (allocation-free).
    void export_bands(SpectrumBands& out) const noexcept {
        out = SpectrumBands{};
        if (!prepared_ || (observed_frames_ == 0 && prior_weight_ >= 1.0))
            return;
        const auto& e = published_;
        double total = 0.0;
        for (int b = 0; b < bins_; ++b) {
            const auto i = static_cast<std::size_t>(b);
            const int band = SpectrumBands::band_of(static_cast<double>(b) * bin_hz_);
            out.ww[static_cast<std::size_t>(band)] += static_cast<float>(e.ww[i]);
            out.dd[static_cast<std::size_t>(band)] += static_cast<float>(e.dd[i]);
            out.re[static_cast<std::size_t>(band)] += static_cast<float>(e.re[i]);
            out.im[static_cast<std::size_t>(band)] += static_cast<float>(e.im[i]);
            total += e.ww[i];
        }
        if (!(total > 0.0))
            return;
        for (int band = 0; band < SpectrumBands::kBands; ++band) {
            const auto j = static_cast<std::size_t>(band);
            out.ww[j] = static_cast<float>(out.ww[j] / total);
            out.dd[j] = static_cast<float>(out.dd[j] / total);
            out.re[j] = static_cast<float>(out.re[j] / total);
            out.im[j] = static_cast<float>(out.im[j] / total);
        }
        out.level_ms = level_local_ * power_to_mean_square_;
        out.valid = true;
    }

    /// How import_bands() treats a saved estimate.
    enum class Import {
        /// As a better prior: playback starts at the saved estimate's level
        /// and the material's own estimate takes over exactly as from a cold
        /// start (a restored session may play different material).
        prior,
        /// As the estimate itself (the same stream, carried across a change
        /// of geometry).
        warm,
    };

    /// A band's energy is spread evenly over the bins whose centres fall in
    /// it (or given to the bin nearest its centre). The frame grid restarts.
    void import_bands(const SpectrumBands& in, Import how = Import::warm) noexcept {
        if (!prepared_ || !in.valid || !(in.level_ms > 0.0) || !(power_to_mean_square_ > 0.0))
            return;
        std::array<int, SpectrumBands::kBands> count{};
        for (int b = 0; b < bins_; ++b)
            ++count[static_cast<std::size_t>(
                SpectrumBands::band_of(static_cast<double>(b) * bin_hz_))];
        Estimate& e = candidate_; // scratch: nothing is being collected now
        e.clear();
        // The estimates are in units of the local level (see frame_): the
        // saved spectrum sums to one frame's worth, and the level comes back
        // as the local level the next frames are measured against.
        const double total = 1.0;
        level_local_ = in.level_ms / power_to_mean_square_;
        for (int band = 0; band < SpectrumBands::kBands; ++band) {
            const auto j = static_cast<std::size_t>(band);
            if (count[j] > 0)
                continue;
            const auto b = static_cast<std::size_t>(
                std::clamp(static_cast<int>(std::lround(SpectrumBands::centre_hz(band) / bin_hz_)),
                           0, bins_ - 1));
            e.ww[b] += total * in.ww[j];
            e.dd[b] += total * in.dd[j];
            e.re[b] += total * in.re[j];
            e.im[b] += total * in.im[j];
        }
        for (int b = 0; b < bins_; ++b) {
            const auto i = static_cast<std::size_t>(b);
            const auto j =
                static_cast<std::size_t>(SpectrumBands::band_of(static_cast<double>(b) * bin_hz_));
            if (count[j] == 0)
                continue;
            const double share = total / static_cast<double>(count[j]);
            e.ww[i] += share * in.ww[j];
            e.dd[i] += share * in.dd[j];
            e.re[i] += share * in.re[j];
            e.im[i] += share * in.im[j];
        }
        if (!config_.track_dry_leg) {
            e.dd = e.ww;
            e.re = e.ww;
            std::fill(e.im.begin(), e.im.end(), 0.0);
        }
        if (how == Import::prior) {
            const double t = e.total();
            for (int b = 0; b < bins_; ++b) {
                const auto i = static_cast<std::size_t>(b);
                prior_[i] = t > 0.0 ? std::max(0.0, e.ww[i]) / t : 0.0;
            }
            e.clear();
            reset();
            return;
        }
        slow_.copy_from(e);
        fast_.copy_from(e);
        e.clear();
        const double t = slow_.total();
        for (int b = 0; b < bins_; ++b) {
            const auto i = static_cast<std::size_t>(b);
            slow_shape_[i] = fast_shape_[i] = t > 0.0 ? slow_.ww[i] / t : 0.0;
        }
        prior_weight_ = 0.0;
        slow_frames_ = static_cast<std::int64_t>(std::ceil(1.0 / std::max(alpha_, 1e-9)));
        fast_frames_ = fast_equivalent_frames_();
        rearm_shapes_();

        forget_memory_();
        candidate_frames_ = 0;
        candidate_active_ = false;
        observed_frames_ = std::max<std::uint64_t>(observed_frames_, 1);
        recent_count_ = 0;
        drop_run_ = 0;
        restart_grid();
        publish_();
    }

    // ── readings ────────────────────────────────────────────────────────────
    [[nodiscard]] bool prepared() const noexcept {
        return prepared_;
    }
    [[nodiscard]] int fft_size() const noexcept {
        return fft_size_;
    }
    [[nodiscard]] int hop() const noexcept {
        return hop_;
    }
    /// Samples until the next frame boundary of the grid.
    [[nodiscard]] int samples_to_next_frame() const noexcept {
        return hop_ - hop_pos_;
    }
    [[nodiscard]] int bins() const noexcept {
        return bins_;
    }
    [[nodiscard]] double bin_hz() const noexcept {
        return bin_hz_;
    }
    [[nodiscard]] double sample_rate() const noexcept {
        return sample_rate_;
    }
    [[nodiscard]] double hop_seconds() const noexcept {
        return static_cast<double>(hop_) / sample_rate_;
    }
    [[nodiscard]] const LoudnessCompensationConfig& config() const noexcept {
        return config_;
    }
    [[nodiscard]] std::uint64_t observed_frames() const noexcept {
        return observed_frames_;
    }
    /// Frames under the absolute gate (silence).
    [[nodiscard]] std::uint64_t gated_frames() const noexcept {
        return gated_frames_;
    }
    [[nodiscard]] std::uint64_t level_drops() const noexcept {
        return level_drops_;
    }
    [[nodiscard]] std::uint64_t restarts() const noexcept {
        return restarts_;
    }
    /// K-weighted loudness of the last frame's wet leg (LUFS-equivalent).
    [[nodiscard]] double last_frame_lufs() const noexcept {
        return last_frame_lufs_;
    }
    /// The prior's remaining share of the estimate (1 until material is heard).
    [[nodiscard]] double prior_weight() const noexcept {
        return prior_weight_;
    }
    /// The prior's per-bin share (sums to one), as legs.
    [[nodiscard]] LegSpectra prior_legs() const noexcept {
        return {prior_, prior_, prior_, zero_};
    }
    /// How fast the prior fades from the next frame on, as a multiple of
    /// prior_seconds' rate (1: as configured). A caller that sees the
    /// material agree with a RESTORED prior slows the fade, so the few
    /// frames the material's own estimate starts from do not swing it.
    void set_prior_fade_scale(double scale) noexcept {
        prior_fade_scale_ = std::clamp(scale, 0.0, 1.0);
    }
    /// Whether the last frame was audible (above the absolute gate).
    [[nodiscard]] bool last_frame_audible() const noexcept {
        return last_audible_;
    }

    /// The slow (wet) estimate per bin, K-weighted, normalised to sum 1.
    [[nodiscard]] std::span<const double> spectrum() const noexcept {
        return config_.weight_by_material ? std::span<const double>(published_.ww) : flat_;
    }
    /// All the legs' slow estimates on the wet leg's scale.
    [[nodiscard]] LegSpectra legs() const noexcept {
        return legs_of_(published_);
    }
    /// The same of the fast estimate.
    [[nodiscard]] LegSpectra fast_legs() const noexcept {
        return legs_of_(published_fast_);
    }
    /// The change detector's pair: fast and slow averages of each audible
    /// frame's wet SHAPE (level-independent), as coherent single-leg spectra.
    [[nodiscard]] LegSpectra fast_shape_legs() const noexcept {
        if (!config_.weight_by_material)
            return {flat_, flat_, flat_, zero_};
        return {fast_shape_, fast_shape_, fast_shape_, zero_};
    }
    [[nodiscard]] LegSpectra slow_shape_legs() const noexcept {
        if (!config_.weight_by_material)
            return {flat_, flat_, flat_, zero_};
        return {slow_shape_, slow_shape_, slow_shape_, zero_};
    }
    [[nodiscard]] std::int64_t shape_frames() const noexcept {
        return shape_frames_;
    }
    /// The slow wet estimate's energy between two frequencies; the whole weighs 1.
    [[nodiscard]] double weight(double lo_hz, double hi_hz) const noexcept {
        return cdf_.weight(lo_hz, hi_hz);
    }
    [[nodiscard]] const SpectrumCdf& cdf() const noexcept {
        return cdf_;
    }

  private:
    static constexpr int kMaxRecent = 128;

    // The detector's slow shape, re-armed after a restart or a release: it
    // is the new material's from now on, averaging on the slow constant
    // (not a short running mean that would track the fast one for seconds).
    void rearm_shapes_() noexcept {
        shape_frames_ = static_cast<std::int64_t>(std::ceil(1.0 / std::max(alpha_, 1e-9)));
    }
    void forget_memory_() noexcept {
        previous_valid_ = false;
        arrived_valid_ = false;
        alternating_ = false;
        frames_since_previous_ = 0;
    }

    struct Estimate {
        std::vector<double> ww, dd, re, im;
        void assign(std::size_t n) {
            for (auto* v : {&ww, &dd, &re, &im})
                v->assign(n, 0.0);
        }
        void clear() noexcept {
            for (auto* v : {&ww, &dd, &re, &im})
                std::fill(v->begin(), v->end(), 0.0);
        }
        void copy_from(const Estimate& o) noexcept {
            std::copy(o.ww.begin(), o.ww.end(), ww.begin());
            std::copy(o.dd.begin(), o.dd.end(), dd.begin());
            std::copy(o.re.begin(), o.re.end(), re.begin());
            std::copy(o.im.begin(), o.im.end(), im.begin());
        }
        void scale(double k) noexcept {
            for (auto* v : {&ww, &dd, &re, &im})
                for (auto& x : *v)
                    x *= k;
        }
        [[nodiscard]] double total() const noexcept {
            double t = 0.0;
            for (const double x : ww)
                t += x;
            return t;
        }
    };

    [[nodiscard]] LegSpectra legs_of_(const Estimate& e) const noexcept {
        if (!config_.weight_by_material)
            return {flat_, flat_, flat_, zero_};
        if (!config_.track_dry_leg)
            return {e.ww, e.ww, e.ww, zero_};
        return {e.ww, e.dd, e.re, e.im};
    }
    [[nodiscard]] std::int64_t fast_equivalent_frames_() const noexcept {
        return static_cast<std::int64_t>(std::ceil(1.0 / std::max(fast_alpha_, 1e-9)));
    }
    [[nodiscard]] double level_total_() const noexcept {
        return slow_frames_ > 0 ? slow_.total() : 0.0;
    }

    [[nodiscard]] float* ring_(int leg, int ch) noexcept {
        return history_.data() + static_cast<std::size_t>((leg * channels_ + ch) * fft_size_);
    }
    void record_(float* ring, const float* src, int offset, int count) noexcept {
        if (src == nullptr)
            return;
        int pos = write_pos_;
        for (int i = 0; i < count; ++i) {
            const float v = src[offset + i];
            ring[pos] = std::isfinite(v) ? v : 0.0f;
            if (++pos == fft_size_)
                pos = 0;
        }
    }
    void transform_(float* ring, std::vector<std::complex<float>>& out) noexcept {
        // Oldest sample first: write_pos_ is where the next one lands.
        for (int i = 0; i < fft_size_; ++i) {
            int at = write_pos_ + i;
            if (at >= fft_size_)
                at -= fft_size_;
            frame_in_[static_cast<std::size_t>(i)] =
                ring[at] * window_[static_cast<std::size_t>(i)];
        }
        fft_->forward_real(frame_in_.data(), out.data());
    }

    void accumulate_(Estimate& e, double a) noexcept {
        const bool two = config_.track_dry_leg;
        for (int b = 0; b < bins_; ++b) {
            const auto i = static_cast<std::size_t>(b);
            e.ww[i] += a * (frame_ww_[i] - e.ww[i]);
            if (two) {
                e.dd[i] += a * (frame_dd_[i] - e.dd[i]);
                e.re[i] += a * (frame_re_[i] - e.re[i]);
                e.im[i] += a * (frame_im_[i] - e.im[i]);
            }
        }
    }

    void frame_() noexcept {
        const bool two = config_.track_dry_leg;
        for (auto* v : {&frame_ww_, &frame_dd_, &frame_re_, &frame_im_})
            std::fill(v->begin(), v->end(), 0.0);
        for (int ch = 0; ch < channels_; ++ch) {
            transform_(ring_(0, ch), wet_spectrum_);
            if (two)
                transform_(ring_(1, ch), dry_spectrum_);
            for (int b = 0; b < bins_; ++b) {
                const auto i = static_cast<std::size_t>(b);
                const std::complex<double> w(wet_spectrum_[i].real(), wet_spectrum_[i].imag());
                frame_ww_[i] += std::norm(w);
                if (two) {
                    const std::complex<double> d(dry_spectrum_[i].real(), dry_spectrum_[i].imag());
                    frame_dd_[i] += std::norm(d);
                    const auto x = w * std::conj(d);
                    frame_re_[i] += x.real();
                    frame_im_[i] += x.imag();
                }
            }
        }
        double total = 0.0;
        for (int b = 0; b < bins_; ++b) {
            const auto i = static_cast<std::size_t>(b);
            const double kp = k_power_[i];
            frame_ww_[i] *= kp;
            frame_dd_[i] *= kp;
            frame_re_[i] *= kp;
            frame_im_[i] *= kp;
            total += frame_ww_[i];
        }
        const double mean_square = total * power_to_mean_square_;
        last_frame_lufs_ = mean_square > 0.0 ? -0.691 + 10.0 * std::log10(mean_square) : -1.0e9;
        last_audible_ = last_frame_lufs_ >= config_.gate_lufs;
        if (!last_audible_) {
            ++gated_frames_;
            return;
        }
        if (settle_frames_ > 0) {
            --settle_frames_;
            return;
        }
        ++observed_frames_;
        // Energy weighting RELATIVE TO THE LOCAL LEVEL: each frame's power is
        // divided by a short (local_level_seconds) average of frame power
        // before it is averaged. Inside a groove that local level hardly
        // moves, so the kick still outweighs its tail exactly as integrated
        // loudness weighs it; but a swell, a fade or a quieter new part no
        // longer lets the loud past dominate the spectrum for seconds.
        level_local_ =
            level_local_ > 0.0 ? level_local_ + local_alpha_ * (total - level_local_) : total;
        // A frame's weight is (its power / the local level)^exponent: 1 is
        // energy weighting within the local window, 0 every frame equal.
        double norm = level_local_ > 0.0 ? 1.0 / level_local_ : 0.0;
        if (config_.level_weight_exponent != 1.0 && total > 0.0 && level_local_ > 0.0)
            norm = std::pow(total / level_local_, config_.level_weight_exponent) / total;
        for (int b = 0; b < bins_; ++b) {
            const auto i = static_cast<std::size_t>(b);
            frame_ww_[i] *= norm;
            frame_dd_[i] *= norm;
            frame_re_[i] *= norm;
            frame_im_[i] *= norm;
        }
        total *= norm;
        // Both estimates: a running mean until they hold as many frames as
        // their time constant spans, an exponential average after.
        ++slow_frames_;
        ++fast_frames_;
        const double slow_alpha = alternating_ ? alt_alpha_ : alpha_;
        const double a_slow =
            slow_alpha >= 1.0 ? 1.0 : std::max(slow_alpha, 1.0 / static_cast<double>(slow_frames_));
        const double a_fast = fast_alpha_ >= 1.0
                                  ? 1.0
                                  : std::max(fast_alpha_, 1.0 / static_cast<double>(fast_frames_));
        accumulate_(slow_, a_slow);
        accumulate_(fast_, a_fast);
        if (candidate_active_) {
            if (candidate_skip_ > 0) {
                --candidate_skip_;
            } else {
                ++candidate_frames_;
                accumulate_(candidate_, 1.0 / static_cast<double>(candidate_frames_));
            }
        }
        if (previous_valid_ && ++frames_since_previous_ > memory_frames_)
            previous_valid_ = false;
        if (arrived_valid_ && ++frames_since_arrival_ > memory_frames_)
            arrived_valid_ = false;
        // The change detector's pair: the same averages of each frame's
        // SHAPE (every audible frame weighs the same), so a change of
        // material shows within a few frames however much quieter it is.
        {
            ++shape_frames_;
            const double shape_alpha = alternating_ ? alt_alpha_ : alpha_;
            const double sa = shape_alpha >= 1.0
                                  ? 1.0
                                  : std::max(shape_alpha, 1.0 / static_cast<double>(shape_frames_));
            const double fa = fast_alpha_ >= 1.0
                                  ? 1.0
                                  : std::max(fast_alpha_, 1.0 / static_cast<double>(shape_frames_));
            const double inv = total > 0.0 ? 1.0 / total : 0.0;
            for (int b = 0; b < bins_; ++b) {
                const auto i = static_cast<std::size_t>(b);
                const double x = frame_ww_[i] * inv;
                slow_shape_[i] += sa * (x - slow_shape_[i]);
                fast_shape_[i] += fa * (x - fast_shape_[i]);
            }
        }
        double dry_total = total;
        if (two) {
            dry_total = 0.0;
            for (const double x : frame_dd_)
                dry_total += x;
        }
        level_drop_(total, dry_total);
        prior_weight_ = std::max(0.0, prior_weight_ - prior_step_ * prior_fade_scale_);
        publish_();
    }

    // Energy weighting lets loud past material outweigh quieter new material
    // for as long as it lingers. When no frame of the last
    // level_drop_window_seconds has come within level_drop_db of the slow
    // estimate's level, the level genuinely dropped (inside a groove the
    // loud hits keep reaching it), so both estimates are rescaled to the
    // recent level and the new material weighs as much as the old.
    void level_drop_(double wet_total, double dry_total) noexcept {
        const auto slot = static_cast<std::size_t>(recent_pos_);
        recent_[slot] = wet_total;
        recent_dry_[slot] = dry_total;
        recent_pos_ = (recent_pos_ + 1) % kMaxRecent;
        recent_count_ = std::min(recent_count_ + 1, kMaxRecent);
        if (!(config_.level_drop_db > 0.0))
            return;
        const auto max_of = [&](const std::array<double, kMaxRecent>& ring, int frames) {
            double m = 0.0;
            for (int k = 1; k <= frames; ++k)
                m = std::max(
                    m, ring[static_cast<std::size_t>((recent_pos_ - k + kMaxRecent) % kMaxRecent)]);
            return m;
        };
        const double floor = std::pow(10.0, -config_.level_drop_db / 10.0);
        double rw = 1.0, rd = 1.0, wet_max = 0.0, dry_max = 0.0;
        if (recent_count_ >= level_drop_frames_) {
            wet_max = max_of(recent_, level_drop_frames_);
            const double wet_level = sum_(slow_.ww);
            if (wet_level > 0.0 && wet_max < wet_level * floor)
                rw = wet_max / wet_level;
        }
        // The live (dry) leg has no change detector of its own -- it is the
        // dry half of a Mix, or the live input under a hold -- so its level
        // follows over the short window.
        if (config_.track_dry_leg && recent_count_ >= dry_drop_frames_) {
            dry_max = max_of(recent_dry_, dry_drop_frames_);
            const double dry_level = sum_(slow_.dd);
            if (dry_level > 0.0 && dry_max < dry_level * floor)
                rd = dry_max / dry_level;
        }
        if (!config_.track_dry_leg)
            rd = rw;
        drop_run_ = rw < 1.0 ? drop_run_ + 1 : 0;
        dry_drop_run_ = rd < 1.0 ? dry_drop_run_ + 1 : 0;
        if (drop_run_ < 2)
            rw = 1.0;
        if (dry_drop_run_ < 2)
            rd = 1.0;
        if (rw >= 1.0 && rd >= 1.0)
            return;
        // Level only: the spectrum is the change detector's business.
        rescale_legs_(slow_, rw, rd);
        const double fw = sum_(fast_.ww);
        const double fd = config_.track_dry_leg ? sum_(fast_.dd) : fw;
        rescale_legs_(fast_, rw < 1.0 && fw > wet_max && wet_max > 0.0 ? wet_max / fw : 1.0,
                      rd < 1.0 && fd > dry_max && dry_max > 0.0 ? dry_max / fd : 1.0);
        if (previous_valid_)
            rescale_legs_(previous_, rw, rd);
        if (rw < 1.0)
            drop_run_ = 0;
        if (rd < 1.0)
            dry_drop_run_ = 0;
        recent_count_ = 0;
        ++level_drops_;
    }

    // Scale the wet leg's power by rw and the dry leg's by rd; their
    // cross-spectrum scales by the geometric mean.
    void rescale_legs_(Estimate& e, double rw, double rd) noexcept {
        const double rx = std::sqrt(rw * rd);
        for (auto& x : e.ww)
            x *= rw;
        if (!config_.track_dry_leg)
            return;
        for (auto& x : e.dd)
            x *= rd;
        for (auto& x : e.re)
            x *= rx;
        for (auto& x : e.im)
            x *= rx;
    }
    [[nodiscard]] static double sum_(const std::vector<double>& v) noexcept {
        double t = 0.0;
        for (const double x : v)
            t += x;
        return t;
    }

    // The published estimates: the prior's decaying share plus the material's
    // averages, each on its own wet leg's scale (its total is 1 - prior share).
    void blend_into_(const Estimate& avg, std::int64_t frames, Estimate& out) noexcept {
        // A restart cleared the averages: keep publishing the last estimate
        // until new material arrives (the prior is for a cold start only).
        if (frames == 0 && observed_frames_ > 0)
            return;
        const double total = frames > 0 ? avg.total() : 0.0;
        const double pw = frames > 0 && total > 0.0 ? prior_weight_ : 1.0;
        const double scale = total > 0.0 ? (1.0 - pw) / total : 0.0;
        const bool two = config_.track_dry_leg;
        for (int b = 0; b < bins_; ++b) {
            const auto i = static_cast<std::size_t>(b);
            const double p = pw * prior_[i];
            out.ww[i] = p + scale * avg.ww[i];
            if (two) {
                out.dd[i] = p + scale * avg.dd[i];
                out.re[i] = p + scale * avg.re[i];
                out.im[i] = scale * avg.im[i];
            }
        }
    }
    void publish_() noexcept {
        if (published_.ww.size() != static_cast<std::size_t>(bins_)) {
            // prepare() sizes these; first publish after it.
            return;
        }
        blend_into_(slow_, slow_frames_, published_);
        blend_into_(fast_, fast_frames_, published_fast_);
        cdf_.assign(spectrum(), bin_hz_);
    }

    // Sizes the published estimates (prepare-time; part of prepare()).
    void size_published_() {
        published_.assign(static_cast<std::size_t>(bins_));
        published_fast_.assign(static_cast<std::size_t>(bins_));
    }

    double sample_rate_ = 48000.0;
    int channels_ = 1;
    LoudnessCompensationConfig config_{};
    int fft_size_ = 4096;
    int hop_ = 2048;
    int bins_ = 2049;
    double bin_hz_ = 48000.0 / 4096.0;
    double power_to_mean_square_ = 0.0;
    double alpha_ = 0.0;
    double fast_alpha_ = 0.0;
    double prior_step_ = 1.0;
    double prior_fade_scale_ = 1.0;
    double prior_weight_ = 1.0;
    double level_local_ = 0.0;
    double local_alpha_ = 1.0;
    bool prepared_ = false;
    std::unique_ptr<pulp::signal::FftT<float>> fft_;
    std::vector<float> window_;
    std::vector<float> history_;
    std::vector<float> frame_in_;
    std::vector<std::complex<float>> wet_spectrum_;
    std::vector<std::complex<float>> dry_spectrum_;
    std::vector<double> frame_ww_, frame_dd_, frame_re_, frame_im_;
    Estimate slow_, fast_, published_, published_fast_;
    std::vector<double> k_power_;
    std::vector<double> prior_;
    std::vector<double> flat_;
    std::vector<double> zero_;
    SpectrumCdf cdf_;
    std::int64_t slow_frames_ = 0, fast_frames_ = 0, shape_frames_ = 0;
    Estimate candidate_, previous_;
    std::int64_t candidate_frames_ = 0;
    int candidate_skip_ = 0;
    bool candidate_active_ = false;
    std::vector<double> previous_shape_, alt_a_shape_, alt_b_shape_, arrived_shape_;
    bool arrived_valid_ = false;
    std::int64_t frames_since_arrival_ = 0;
    bool previous_valid_ = false;
    bool alternating_ = false;
    std::int64_t frames_since_previous_ = 0, memory_frames_ = 0;
    std::uint64_t merges_ = 0;
    double alt_alpha_ = 0.0;
    double geometry_rate_ = 0.0;
    int geometry_channels_ = 0;
    std::vector<double> fast_shape_, slow_shape_;
    std::uint64_t observed_frames_ = 0;
    std::uint64_t gated_frames_ = 0;
    std::uint64_t level_drops_ = 0;
    std::uint64_t restarts_ = 0;
    std::array<double, kMaxRecent> recent_{}, recent_dry_{};
    int recent_pos_ = 0, recent_count_ = 0, level_drop_frames_ = 9, drop_run_ = 0;
    int dry_drop_frames_ = 9, dry_drop_run_ = 0;
    int settle_frames_ = 0;
    double last_frame_lufs_ = -1.0e9;
    bool last_audible_ = false;
    int write_pos_ = 0;
    int filled_ = 0;
    int hop_pos_ = 0;
};

// ── 5. The target ───────────────────────────────────────────────────────────

/// What the make-up gain is heading for. A shape edit (or a switch-on) moves
/// it at once -- the user asked for it. The material moves it with a slew
/// that is proportional to how far it has to go: at least
/// `min_slew_db_per_second` (so a steady groove's frame-to-frame wobble is
/// smoothed away), `rate_per_second` x the distance when that is faster
/// beyond a knee (exponential approach to a far target), at most
/// `max_slew_db_per_second`.
/// The caller ramps the applied gain toward it.
class MakeupTarget {
  public:
    void reset() noexcept {
        primed_ = false;
        value_db_ = 0.0f;
    }
    [[nodiscard]] bool primed() const noexcept {
        return primed_;
    }
    [[nodiscard]] float value_db() const noexcept {
        return value_db_;
    }

    /// Jump to @p db. Returns whether the target changed.
    bool retarget(float db) noexcept {
        const bool moved = !primed_ || db != value_db_;
        value_db_ = db;
        primed_ = true;
        return moved;
    }

    /// Move toward @p wanted_db by at most slew x @p seconds (no limit when
    /// the slew is <= 0). Returns whether the target changed.
    bool follow(float wanted_db, double seconds, double slew_db_per_second) noexcept {
        return follow(wanted_db, seconds, slew_db_per_second, 0.0, 0.0);
    }

    /// The proportional form described above; the speed-up counts only the
    /// distance beyond @p knee_db.
    bool follow(float wanted_db, double seconds, double min_slew_db_per_second,
                double rate_per_second, double max_slew_db_per_second,
                double knee_db = 0.0) noexcept {
        if (!primed_)
            return retarget(wanted_db);
        float next = wanted_db;
        if (min_slew_db_per_second > 0.0) {
            const double distance = std::abs(static_cast<double>(wanted_db) - value_db_);
            double slew = std::max(min_slew_db_per_second,
                                   rate_per_second * std::max(0.0, distance - knee_db));
            if (max_slew_db_per_second > 0.0)
                slew = std::min(slew, max_slew_db_per_second);
            slew = std::max(slew, min_slew_db_per_second);
            const auto step = static_cast<float>(slew * std::max(0.0, seconds));
            next = value_db_ + std::clamp(wanted_db - value_db_, -step, step);
        }
        if (next == value_db_)
            return false;
        value_db_ = next;
        return true;
    }

  private:
    bool primed_ = false;
    float value_db_ = 0.0f;
};

// SPECTR-RENDER-PATH END

} // namespace pulp::signal
