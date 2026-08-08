#pragma once

/// @file nway_crossfade.hpp
/// Fixed-capacity, real-time-safe crossfade across an ordered set of paths.

#include "crossfade.hpp"

#include <pulp/runtime/triple_buffer.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace pulp::signal {

enum class NWayCrossfadePrepareError {
    None,
    TooFewPaths,
    TooManyPaths,
    NonFinitePosition,
    InvalidGainLaw,
};

enum class NWayCrossfadeProcessStatus {
    Ok,
    Unprepared,
    InsufficientInputs,
    ShortInput,
    ShortOutput,
    OverlappingBuffers,
};

/// Immutable control-thread product consumed by `NWayCrossfadeT::process()`.
///
/// `position` addresses the ordered paths: 0 selects path 0, 1 selects path 1,
/// and so on. Values outside the range clamp to the nearest endpoint. Between
/// integer positions only the two adjacent paths are active. EqualGain has an
/// L1 weight sum of one; EqualPower has a squared-weight sum of one. This makes
/// a two-path plan exactly equivalent to `crossfade_gains(position, law)`.
template <typename SampleType, std::size_t MaxPaths>
class NWayCrossfadePlanT {
    static_assert(std::is_floating_point_v<SampleType>);
    static_assert(MaxPaths >= 2);

public:
    NWayCrossfadePrepareError prepare(std::size_t path_count, SampleType position,
                                      CrossfadeGainLaw law) noexcept {
        if (path_count < 2) return NWayCrossfadePrepareError::TooFewPaths;
        if (path_count > MaxPaths) return NWayCrossfadePrepareError::TooManyPaths;
        if (!std::isfinite(position)) return NWayCrossfadePrepareError::NonFinitePosition;
        if (law != CrossfadeGainLaw::EqualGain && law != CrossfadeGainLaw::EqualPower)
            return NWayCrossfadePrepareError::InvalidGainLaw;

        std::array<SampleType, MaxPaths> next_weights{};
        const SampleType last = static_cast<SampleType>(path_count - 1);
        const SampleType clamped = std::clamp(position, SampleType{0}, last);
        const auto left = static_cast<std::size_t>(clamped);
        if (left == path_count - 1) {
            next_weights[left] = SampleType{1};
        } else {
            const SampleType local_position = clamped - static_cast<SampleType>(left);
            crossfade_gains(local_position, law, next_weights[left], next_weights[left + 1]);
        }

        weights_ = next_weights;
        path_count_ = path_count;
        position_ = clamped;
        law_ = law;
        prepared_ = true;
        return NWayCrossfadePrepareError::None;
    }

    bool prepared() const noexcept { return prepared_; }
    std::size_t path_count() const noexcept { return path_count_; }
    SampleType position() const noexcept { return position_; }
    CrossfadeGainLaw gain_law() const noexcept { return law_; }
    std::span<const SampleType> weights() const noexcept {
        return {weights_.data(), path_count_};
    }

private:
    std::array<SampleType, MaxPaths> weights_{};
    std::size_t path_count_ = 0;
    SampleType position_ = SampleType{0};
    CrossfadeGainLaw law_ = CrossfadeGainLaw::EqualGain;
    bool prepared_ = false;
};

/// Block mixer with fixed-capacity, transactional control publication.
///
/// One control thread calls `configure()` and one audio thread calls `process()`.
/// A rejected configuration is never published. The process path performs no
/// allocation, locking, or I/O and reads one immutable plan for the whole block.
template <typename SampleType = float, std::size_t MaxPaths = 16>
class NWayCrossfadeT {
public:
    using Plan = NWayCrossfadePlanT<SampleType, MaxPaths>;

    NWayCrossfadePrepareError configure(std::size_t path_count, SampleType position,
                                        CrossfadeGainLaw law) noexcept {
        Plan next;
        const auto error = next.prepare(path_count, position, law);
        if (error == NWayCrossfadePrepareError::None) plans_.write(next);
        return error;
    }

    NWayCrossfadeProcessStatus process(std::span<const std::span<const SampleType>> inputs,
                                       std::span<SampleType> output,
                                       std::size_t frames) noexcept {
        const Plan& plan = plans_.read();
        if (!plan.prepared()) return NWayCrossfadeProcessStatus::Unprepared;
        if (inputs.size() < plan.path_count())
            return NWayCrossfadeProcessStatus::InsufficientInputs;
        if (output.size() < frames) return NWayCrossfadeProcessStatus::ShortOutput;

        for (std::size_t path = 0; path < plan.path_count(); ++path) {
            if (inputs[path].size() < frames) return NWayCrossfadeProcessStatus::ShortInput;
            if (overlaps(inputs[path].first(frames), output.first(frames)) &&
                inputs[path].data() != output.data())
                return NWayCrossfadeProcessStatus::OverlappingBuffers;
        }

        const auto weights = plan.weights();
        for (std::size_t frame = 0; frame < frames; ++frame) {
            SampleType mixed = SampleType{0};
            for (std::size_t path = 0; path < plan.path_count(); ++path)
                mixed += inputs[path][frame] * weights[path];
            output[frame] = mixed;
        }
        return NWayCrossfadeProcessStatus::Ok;
    }

private:
    static bool overlaps(std::span<const SampleType> input,
                         std::span<SampleType> output) noexcept {
        if (input.empty() || output.empty()) return false;
        const auto input_begin = reinterpret_cast<std::uintptr_t>(input.data());
        const auto output_begin = reinterpret_cast<std::uintptr_t>(output.data());
        const auto input_end = input_begin + input.size_bytes();
        const auto output_end = output_begin + output.size_bytes();
        return input_begin < output_end && output_begin < input_end;
    }

    pulp::runtime::TripleBuffer<Plan> plans_{};
};

using NWayCrossfade = NWayCrossfadeT<float, 16>;
using NWayCrossfade64 = NWayCrossfadeT<double, 16>;

}  // namespace pulp::signal
