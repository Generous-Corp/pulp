#pragma once

#include <pulp/signal/fft.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

namespace pulp::signal {

enum class OnsetDetectionMethod : std::uint8_t {
    EnergyFlux,
    SpectralFlux,
    HighFrequencyContent,
    /// Log-magnitude spectral flux on a logarithmic filterbank, comparing each
    /// frame with a frequency-max-filtered earlier frame so that vibrato,
    /// which moves partials sideways by a band or so, is not counted as new
    /// energy. Böck and Widmer, "Maximum Filter Vibrato Suppression for Onset
    /// Detection", DAFx 2013.
    SuperFlux,
};

/// Settings of the SuperFlux novelty function (OnsetDetectionMethod::SuperFlux).
/// The defaults follow the published configuration: 24 bands per octave from
/// 27.5 Hz to 16 kHz, a maximum filter three bands wide, and a lag chosen from
/// the window and hop.
struct SuperFluxConfig {
    /// Filterbank bands per octave, 1 to 48. Bands narrower than one FFT bin
    /// are merged, so low frequencies use fewer than this.
    double bands_per_octave = 24.0;
    double min_hz = 27.5;
    double max_hz = 16000.0;
    /// `lambda` in `log(lambda * |X| + 1)`, with `|X|` calibrated so a full-scale
    /// sine at a bin centre reads 1. Larger values compress quiet detail less
    /// aggressively; the published value 1 applies to a differently scaled
    /// spectrogram.
    double compression = 1000.0;
    /// Width of the maximum filter across bands (odd, 1 to 15). 1 disables it.
    std::uint32_t max_filter_width = 3;
    /// Frames between the compared spectra, 1 to 8. 0 derives it from the
    /// window and hop: the earlier frame is the first whose Hann window has
    /// moved past the point where it reaches half its peak, which is a quarter
    /// of the window (rounded, at least 1).
    std::uint32_t lag_frames = 0;
};

/// Pure novelty transitions shared by fixed-capacity streaming analysis and
/// background/offline adapters. The first window is represented by callers as
/// zero novelty because it has no predecessor.
inline double onset_energy_flux(double previous, double current) noexcept {
    if (!std::isfinite(previous) || !std::isfinite(current))
        return std::numeric_limits<double>::quiet_NaN();
    return std::max(0.0, current - previous);
}

inline double onset_spectral_bin_flux(OnsetDetectionMethod method, double previous, double current,
                                      std::size_t bin) noexcept {
    if (!std::isfinite(previous) || !std::isfinite(current))
        return std::numeric_limits<double>::quiet_NaN();
    const auto positive = std::max(0.0, current - previous);
    return method == OnsetDetectionMethod::HighFrequencyContent
               ? positive * static_cast<double>(bin + 1u)
               : positive;
}

struct StreamingAnalysisConfig {
    double sample_rate = 48000.0;
    std::size_t channels = 1;
    std::uint32_t fft_size = 2048;
    std::uint64_t hop_size = 2048;
    std::uint64_t max_retained_fft_bytes = 64u * 1024u * 1024u;
};

namespace detail {

enum class AnalysisWindow : std::uint8_t { rectangular, symmetric_hann };

template <typename SampleType, std::size_t MaxFftSize, std::size_t MaxChannels>
class StreamingAnalysisWindowT {
    static_assert(std::is_floating_point_v<SampleType>);
    static_assert(MaxFftSize >= 2);
    static_assert(MaxChannels > 0);

  public:
    [[nodiscard]] bool prepare(const StreamingAnalysisConfig& config, AnalysisWindow window,
                               bool require_fft = true) {
        if (!valid_config(config, require_fft))
            return false;

        std::uint64_t retained_fft_bytes = 0;
        std::optional<FftT<SampleType>> fft;
        if (require_fft) {
            if (!checked_fft_retained_bytes<SampleType>(
                    config.fft_size, config.max_retained_fft_bytes, retained_fft_bytes))
                return false;
            fft.emplace(static_cast<int>(config.fft_size));
            if (!fft->ready())
                return false;
        }
        std::array<double, MaxFftSize> coefficients{};
        for (std::size_t i = 0; i < config.fft_size; ++i) {
            if (window == AnalysisWindow::symmetric_hann) {
                constexpr double two_pi = 6.28318530717958647692;
                const auto phase =
                    two_pi * static_cast<double>(i) / static_cast<double>(config.fft_size - 1u);
                coefficients[i] = 0.5 - 0.5 * std::cos(phase);
            } else {
                coefficients[i] = 1.0;
            }
        }

        config_ = config;
        retained_fft_bytes_ = retained_fft_bytes;
        fft_ = std::move(fft);
        window_ = coefficients;
        prepared_ = true;
        reset();
        return true;
    }

    void reset() noexcept {
        mono_ring_.fill(SampleType{0});
        power_ring_.fill(0.0);
        time_frame_.fill(SampleType{0});
        spectrum_.fill({});
        write_ = 0;
        filled_ = 0;
        until_next_frame_ = 0;
        stream_frames_ = 0;
    }

    void clear_history_after_fault() noexcept {
        mono_ring_.fill(SampleType{0});
        power_ring_.fill(0.0);
        time_frame_.fill(SampleType{0});
        spectrum_.fill({});
        write_ = 0;
        filled_ = 0;
        until_next_frame_ = 0;
    }

    // A rejected input sample still occupies one position on the absolute
    // input timeline. It cannot contribute to a window, so restart readiness
    // from the next finite sample while preserving timestamp monotonicity.
    void reject_sample() noexcept {
        ++stream_frames_;
        clear_history_after_fault();
    }

    [[nodiscard]] bool push(double mono, double channel_power) noexcept {
        mono_ring_[write_] = mono;
        power_ring_[write_] = channel_power;
        write_ = (write_ + 1u) % config_.fft_size;
        ++stream_frames_;

        if (filled_ < config_.fft_size) {
            ++filled_;
            if (filled_ == config_.fft_size) {
                until_next_frame_ = config_.hop_size;
                return true;
            }
            return false;
        }

        if (--until_next_frame_ == 0) {
            until_next_frame_ = config_.hop_size;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool render_spectrum() noexcept {
        return render_spectrum(false);
    }

    [[nodiscard]] bool render_gain_normalized_spectrum() noexcept {
        return render_spectrum(true);
    }

    [[nodiscard]] double frame_energy() const noexcept {
        long double sum = 0.0L;
        const auto divisor = static_cast<long double>(config_.fft_size);
        for (std::size_t i = 0; i < config_.fft_size; ++i) {
            const auto source = (write_ + i) % config_.fft_size;
            // Divide each admitted finite term before accumulation. This
            // remains bounded when long double has only double precision.
            sum += static_cast<long double>(power_ring_[source]) / divisor;
        }
        return static_cast<double>(sum);
    }

    /// Latest rendered spectrum. Bins DC through Nyquist (fft_size / 2 + 1)
    /// are written; the slots above Nyquist are not.
    [[nodiscard]] const std::array<std::complex<SampleType>, MaxFftSize>&
    spectrum() const noexcept {
        return spectrum_;
    }
    [[nodiscard]] const StreamingAnalysisConfig& config() const noexcept {
        return config_;
    }
    [[nodiscard]] std::uint64_t stream_frames() const noexcept {
        return stream_frames_;
    }
    [[nodiscard]] bool prepared() const noexcept {
        return prepared_;
    }
    [[nodiscard]] std::uint64_t retained_fft_bytes() const noexcept {
        return retained_fft_bytes_;
    }

  private:
    [[nodiscard]] bool render_spectrum(bool normalize_gain) noexcept {
        if (!fft_)
            return false;
        double maximum = 0.0;
        if (normalize_gain) {
            for (std::size_t i = 0; i < config_.fft_size; ++i) {
                const auto source = (write_ + i) % config_.fft_size;
                const auto windowed = mono_ring_[source] * window_[i];
                if (!std::isfinite(windowed))
                    return false;
                maximum = std::max(maximum, std::abs(windowed));
            }
        }
        for (std::size_t i = 0; i < config_.fft_size; ++i) {
            const auto source = (write_ + i) % config_.fft_size;
            const auto windowed = mono_ring_[source] * window_[i];
            time_frame_[i] = static_cast<SampleType>(maximum > 0.0 ? windowed / maximum : windowed);
            if (!std::isfinite(time_frame_[i]))
                return false;
        }
        fft_->forward_real_half(time_frame_.data(), spectrum_.data());
        for (std::size_t bin = 0; bin <= config_.fft_size / 2u; ++bin) {
            if (!std::isfinite(spectrum_[bin].real()) || !std::isfinite(spectrum_[bin].imag()))
                return false;
        }
        return true;
    }
    static bool valid_config(const StreamingAnalysisConfig& config, bool require_fft) noexcept {
        return std::isfinite(config.sample_rate) && config.sample_rate > 0.0 &&
               config.channels > 0 && config.channels <= MaxChannels && config.fft_size >= 2 &&
               config.fft_size <= MaxFftSize &&
               (!require_fft || (config.fft_size & (config.fft_size - 1u)) == 0) &&
               config.hop_size > 0 && (!require_fft || config.max_retained_fft_bytes > 0);
    }

    StreamingAnalysisConfig config_{};
    std::optional<FftT<SampleType>> fft_{};
    std::array<double, MaxFftSize> mono_ring_{};
    std::array<double, MaxFftSize> power_ring_{};
    std::array<double, MaxFftSize> window_{};
    std::array<SampleType, MaxFftSize> time_frame_{};
    std::array<std::complex<SampleType>, MaxFftSize> spectrum_{};
    std::size_t write_ = 0;
    std::size_t filled_ = 0;
    std::uint64_t until_next_frame_ = 0;
    std::uint64_t stream_frames_ = 0;
    std::uint64_t retained_fft_bytes_ = 0;
    bool prepared_ = false;
};

template <typename SampleType, std::size_t MaxChannels>
bool mix_planar_frame(const SampleType* const* channels, std::size_t channel_count,
                      std::size_t frame, bool compute_power, double& mono,
                      double& channel_power) noexcept {
    if (channels == nullptr || channel_count == 0 || channel_count > MaxChannels)
        return false;
    double scale = 0.0;
    for (std::size_t channel = 0; channel < channel_count; ++channel) {
        if (channels[channel] == nullptr)
            return false;
        const auto sample = static_cast<double>(channels[channel][frame]);
        if (!std::isfinite(sample))
            return false;
        scale = std::max(scale, std::abs(sample));
    }

    if (scale == 0.0) {
        mono = 0.0;
        channel_power = 0.0;
        return true;
    }

    double scaled_sum = 0.0;
    double scaled_power = 0.0;
    for (std::size_t channel = 0; channel < channel_count; ++channel) {
        const auto scaled = static_cast<double>(channels[channel][frame]) / scale;
        scaled_sum += scaled;
        if (compute_power)
            scaled_power += scaled * scaled;
    }
    const auto divisor = static_cast<double>(channel_count);
    const auto mixed = scale * (scaled_sum / divisor);
    if (!std::isfinite(mixed) ||
        std::abs(mixed) > static_cast<double>(std::numeric_limits<SampleType>::max()))
        return false;
    mono = mixed;
    channel_power = 0.0;
    if (compute_power) {
        if (scale > std::sqrt(std::numeric_limits<double>::max()))
            return false;
        channel_power = (scale * scale) * (scaled_power / divisor);
        if (!std::isfinite(channel_power))
            return false;
    }
    return true;
}

inline int pitch_class_for_frequency(double frequency_hz) noexcept {
    if (!(frequency_hz > 0.0) || !std::isfinite(frequency_hz))
        return -1;
    const auto midi = 69.0 + 12.0 * std::log2(frequency_hz / 440.0);
    if (!std::isfinite(midi))
        return -1;
    auto pitch_class = static_cast<int>(std::lround(midi)) % 12;
    if (pitch_class < 0)
        pitch_class += 12;
    return pitch_class;
}

/// SuperFlux novelty (Böck and Widmer, DAFx 2013): the magnitude spectrum is
/// reduced to a logarithmic filterbank of triangular bands, compressed with
/// `log(lambda * x + 1)`, and each frame is compared with the frame `lag`
/// earlier after that earlier frame is max-filtered across bands. The novelty
/// is the sum of the positive differences. Because the reference is the local
/// maximum, a partial that has merely drifted to a neighbouring band (vibrato)
/// is not counted as an increase, while a genuine onset, which raises energy
/// in many bands at once, is.
///
/// Fixed capacity: at most kMaxBands bands and kMaxLag stored frames.
template <typename SampleType, std::size_t MaxFftSize> class SuperFluxNoveltyT {
  public:
    static constexpr std::size_t kMaxBands = 256;
    static constexpr std::size_t kMaxLag = 8;

    [[nodiscard]] bool prepare(const StreamingAnalysisConfig& config,
                               const SuperFluxConfig& params) noexcept {
        if (!(params.bands_per_octave >= 1.0 && params.bands_per_octave <= 48.0) ||
            !std::isfinite(params.min_hz) || !std::isfinite(params.max_hz) ||
            !(params.min_hz > 0.0) || !(params.max_hz > params.min_hz) ||
            !(params.compression > 0.0) || !std::isfinite(params.compression) ||
            params.max_filter_width == 0 || params.max_filter_width > 15 ||
            params.max_filter_width % 2u == 0 || params.lag_frames > kMaxLag ||
            config.fft_size > MaxFftSize || config.fft_size < 8)
            return false;

        const double fft = static_cast<double>(config.fft_size);
        const double nyquist_bin = fft / 2.0;
        const double high_hz = std::min(params.max_hz, 0.5 * config.sample_rate);
        // Centre frequencies 440 * 2^(i / bands_per_octave), snapped to FFT bins;
        // bands that land on a bin already used are dropped.
        const int first = static_cast<int>(
            std::ceil(params.bands_per_octave * std::log2(params.min_hz / 440.0) - 1e-9));
        count_ = 0;
        for (int i = first; count_ < kMaxBands; ++i) {
            const double hz = 440.0 * std::exp2(static_cast<double>(i) / params.bands_per_octave);
            if (hz > high_hz)
                break;
            const double bin = std::round(hz * fft / config.sample_rate);
            if (bin < 1.0)
                continue;
            if (bin > nyquist_bin - 1.0)
                break;
            const auto b = static_cast<std::uint32_t>(bin);
            if (count_ == 0 || b > center_[count_ - 1])
                center_[count_++] = b;
        }
        if (count_ < 2)
            return false;
        for (std::size_t i = 0; i < count_; ++i) {
            left_[i] = i == 0 ? center_[0] - 1u : center_[i - 1];
            right_[i] = i + 1 == count_
                            ? std::min(center_[i] + 1u, static_cast<std::uint32_t>(nyquist_bin))
                            : center_[i + 1];
        }

        double window_sum = 0.0;
        for (std::size_t i = 0; i < config.fft_size; ++i)
            window_sum += 0.5 - 0.5 * std::cos(6.28318530717958647692 * static_cast<double>(i) /
                                               static_cast<double>(config.fft_size - 1u));
        amplitude_scale_ = 2.0 / window_sum;
        compression_ = params.compression;
        half_width_ = params.max_filter_width / 2u;
        if (params.lag_frames != 0) {
            lag_ = params.lag_frames;
        } else {
            const double derived = std::round(0.25 * fft / static_cast<double>(config.hop_size));
            lag_ = static_cast<std::size_t>(std::clamp(derived, 1.0, static_cast<double>(kMaxLag)));
        }
        reset();
        return true;
    }

    void reset() noexcept {
        stored_ = 0;
        head_ = 0;
    }

    [[nodiscard]] std::size_t band_count() const noexcept {
        return count_;
    }
    [[nodiscard]] std::size_t lag_frames() const noexcept {
        return lag_;
    }

    /// Consumes the half spectrum (bins 0 .. fft_size / 2) of one frame and
    /// returns its novelty: 0 until `lag` earlier frames exist, NaN if the
    /// spectrum is not finite.
    [[nodiscard]] double process(const std::complex<SampleType>* spectrum) noexcept {
        auto& current = history_[head_];
        for (std::size_t i = 0; i < count_; ++i) {
            double weighted = 0.0;
            double weights = 0.0;
            const auto l = left_[i], c = center_[i], r = right_[i];
            for (std::uint32_t b = l + 1u; b < r; ++b) {
                const double w = b <= c ? static_cast<double>(b - l) / static_cast<double>(c - l)
                                        : static_cast<double>(r - b) / static_cast<double>(r - c);
                weighted += w * static_cast<double>(std::abs(spectrum[b]));
                weights += w;
            }
            const double magnitude = amplitude_scale_ * weighted / weights;
            if (!std::isfinite(magnitude))
                return std::numeric_limits<double>::quiet_NaN();
            current[i] = std::log1p(compression_ * magnitude);
        }

        double novelty = 0.0;
        if (stored_ >= lag_) {
            const auto& reference = history_[(head_ + kMaxLag - lag_) % kMaxLag];
            for (std::size_t i = 0; i < count_; ++i) {
                const std::size_t lo = i >= half_width_ ? i - half_width_ : 0;
                const std::size_t hi = std::min(count_ - 1, i + half_width_);
                double reference_max = reference[lo];
                for (std::size_t j = lo + 1; j <= hi; ++j)
                    reference_max = std::max(reference_max, reference[j]);
                novelty += std::max(0.0, current[i] - reference_max);
            }
        }
        head_ = (head_ + 1) % kMaxLag;
        if (stored_ < kMaxLag)
            ++stored_;
        return novelty;
    }

  private:
    std::array<std::array<double, kMaxBands>, kMaxLag> history_{};
    std::array<std::uint32_t, kMaxBands> left_{};
    std::array<std::uint32_t, kMaxBands> center_{};
    std::array<std::uint32_t, kMaxBands> right_{};
    std::size_t count_ = 0;
    std::size_t lag_ = 1;
    std::size_t half_width_ = 1;
    std::size_t stored_ = 0;
    std::size_t head_ = 0;
    double amplitude_scale_ = 1.0;
    double compression_ = 1000.0;
};

} // namespace detail

/// One complete chroma analysis window. `magnitude` preserves the raw peak-bin
/// accumulation used by the built-in key analyzer; `normalized` is its
/// per-window unit-sum form. No padded tail frames are emitted.
template <typename SampleType> struct ChromaFrameT {
    std::array<double, 12> magnitude{};
    std::array<SampleType, 12> normalized{};
    std::uint64_t window_start_frame = 0;
    std::uint64_t window_center_frame = 0;
    std::uint64_t ready_at_frame = 0;
    std::uint64_t sequence = 0;
    bool valid = false;
};

/// Fixed-capacity chroma analysis over caller-owned planar input.
///
/// prepare() is control-thread work and may allocate only inside FftT. After a
/// successful prepare, reset() and process() allocate nothing. The instance and
/// its nothrow sink have one processing-thread owner; cross-thread publication
/// of copied frames is the caller's responsibility. Only complete windows are
/// emitted at starts 0, hop, 2*hop... for overlapping, contiguous, or gapped
/// cadence.
template <typename SampleType = float, std::size_t MaxFftSize = 4096, std::size_t MaxChannels = 8>
class ChromaFrontEndT {
    static_assert(std::is_floating_point_v<SampleType>);
    static_assert(MaxFftSize >= 2);
    static_assert(MaxChannels > 0);

  public:
    using Frame = ChromaFrameT<SampleType>;

    [[nodiscard]] bool prepare(const StreamingAnalysisConfig& config) {
        if (!window_.prepare(config, detail::AnalysisWindow::symmetric_hann))
            return false;
        sequence_ = 0;
        current_ = {};
        return true;
    }

    void reset() noexcept {
        window_.reset();
        sequence_ = 0;
        current_ = {};
    }

    template <typename Sink>
    [[nodiscard]] bool process(const SampleType* const* channels, std::size_t channel_count,
                               std::size_t frames, Sink&& sink) noexcept {
        static_assert(std::is_nothrow_invocable_v<Sink&, const Frame&>,
                      "analysis sinks must be noexcept");
        if (!window_.prepared() || channel_count != window_.config().channels ||
            channels == nullptr)
            return false;
        for (std::size_t channel = 0; channel < channel_count; ++channel) {
            if (channels[channel] == nullptr)
                return false;
        }

        bool finite = true;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            double mono{};
            double power{};
            if (!detail::mix_planar_frame<SampleType, MaxChannels>(channels, channel_count, frame,
                                                                   false, mono, power)) {
                window_.reject_sample();
                finite = false;
                continue;
            }
            if (!window_.push(mono, power))
                continue;
            if (!render_frame()) {
                window_.clear_history_after_fault();
                finite = false;
                continue;
            }
            sink(current_);
        }
        return finite;
    }

    [[nodiscard]] const Frame& current() const noexcept {
        return current_;
    }
    [[nodiscard]] std::uint32_t algorithmic_latency_samples() const noexcept {
        return window_.prepared() ? window_.config().fft_size / 2u : 0u;
    }
    [[nodiscard]] std::uint32_t startup_samples() const noexcept {
        return window_.prepared() ? window_.config().fft_size : 0u;
    }
    [[nodiscard]] std::uint64_t retained_fft_bytes() const noexcept {
        return window_.retained_fft_bytes();
    }

  private:
    [[nodiscard]] bool render_frame() noexcept {
        if (!window_.render_spectrum())
            return false;
        Frame next{};
        const auto& config = window_.config();
        const auto& spectrum = window_.spectrum();
        for (std::size_t bin = 1; bin < config.fft_size / 2u; ++bin) {
            const auto magnitude = static_cast<double>(std::abs(spectrum[bin]));
            const auto left = static_cast<double>(std::abs(spectrum[bin - 1u]));
            const auto right = static_cast<double>(std::abs(spectrum[bin + 1u]));
            if (!std::isfinite(magnitude) || !std::isfinite(left) || !std::isfinite(right))
                return false;
            if (magnitude < left || magnitude < right)
                continue;
            const auto normalized_bin =
                static_cast<double>(bin) / static_cast<double>(config.fft_size);
            const auto frequency = config.sample_rate * normalized_bin;
            if (frequency < 55.0 || frequency > 5000.0)
                continue;
            const auto pitch_class = detail::pitch_class_for_frequency(frequency);
            if (pitch_class >= 0)
                next.magnitude[static_cast<std::size_t>(pitch_class)] += magnitude;
        }
        long double total = 0.0L;
        for (const auto value : next.magnitude)
            total += static_cast<long double>(value);
        if (!std::isfinite(total))
            return false;
        if (total > 0.0L) {
            for (std::size_t pc = 0; pc < 12; ++pc)
                next.normalized[pc] =
                    static_cast<SampleType>(static_cast<long double>(next.magnitude[pc]) / total);
            next.valid = true;
        }
        next.ready_at_frame = window_.stream_frames() - 1u;
        next.window_start_frame = window_.stream_frames() - config.fft_size;
        next.window_center_frame = next.window_start_frame + config.fft_size / 2u;
        next.sequence = ++sequence_;
        current_ = next;
        return true;
    }

    detail::StreamingAnalysisWindowT<SampleType, MaxFftSize, MaxChannels> window_{};
    Frame current_{};
    std::uint64_t sequence_ = 0;
};

/// One complete novelty window. This deliberately carries no marker or
/// confidence: the offline detector's confidence depends on the global maximum
/// and its adaptive threshold looks both backward and forward in the material.
template <typename SampleType> struct OnsetNoveltyFrameT {
    double novelty = 0.0;
    std::uint64_t window_start_frame = 0;
    std::uint64_t window_center_frame = 0;
    std::uint64_t ready_at_frame = 0;
    std::uint64_t sequence = 0;
    OnsetDetectionMethod method = OnsetDetectionMethod::EnergyFlux;
};

/// Fixed-capacity streaming novelty analysis. This does not publish onset
/// markers or confidence because those require a global offline postpass.
/// Ownership, preparation, cadence, and callback rules match ChromaFrontEndT.
template <typename SampleType = float, std::size_t MaxFftSize = 4096, std::size_t MaxChannels = 8>
class OnsetNoveltyFrontEndT {
    static_assert(std::is_floating_point_v<SampleType>);
    static_assert(MaxFftSize >= 2);
    static_assert(MaxChannels > 0);

  public:
    using Frame = OnsetNoveltyFrameT<SampleType>;

    /// `superflux` is read only for OnsetDetectionMethod::SuperFlux, which also
    /// switches the analysis window from rectangular to Hann.
    [[nodiscard]] bool prepare(const StreamingAnalysisConfig& config, OnsetDetectionMethod method,
                               const SuperFluxConfig& superflux = {}) {
        if (method != OnsetDetectionMethod::EnergyFlux &&
            method != OnsetDetectionMethod::SpectralFlux &&
            method != OnsetDetectionMethod::HighFrequencyContent &&
            method != OnsetDetectionMethod::SuperFlux)
            return false;
        const auto require_fft = method != OnsetDetectionMethod::EnergyFlux;
        const auto window = method == OnsetDetectionMethod::SuperFlux
                                ? detail::AnalysisWindow::symmetric_hann
                                : detail::AnalysisWindow::rectangular;
        if (!window_.prepare(config, window, require_fft))
            return false;
        if (method == OnsetDetectionMethod::SuperFlux &&
            !superflux_.prepare(window_.config(), superflux))
            return false;
        method_ = method;
        previous_magnitudes_.fill(0.0);
        previous_energy_ = 0.0;
        have_previous_ = false;
        sequence_ = 0;
        current_ = {};
        return true;
    }

    void reset() noexcept {
        window_.reset();
        superflux_.reset();
        previous_magnitudes_.fill(0.0);
        previous_energy_ = 0.0;
        have_previous_ = false;
        sequence_ = 0;
        current_ = {};
    }

    template <typename Sink>
    [[nodiscard]] bool process(const SampleType* const* channels, std::size_t channel_count,
                               std::size_t frames, Sink&& sink) noexcept {
        static_assert(std::is_nothrow_invocable_v<Sink&, const Frame&>,
                      "analysis sinks must be noexcept");
        if (!window_.prepared() || channel_count != window_.config().channels ||
            channels == nullptr)
            return false;
        for (std::size_t channel = 0; channel < channel_count; ++channel) {
            if (channels[channel] == nullptr)
                return false;
        }

        bool finite = true;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            double mono{};
            double power{};
            if (!detail::mix_planar_frame<SampleType, MaxChannels>(
                    channels, channel_count, frame, method_ == OnsetDetectionMethod::EnergyFlux,
                    mono, power)) {
                reject_sample();
                finite = false;
                continue;
            }
            if (!window_.push(mono, power))
                continue;
            if (!render_frame()) {
                clear_history_after_fault();
                finite = false;
                continue;
            }
            sink(current_);
        }
        return finite;
    }

    [[nodiscard]] const Frame& current() const noexcept {
        return current_;
    }
    [[nodiscard]] std::uint32_t readiness_latency_samples() const noexcept {
        return window_.prepared() ? window_.config().fft_size - 1u : 0u;
    }
    [[nodiscard]] std::uint64_t retained_fft_bytes() const noexcept {
        return window_.retained_fft_bytes();
    }

  private:
    void clear_history_after_fault() noexcept {
        window_.clear_history_after_fault();
        superflux_.reset();
        previous_magnitudes_.fill(0.0);
        previous_energy_ = 0.0;
        have_previous_ = false;
    }

    void reject_sample() noexcept {
        window_.reject_sample();
        previous_magnitudes_.fill(0.0);
        previous_energy_ = 0.0;
        have_previous_ = false;
    }

    [[nodiscard]] bool render_frame() noexcept {
        double novelty = 0.0;
        if (method_ == OnsetDetectionMethod::EnergyFlux) {
            const auto energy = window_.frame_energy();
            if (!std::isfinite(energy))
                return false;
            if (have_previous_)
                novelty = onset_energy_flux(previous_energy_, energy);
            previous_energy_ = energy;
        } else if (method_ == OnsetDetectionMethod::SuperFlux) {
            if (!window_.render_spectrum())
                return false;
            novelty = superflux_.process(window_.spectrum().data());
        } else {
            if (!window_.render_spectrum())
                return false;
            const auto& spectrum = window_.spectrum();
            const auto bins = window_.config().fft_size / 2u + 1u;
            for (std::size_t bin = 0; bin < bins; ++bin) {
                const auto magnitude = static_cast<double>(std::abs(spectrum[bin]));
                if (!std::isfinite(magnitude))
                    return false;
                if (have_previous_) {
                    novelty +=
                        onset_spectral_bin_flux(method_, previous_magnitudes_[bin], magnitude, bin);
                }
                previous_magnitudes_[bin] = magnitude;
            }
        }
        if (!std::isfinite(novelty))
            return false;
        have_previous_ = true;

        const auto& config = window_.config();
        Frame next{};
        next.novelty = novelty;
        next.ready_at_frame = window_.stream_frames() - 1u;
        next.window_start_frame = window_.stream_frames() - config.fft_size;
        next.window_center_frame = next.window_start_frame + config.fft_size / 2u;
        next.sequence = ++sequence_;
        next.method = method_;
        current_ = next;
        return true;
    }

    detail::StreamingAnalysisWindowT<SampleType, MaxFftSize, MaxChannels> window_{};
    detail::SuperFluxNoveltyT<SampleType, MaxFftSize> superflux_{};
    std::array<double, MaxFftSize / 2u + 1u> previous_magnitudes_{};
    double previous_energy_ = 0.0;
    Frame current_{};
    OnsetDetectionMethod method_ = OnsetDetectionMethod::EnergyFlux;
    std::uint64_t sequence_ = 0;
    bool have_previous_ = false;
};

using ChromaFrontEnd = ChromaFrontEndT<float>;
using ChromaFrontEnd64 = ChromaFrontEndT<double>;
using OnsetNoveltyFrontEnd = OnsetNoveltyFrontEndT<float>;
using OnsetNoveltyFrontEnd64 = OnsetNoveltyFrontEndT<double>;

} // namespace pulp::signal
