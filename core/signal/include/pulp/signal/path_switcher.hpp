#pragma once

#include "crossfade.hpp"
#include "detail/audio_range.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <type_traits>

namespace pulp::signal {

/// Fixed-capacity, click-free path selector. Requests made during a fade begin
/// from the current N-way gain vector, so automation never jumps. The vector is
/// smoothstep-interpolated and L2-normalized on every sample, preserving a
/// constant-power gain law for decorrelated paths. Processing is allocation-
/// free; source buffers must not alias the destination.
template <typename SampleType = float, std::size_t MaxPaths = 16> class ClickFreePathSwitcherT {
  public:
    static_assert(MaxPaths > 0);
    static_assert(std::is_floating_point_v<SampleType>);
    static constexpr std::size_t max_paths = MaxPaths;

    bool configure(std::size_t paths, std::size_t initial_path, std::size_t fade_samples) noexcept {
        if (paths == 0 || paths > MaxPaths || initial_path >= paths)
            return false;
        paths_ = paths;
        fade_samples_ = fade_samples < 2 ? 0 : fade_samples;
        selected_path_ = initial_path;
        position_ = fade_samples_;
        from_.fill(SampleType{0});
        weights_.fill(SampleType{0});
        target_.fill(SampleType{0});
        weights_[initial_path] = SampleType{1};
        from_[initial_path] = SampleType{1};
        target_[initial_path] = SampleType{1};
        return true;
    }

    bool request_path(std::size_t path) noexcept {
        if (path >= paths_)
            return false;
        selected_path_ = path;
        from_ = weights_;
        target_.fill(SampleType{0});
        target_[path] = SampleType{1};
        position_ = 0;
        if (fade_samples_ == 0) {
            weights_ = target_;
            position_ = 0;
        }
        return true;
    }

    void reset() noexcept {
        weights_.fill(SampleType{0});
        from_.fill(SampleType{0});
        target_.fill(SampleType{0});
        if (paths_ > 0) {
            weights_[selected_path_] = SampleType{1};
            from_[selected_path_] = SampleType{1};
            target_[selected_path_] = SampleType{1};
        }
        position_ = fade_samples_;
    }

    std::size_t path_count() const noexcept {
        return paths_;
    }
    std::size_t selected_path() const noexcept {
        return selected_path_;
    }
    bool switching() const noexcept {
        return position_ < fade_samples_;
    }
    std::span<const SampleType> weights() const noexcept {
        return {weights_.data(), paths_};
    }

    bool next_gains(std::span<SampleType> destination) noexcept {
        if (paths_ == 0 || destination.size() < paths_)
            return false;
        if (position_ < fade_samples_) {
            if (position_ == 0) {
                weights_ = from_;
            } else if (position_ + 1 == fade_samples_) {
                weights_ = target_;
            } else {
                using ProgressType =
                    std::conditional_t<(sizeof(SampleType) < sizeof(double)), double, long double>;
                const auto t = static_cast<ProgressType>(position_) /
                               static_cast<ProgressType>(fade_samples_ - 1);
                const auto u = crossfade_smoothstep(t);
                std::array<ProgressType, MaxPaths> mixed{};
                ProgressType norm_squared{};
                for (std::size_t path = 0; path < paths_; ++path) {
                    mixed[path] = static_cast<ProgressType>(from_[path]) * (ProgressType{1} - u) +
                                  static_cast<ProgressType>(target_[path]) * u;
                    norm_squared += mixed[path] * mixed[path];
                }
                const auto scale = norm_squared > ProgressType{0}
                                       ? ProgressType{1} / std::sqrt(norm_squared)
                                       : ProgressType{1};
                for (std::size_t path = 0; path < paths_; ++path)
                    weights_[path] = static_cast<SampleType>(mixed[path] * scale);
            }
            ++position_;
        }
        std::copy_n(weights_.begin(), paths_, destination.begin());
        return true;
    }

    bool process(const SampleType* const* sources, std::size_t source_count,
                 SampleType* destination, std::size_t frames) noexcept {
        if (sources == nullptr || destination == nullptr || source_count != paths_ || paths_ == 0)
            return false;
        for (std::size_t path = 0; path < paths_; ++path)
            if (sources[path] == nullptr ||
                detail::audio_ranges_overlap(sources[path], destination, frames))
                return false;
        std::array<SampleType, MaxPaths> gains{};
        for (std::size_t frame = 0; frame < frames; ++frame) {
            next_gains(std::span<SampleType>(gains.data(), paths_));
            SampleType sum{};
            for (std::size_t path = 0; path < paths_; ++path)
                sum += sources[path][frame] * gains[path];
            destination[frame] = sum;
        }
        return true;
    }

  private:
    std::size_t paths_ = 0;
    std::size_t selected_path_ = 0;
    std::size_t fade_samples_ = 0;
    std::size_t position_ = 0;
    std::array<SampleType, MaxPaths> from_{};
    std::array<SampleType, MaxPaths> target_{};
    std::array<SampleType, MaxPaths> weights_{};
};

using ClickFreePathSwitcher = ClickFreePathSwitcherT<float>;
using ClickFreePathSwitcher64 = ClickFreePathSwitcherT<double>;

} // namespace pulp::signal
