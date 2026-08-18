#pragma once

/// @file transfer_curve.hpp
/// Fixed-capacity arbitrary transfer curves with lock-free publication.

#include <pulp/runtime/triple_buffer.hpp>
#include <pulp/signal/modulation_curve.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <type_traits>

namespace pulp::signal {

template <typename SampleType>
struct TransferCurvePointT {
    SampleType input{};
    SampleType output{};
    ModulationCurve curve_to_next{};
};

template <typename SampleType = float, std::size_t MaxPoints = 32>
struct PreparedTransferCurveT {
    static_assert(MaxPoints >= 2, "a transfer curve needs room for two endpoints");

    using Point = TransferCurvePointT<SampleType>;

    std::array<Point, MaxPoints> points{};
    std::array<SampleType, MaxPoints - 1> inverse_input_deltas{};
    std::size_t point_count = 0;
    SampleType input_min{};
    SampleType input_max{};
    SampleType output_min{};
    SampleType output_max{};
};

/// Validate and prepare a piecewise transfer curve.
///
/// Points must be finite, strictly ordered by input, and lie inside the
/// explicit input/output domains. The first and last point must coincide with
/// the input-domain endpoints. Invalid input returns `std::nullopt` without a
/// partial result. This bounded control-side operation allocates no memory.
template <typename SampleType = float, std::size_t MaxPoints = 32>
std::optional<PreparedTransferCurveT<SampleType, MaxPoints>> prepare_transfer_curve(
    std::span<const TransferCurvePointT<SampleType>> points,
    SampleType input_min,
    SampleType input_max,
    SampleType output_min,
    SampleType output_max) noexcept {
    static_assert(std::is_floating_point_v<SampleType>,
                  "transfer curves require a floating-point sample type");

    if (points.size() < 2 || points.size() > MaxPoints ||
        !std::isfinite(input_min) || !std::isfinite(input_max) ||
        !std::isfinite(output_min) || !std::isfinite(output_max) ||
        !(input_min < input_max) || output_min > output_max ||
        !std::isfinite(input_max - input_min) ||
        !std::isfinite(output_max - output_min) ||
        points.front().input != input_min || points.back().input != input_max)
        return std::nullopt;

    PreparedTransferCurveT<SampleType, MaxPoints> result{};
    result.point_count = points.size();
    result.input_min = input_min;
    result.input_max = input_max;
    result.output_min = output_min;
    result.output_max = output_max;

    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto point = points[i];
        if (!std::isfinite(point.input) || !std::isfinite(point.output) ||
            point.input < input_min || point.input > input_max ||
            point.output < output_min || point.output > output_max)
            return std::nullopt;

        result.points[i] = point;
        result.points[i].curve_to_next = sanitize_modulation_curve(point.curve_to_next);
        if (i == 0) continue;

        const auto input_delta = point.input - points[i - 1].input;
        if (!(input_delta > SampleType{}) || !std::isfinite(input_delta) ||
            !std::isfinite(point.output - points[i - 1].output))
            return std::nullopt;

        const auto inverse_input_delta = SampleType{1} / input_delta;
        if (!std::isfinite(inverse_input_delta)) return std::nullopt;
        result.inverse_input_deltas[i - 1] = inverse_input_delta;
    }

    return result;
}

/// Evaluate a prepared curve. Finite inputs are clamped to the explicit input
/// domain; non-finite inputs recover to silence. Segment interpolation uses
/// the shared modulation-curve vocabulary and cannot overshoot its endpoints.
template <typename SampleType, std::size_t MaxPoints>
SampleType evaluate_transfer_curve(
    const PreparedTransferCurveT<SampleType, MaxPoints>& curve,
    SampleType input) noexcept {
    if (!std::isfinite(input) || curve.point_count < 2 || curve.point_count > MaxPoints)
        return SampleType{};
    if (input <= curve.input_min) return curve.points.front().output;
    if (input >= curve.input_max) return curve.points[curve.point_count - 1].output;

    std::size_t low = 0;
    std::size_t high = curve.point_count - 1;
    while (high - low > 1) {
        const auto middle = low + (high - low) / 2;
        if (input < curve.points[middle].input)
            high = middle;
        else
            low = middle;
    }

    const auto& left = curve.points[low];
    const auto& right = curve.points[low + 1];
    const auto progress = (input - left.input) * curve.inverse_input_deltas[low];
    const auto output = interpolate_modulation_curve(
        left.output, right.output, progress, left.curve_to_next);
    if (!std::isfinite(output)) return SampleType{};
    return std::clamp(output, std::min(left.output, right.output),
                      std::max(left.output, right.output));
}

/// Audio-thread processor for a fixed-capacity arbitrary transfer curve.
///
/// `publish_curve()` is the single-writer control-side entry point. A complete,
/// validated curve is published through a triple buffer and becomes visible at
/// the next scalar or block process boundary. The audio path is lock-free and
/// allocation-free. The processor deliberately owns no oversampling policy;
/// callers that need alias suppression compose it with `OversamplerT`.
template <typename SampleType = float, std::size_t MaxPoints = 32>
class TransferCurveT {
public:
    using Point = TransferCurvePointT<SampleType>;
    using PreparedCurve = PreparedTransferCurveT<SampleType, MaxPoints>;

    static constexpr std::size_t max_points = MaxPoints;

    TransferCurveT() noexcept : published_(identity_curve()), active_(identity_curve()) {}

    bool publish_curve(std::span<const Point> points,
                       SampleType input_min,
                       SampleType input_max,
                       SampleType output_min,
                       SampleType output_max) noexcept {
        const auto prepared = prepare_transfer_curve<SampleType, MaxPoints>(
            points, input_min, input_max, output_min, output_max);
        if (!prepared) return false;
        published_.write(*prepared);
        return true;
    }

    SampleType process(SampleType input) noexcept {
        sync_curve();
        return evaluate_transfer_curve(active_, input);
    }

    void process(const SampleType* input, SampleType* output, int num_samples) noexcept {
        if (input == nullptr || output == nullptr || num_samples <= 0) return;
        sync_curve();
        for (int i = 0; i < num_samples; ++i)
            output[i] = evaluate_transfer_curve(active_, input[i]);
    }

    void process(SampleType* samples, int num_samples) noexcept {
        process(samples, samples, num_samples);
    }

    /// A transfer curve has no history. Reset only adopts the latest complete
    /// control-side publication so the next sample starts from that snapshot.
    void reset() noexcept { sync_curve(); }

    static constexpr int latency_samples() noexcept { return 0; }
    static constexpr int tail_samples() noexcept { return 0; }

private:
    static constexpr PreparedCurve identity_curve() noexcept {
        PreparedCurve curve{};
        curve.points[0] = Point{SampleType{-1}, SampleType{-1}};
        curve.points[1] = Point{SampleType{1}, SampleType{1}};
        curve.inverse_input_deltas[0] = SampleType{0.5};
        curve.point_count = 2;
        curve.input_min = SampleType{-1};
        curve.input_max = SampleType{1};
        curve.output_min = SampleType{-1};
        curve.output_max = SampleType{1};
        return curve;
    }

    void sync_curve() noexcept { active_ = published_.read(); }

    pulp::runtime::TripleBuffer<PreparedCurve> published_;
    PreparedCurve active_{};
};

using TransferCurvePoint = TransferCurvePointT<float>;
using TransferCurvePoint64 = TransferCurvePointT<double>;
using PreparedTransferCurve = PreparedTransferCurveT<float, 32>;
using PreparedTransferCurve64 = PreparedTransferCurveT<double, 32>;
using TransferCurve = TransferCurveT<float, 32>;
using TransferCurve64 = TransferCurveT<double, 32>;

} // namespace pulp::signal
