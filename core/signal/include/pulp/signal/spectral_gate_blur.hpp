#pragma once

/// @file spectral_gate_blur.hpp
/// Allocation-free frame-domain spectral gate and bounded temporal blur.
///
/// These processors operate on the one-sided complex frames emitted by
/// `SpectralFrameEngineT`; they deliberately do not own or duplicate an FFT.
/// The gate compares every bin independently with either one scalar magnitude
/// threshold or a caller-owned threshold curve. The blur is a causal finite
/// moving average of bin magnitudes and retains the current non-zero phase (or
/// the last finite phase while a bin decays). A finite complex bin whose
/// mathematical magnitude exceeds `SampleType` saturates to its maximum finite
/// magnitude without changing phase. Both processors modify frames in place.

#include <pulp/signal/checked_allocation.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

namespace pulp::signal {

template <typename SampleType = float>
class SpectralGateT {
public:
    static_assert(std::is_floating_point_v<SampleType>);

    /// Set the scalar linear-magnitude threshold. Invalid values are rejected
    /// without changing the live threshold.
    bool set_threshold_magnitude(SampleType threshold) noexcept {
        if (!std::isfinite(threshold) || threshold < SampleType{0}) return false;
        threshold_magnitude_ = threshold;
        return true;
    }

    SampleType threshold_magnitude() const noexcept { return threshold_magnitude_; }

    /// Gate a coherent channel group in place. When `thresholds` is non-null,
    /// it points to `num_bins` finite, non-negative per-bin thresholds and
    /// overrides the scalar threshold for this frame. Invalid geometry or a
    /// bad threshold curve is rejected before any bin is changed. Non-finite
    /// spectral input is replaced with silence so it cannot poison synthesis.
    bool process(std::complex<SampleType>* const* frames, int channels, int num_bins,
                 const SampleType* thresholds = nullptr) noexcept {
        if (frames == nullptr || channels <= 0 || num_bins <= 0) return false;
        for (int ch = 0; ch < channels; ++ch)
            if (frames[ch] == nullptr) return false;
        if (thresholds != nullptr) {
            for (int bin = 0; bin < num_bins; ++bin) {
                if (!std::isfinite(thresholds[bin])
                    || thresholds[bin] < SampleType{0})
                    return false;
            }
        }

        for (int ch = 0; ch < channels; ++ch) {
            for (int bin = 0; bin < num_bins; ++bin) {
                auto& value = frames[ch][bin];
                if (!std::isfinite(value.real()) || !std::isfinite(value.imag())) {
                    value = {};
                    continue;
                }
                const SampleType threshold =
                    thresholds != nullptr ? thresholds[bin] : threshold_magnitude_;
                if (std::abs(value) < threshold) value = {};
            }
        }
        return true;
    }

private:
    SampleType threshold_magnitude_ = SampleType{0};
};

template <typename SampleType = float>
class SpectralFrameBlurT {
public:
    static_assert(std::is_floating_point_v<SampleType>);

    static constexpr int maximum_channels = 64;
    static constexpr int maximum_bins = 8193;
    static constexpr int maximum_frames = 128;

    static bool checked_retained_bytes(int channels, int num_bins, int blur_frames,
                                       std::uint64_t target_max_bytes,
                                       std::uint64_t& bytes) noexcept {
        if (channels <= 0 || channels > maximum_channels || num_bins <= 0
            || num_bins > maximum_bins || blur_frames <= 0
            || blur_frames > maximum_frames)
            return false;

        std::uint64_t frame_elements = 0;
        std::uint64_t history_elements = 0;
        if (!checked_capacity_product(static_cast<std::uint64_t>(channels),
                                      static_cast<std::uint64_t>(num_bins),
                                      std::numeric_limits<std::uint64_t>::max(),
                                      frame_elements)
            || !checked_capacity_product(frame_elements,
                                         static_cast<std::uint64_t>(blur_frames),
                                         std::numeric_limits<std::uint64_t>::max(),
                                         history_elements))
            return false;

        CheckedRetainedByteCharge charge(target_max_bytes);
        if (!charge.add<SampleType>(history_elements)
            || !charge.add<SampleType>(frame_elements)
            || !charge.add<std::uint16_t>(frame_elements)
            || !charge.add<std::complex<SampleType>>(frame_elements))
            return false;
        bytes = charge.total();
        return true;
    }

    static bool supports_configuration(int channels, int num_bins,
                                       int blur_frames) noexcept {
        std::uint64_t ignored = 0;
        return checked_retained_bytes(channels, num_bins, blur_frames,
                                      kTargetAddressMaximumBytes, ignored);
    }

    /// Allocate fixed history off the audio thread. Invalid geometry is
    /// rejected without changing the prepared instance.
    bool prepare(int channels, int num_bins, int blur_frames) {
        std::uint64_t retained_bytes = 0;
        if (!checked_retained_bytes(channels, num_bins, blur_frames,
                                    kTargetAddressMaximumBytes, retained_bytes))
            return false;

        const auto frame_elements = static_cast<std::size_t>(channels)
                                  * static_cast<std::size_t>(num_bins);
        std::vector<SampleType> new_history(
            frame_elements * static_cast<std::size_t>(blur_frames), SampleType{0});
        std::vector<SampleType> new_means(frame_elements, SampleType{0});
        std::vector<std::uint16_t> new_nonzero_counts(frame_elements, 0);
        std::vector<std::complex<SampleType>> new_phases(
            frame_elements, std::complex<SampleType>{SampleType{1}, SampleType{0}});

        history_.swap(new_history);
        means_.swap(new_means);
        nonzero_counts_.swap(new_nonzero_counts);
        phases_.swap(new_phases);
        channels_ = channels;
        num_bins_ = num_bins;
        blur_frames_ = blur_frames;
        retained_bytes_ = retained_bytes;
        write_frame_ = 0;
        filled_frames_ = 0;
        return true;
    }

    /// Clear the finite history without changing prepared capacity.
    void reset() noexcept {
        std::fill(history_.begin(), history_.end(), SampleType{0});
        std::fill(means_.begin(), means_.end(), SampleType{0});
        std::fill(nonzero_counts_.begin(), nonzero_counts_.end(), std::uint16_t{0});
        std::fill(phases_.begin(), phases_.end(),
                  std::complex<SampleType>{SampleType{1}, SampleType{0}});
        write_frame_ = 0;
        filled_frames_ = 0;
    }

    /// Apply a causal box blur across at most `blur_frames()` analysis frames.
    /// Startup divides by the number of frames actually observed, not by zero
    /// padding. Invalid geometry is rejected before mutation. Non-finite bins
    /// enter history as silence; finite subsequent input therefore recovers.
    bool process(std::complex<SampleType>* const* frames, int channels,
                 int num_bins) noexcept {
        if (channels != channels_ || num_bins != num_bins_ || frames == nullptr
            || blur_frames_ <= 0)
            return false;
        for (int ch = 0; ch < channels_; ++ch)
            if (frames[ch] == nullptr) return false;

        const auto frame_elements = static_cast<std::size_t>(channels_)
                                  * static_cast<std::size_t>(num_bins_);
        const auto history_offset = static_cast<std::size_t>(write_frame_)
                                  * frame_elements;
        const bool history_full = filled_frames_ >= blur_frames_;
        const auto divisor = static_cast<SampleType>(
            std::min(filled_frames_ + 1, blur_frames_));

        for (int ch = 0; ch < channels_; ++ch) {
            for (int bin = 0; bin < num_bins_; ++bin) {
                const auto index = static_cast<std::size_t>(ch)
                                 * static_cast<std::size_t>(num_bins_)
                                 + static_cast<std::size_t>(bin);
                auto& value = frames[ch][bin];
                SampleType magnitude = SampleType{0};
                if (std::isfinite(value.real()) && std::isfinite(value.imag())) {
                    const SampleType real = value.real();
                    const SampleType imag = value.imag();
                    const SampleType scale = std::max(std::abs(real), std::abs(imag));
                    if (scale > SampleType{0}) {
                        const SampleType scaled_real = real / scale;
                        const SampleType scaled_imag = imag / scale;
                        const SampleType scaled_magnitude =
                            std::hypot(scaled_real, scaled_imag);
                        phases_[index] = {scaled_real / scaled_magnitude,
                                          scaled_imag / scaled_magnitude};
                        const SampleType maximum =
                            std::numeric_limits<SampleType>::max();
                        magnitude = scale > maximum / scaled_magnitude
                            ? maximum
                            : scale * scaled_magnitude;
                    }
                }

                auto& old_magnitude = history_[history_offset + index];
                if (old_magnitude > SampleType{0}) --nonzero_counts_[index];
                if (magnitude > SampleType{0}) ++nonzero_counts_[index];
                auto& mean = means_[index];
                if (history_full)
                    mean = std::fma(SampleType{1} / divisor,
                                    magnitude - old_magnitude, mean);
                else
                    mean += (magnitude - mean) / divisor;
                if (nonzero_counts_[index] == 0)
                    mean = SampleType{0};
                else if (!std::isfinite(mean))
                    mean = std::numeric_limits<SampleType>::max();
                old_magnitude = magnitude;
                value = phases_[index] * std::max(mean, SampleType{0});
            }
        }

        write_frame_ = (write_frame_ + 1) % blur_frames_;
        if (filled_frames_ < blur_frames_) ++filled_frames_;
        return true;
    }

    int channels() const noexcept { return channels_; }
    int num_bins() const noexcept { return num_bins_; }
    int blur_frames() const noexcept { return blur_frames_; }
    int filled_frames() const noexcept { return filled_frames_; }
    std::uint64_t retained_bytes() const noexcept { return retained_bytes_; }

private:
    int channels_ = 0;
    int num_bins_ = 0;
    int blur_frames_ = 0;
    int write_frame_ = 0;
    int filled_frames_ = 0;
    std::uint64_t retained_bytes_ = 0;
    std::vector<SampleType> history_;
    std::vector<SampleType> means_;
    std::vector<std::uint16_t> nonzero_counts_;
    std::vector<std::complex<SampleType>> phases_;
};

using SpectralGate = SpectralGateT<float>;
using SpectralGate64 = SpectralGateT<double>;
using SpectralFrameBlur = SpectralFrameBlurT<float>;
using SpectralFrameBlur64 = SpectralFrameBlurT<double>;

} // namespace pulp::signal
