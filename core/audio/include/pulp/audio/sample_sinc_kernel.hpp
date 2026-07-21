#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

namespace pulp::audio {

inline constexpr std::size_t kMaximumSampleSincCutoffTables = 32;
inline constexpr std::size_t kMaximumSampleSincOctaveTables = 8;
inline constexpr double kMaximumDenseSampleSincConsumption = 128.0;
inline constexpr std::uint32_t kDefaultSampleSincHalfWidth = 16;
inline constexpr std::uint32_t kHighQualitySampleSincHalfWidth = 24;

struct SampleSincKernelConfig {
    std::uint32_t half_width = kDefaultSampleSincHalfWidth;
    std::uint32_t phases = 512;
    double cutoff = 1.0;
    double kaiser_beta = 9.0;
};

class SampleSincKernel;

class SampleSincKernelView {
public:
    SampleSincKernelView() = default;
    // Borrowed immutable table view. The issuing kernel owner must remain
    // built and must not be released or rebuilt until every prepared reader
    // and block plan using this view is quiescent.
    std::uint32_t tap_count() const noexcept { return half_width_ * 2; }
    std::int32_t first_offset() const noexcept {
        return 1 - static_cast<std::int32_t>(half_width_);
    }
    std::uint32_t half_width() const noexcept { return half_width_; }
    std::uint32_t phases() const noexcept { return phases_; }
    double cutoff() const noexcept { return cutoff_; }
    const void* storage_identity() const noexcept { return table_; }
    bool valid() const noexcept {
        const auto count = static_cast<std::size_t>(tap_count());
        const auto rows = static_cast<std::size_t>(phases_) + 1;
        return table_ != nullptr && half_width_ >= 2 && half_width_ <= 64 &&
               phases_ >= 2 && phases_ <= 4096 &&
               cutoff_ > 0.0 && cutoff_ <= 1.0 && std::isfinite(cutoff_) &&
               rows <= std::numeric_limits<std::size_t>::max() / count &&
               table_sample_count_ >= rows * count;
    }

    float apply(std::span<const float> taps, double fraction) const noexcept {
        if (!valid() || taps.size() != tap_count() ||
            !std::isfinite(fraction)) {
            return 0.0f;
        }
        const auto phase = std::clamp(fraction, 0.0, 1.0) *
                           static_cast<double>(phases_);
        const auto first_phase = phase >= static_cast<double>(phases_)
            ? phases_ - 1
            : static_cast<std::uint32_t>(phase);
        const auto blend = static_cast<float>(
            std::clamp(phase - static_cast<double>(first_phase), 0.0, 1.0));
        const auto count = static_cast<std::size_t>(tap_count());
        const auto* first = table_ + static_cast<std::size_t>(first_phase) * count;
        const auto* second = first + count;
        float result = 0.0f;
        for (std::size_t tap = 0; tap < count; ++tap) {
            const auto coefficient =
                first[tap] + blend * (second[tap] - first[tap]);
            result += coefficient * taps[tap];
        }
        return result;
    }

private:
    friend class SampleSincKernel;

    SampleSincKernelView(const float* table,
                         std::size_t table_sample_count,
                         std::uint32_t half_width,
                         std::uint32_t phases,
                         double cutoff) noexcept
        : table_(table),
          table_sample_count_(table_sample_count),
          half_width_(half_width),
          phases_(phases),
          cutoff_(cutoff) {}

    const float* table_ = nullptr;
    std::size_t table_sample_count_ = 0;
    std::uint32_t half_width_ = 0;
    std::uint32_t phases_ = 0;
    double cutoff_ = 0.0;
};

static_assert(std::is_trivially_copyable_v<SampleSincKernelView>);

class SampleSincKernel {
public:
    SampleSincKernel() = default;
    SampleSincKernel(const SampleSincKernel&) = delete;
    SampleSincKernel& operator=(const SampleSincKernel&) = delete;
    SampleSincKernel(SampleSincKernel&&) = delete;
    SampleSincKernel& operator=(SampleSincKernel&&) = delete;

    bool build(const SampleSincKernelConfig& config) {
        release();
        if (!valid_config(config)) return false;
        const auto taps = static_cast<std::size_t>(config.half_width) * 2;
        const auto rows = static_cast<std::size_t>(config.phases) + 1;
        if (rows > std::numeric_limits<std::size_t>::max() / taps)
            return false;

        try {
            table_.assign(rows * taps, 0.0f);
        } catch (...) {
            return false;
        }

        const auto window_denominator = bessel_i0(config.kaiser_beta);
        for (std::uint32_t phase = 0; phase <= config.phases; ++phase) {
            const auto fraction = static_cast<double>(phase) /
                                  static_cast<double>(config.phases);
            double sum = 0.0;
            auto* row = table_.data() + static_cast<std::size_t>(phase) * taps;
            for (std::size_t tap = 0; tap < taps; ++tap) {
                const auto offset = static_cast<double>(
                    static_cast<std::int64_t>(tap) + 1 -
                    static_cast<std::int64_t>(config.half_width)) - fraction;
                const auto normalized = offset /
                                        static_cast<double>(config.half_width);
                const auto window = std::abs(normalized) < 1.0
                    ? bessel_i0(config.kaiser_beta *
                                std::sqrt(1.0 - normalized * normalized)) /
                          window_denominator
                    : 0.0;
                const auto coefficient = config.cutoff *
                    sinc(config.cutoff * offset) * window;
                row[tap] = static_cast<float>(coefficient);
                sum += coefficient;
            }
            if (!std::isfinite(sum) || std::abs(sum) < 1.0e-12) {
                release();
                return false;
            }
            const auto inverse_sum = 1.0 / sum;
            for (std::size_t tap = 0; tap < taps; ++tap)
                row[tap] = static_cast<float>(
                    static_cast<double>(row[tap]) * inverse_sum);
        }

        view_ = SampleSincKernelView(table_.data(), table_.size(),
                                     config.half_width, config.phases,
                                     config.cutoff);
        return true;
    }

    void release() noexcept {
        view_ = {};
        std::vector<float>().swap(table_);
    }

    SampleSincKernelView view() const noexcept { return view_; }

private:
    static bool valid_config(const SampleSincKernelConfig& config) noexcept {
        return config.half_width >= 2 && config.half_width <= 64 &&
               config.phases >= 2 && config.phases <= 4096 &&
               config.cutoff > 0.0 && config.cutoff <= 1.0 &&
               std::isfinite(config.cutoff) &&
               config.kaiser_beta >= 0.0 &&
               std::isfinite(config.kaiser_beta);
    }

    static double sinc(double value) noexcept {
        if (std::abs(value) < 1.0e-12) return 1.0;
        constexpr double pi = 3.1415926535897932384626433832795;
        const auto argument = pi * value;
        return std::sin(argument) / argument;
    }

    static double bessel_i0(double value) noexcept {
        double sum = 1.0;
        double term = 1.0;
        const auto half = value * 0.5;
        for (int order = 1; order < 64; ++order) {
            const auto ratio = half / static_cast<double>(order);
            term *= ratio * ratio;
            sum += term;
            if (term <= sum * 1.0e-15) break;
        }
        return sum;
    }

    std::vector<float> table_;
    SampleSincKernelView view_{};
};

struct SampleSincKernelSelection {
    SampleSincKernelView wider{};
    SampleSincKernelView narrower{};
    float narrower_gain = 0.0f;

    bool valid() const noexcept {
        return wider.valid() && narrower.valid() &&
               wider.tap_count() == narrower.tap_count() &&
               narrower_gain >= 0.0f && narrower_gain <= 1.0f &&
               std::isfinite(narrower_gain);
    }

    float apply(std::span<const float> taps, double fraction) const noexcept {
        if (!valid()) return 0.0f;
        if (narrower_gain <= 0.0f ||
            wider.storage_identity() == narrower.storage_identity())
            return wider.apply(taps, fraction);
        if (narrower_gain >= 1.0f)
            return narrower.apply(taps, fraction);
        const auto wide = wider.apply(taps, fraction);
        const auto narrow = narrower.apply(taps, fraction);
        return wide + narrower_gain * (narrow - wide);
    }
};

static_assert(std::is_trivially_copyable_v<SampleSincKernelSelection>);

struct SampleSincKernelBankView {
    std::array<SampleSincKernelView, kMaximumSampleSincCutoffTables> kernels{};
    std::uint32_t kernel_count = 0;

    bool valid() const noexcept {
        if (kernel_count == 0 || kernel_count > kernels.size()) return false;
        const auto taps = kernels[0].tap_count();
        for (std::uint32_t index = 0; index < kernel_count; ++index) {
            if (!kernels[index].valid() || kernels[index].tap_count() != taps)
                return false;
            if (index > 0 &&
                kernels[index].cutoff() >= kernels[index - 1].cutoff())
                return false;
        }
        return true;
    }

    SampleSincKernelSelection select(
        double source_frames_per_output) const noexcept {
        if (!valid() || !(source_frames_per_output > 0.0) ||
            !std::isfinite(source_frames_per_output)) {
            return {};
        }
        const auto target_cutoff = std::min(1.0, 1.0 / source_frames_per_output);
        if (target_cutoff >= kernels[0].cutoff())
            return {kernels[0], kernels[0], 0.0f};
        for (std::uint32_t index = 1; index < kernel_count; ++index) {
            if (target_cutoff == kernels[index].cutoff())
                return {kernels[index], kernels[index], 0.0f};
            if (target_cutoff < kernels[index].cutoff()) continue;
            const auto wider = kernels[index - 1];
            const auto narrower = kernels[index];
            const auto gain = static_cast<float>(
                (wider.cutoff() - target_cutoff) /
                (wider.cutoff() - narrower.cutoff()));
            return {wider, narrower, std::clamp(gain, 0.0f, 1.0f)};
        }
        return {};
    }
};

static_assert(std::is_trivially_copyable_v<SampleSincKernelBankView>);

class SampleSincKernelBank {
public:
    SampleSincKernelBank() = default;
    SampleSincKernelBank(const SampleSincKernelBank&) = delete;
    SampleSincKernelBank& operator=(const SampleSincKernelBank&) = delete;
    SampleSincKernelBank(SampleSincKernelBank&&) = delete;
    SampleSincKernelBank& operator=(SampleSincKernelBank&&) = delete;

    bool build_for_maximum_consumption(
        double maximum_source_frames_per_output,
        std::uint32_t half_width = kDefaultSampleSincHalfWidth,
        std::uint32_t phases = 512,
        double kaiser_beta = 9.0) {
        release();
        if (!(maximum_source_frames_per_output > 0.0) ||
            !std::isfinite(maximum_source_frames_per_output)) {
            return false;
        }
        const auto octave = maximum_source_frames_per_output <= 1.0
            ? 0.0
            : std::ceil(std::log2(maximum_source_frames_per_output));
        if (octave >= static_cast<double>(kMaximumSampleSincOctaveTables))
            return false;
        return build(static_cast<std::uint32_t>(octave) + 1,
                     half_width, phases, kaiser_beta);
    }

    // Production-quality bank: quarter-octave cutoff spacing bounds the
    // above-target contribution while preserving continuous table blending.
    // The 48-tap default supplies enough transition rejection for the first
    // fractional-downsample interval, where octave-only 32-tap tables leak a
    // near-Nyquist source tone into the output by only 7-11 dB.
    bool build_dense_for_maximum_consumption(
        double maximum_source_frames_per_output,
        std::uint32_t tables_per_octave = 4,
        std::uint32_t half_width = kHighQualitySampleSincHalfWidth,
        std::uint32_t phases = 512,
        double kaiser_beta = 9.0) {
        release();
        if (!(maximum_source_frames_per_output > 0.0) ||
            maximum_source_frames_per_output >
                kMaximumDenseSampleSincConsumption ||
            !std::isfinite(maximum_source_frames_per_output) ||
            tables_per_octave == 0 || tables_per_octave > 8) {
            return false;
        }
        const auto intervals = maximum_source_frames_per_output <= 1.0
            ? 0.0
            : std::ceil(std::log2(maximum_source_frames_per_output) *
                        static_cast<double>(tables_per_octave));
        if (intervals >= static_cast<double>(kMaximumSampleSincCutoffTables))
            return false;
        const auto table_count = static_cast<std::uint32_t>(intervals) + 1;
        for (std::uint32_t index = 0; index < table_count; ++index) {
            const auto cutoff = std::exp2(
                -static_cast<double>(index) /
                static_cast<double>(tables_per_octave));
            if (!kernels_[index].build(
                    {half_width, phases, cutoff, kaiser_beta})) {
                release();
                return false;
            }
            view_.kernels[index] = kernels_[index].view();
        }
        view_.kernel_count = table_count;
        return true;
    }

    bool build(std::uint32_t cutoff_table_count,
               std::uint32_t half_width = kDefaultSampleSincHalfWidth,
               std::uint32_t phases = 512,
               double kaiser_beta = 9.0) {
        release();
        if (cutoff_table_count == 0 ||
            cutoff_table_count > kMaximumSampleSincCutoffTables) {
            return false;
        }
        for (std::uint32_t index = 0; index < cutoff_table_count; ++index) {
            const auto cutoff = std::ldexp(1.0, -static_cast<int>(index));
            if (!kernels_[index].build({half_width, phases, cutoff, kaiser_beta})) {
                release();
                return false;
            }
            view_.kernels[index] = kernels_[index].view();
        }
        view_.kernel_count = cutoff_table_count;
        return true;
    }

    void release() noexcept {
        view_ = {};
        for (auto& kernel : kernels_) kernel.release();
    }

    SampleSincKernelBankView view() const noexcept { return view_; }

private:
    std::array<SampleSincKernel, kMaximumSampleSincCutoffTables> kernels_{};
    SampleSincKernelBankView view_{};
};

}  // namespace pulp::audio
