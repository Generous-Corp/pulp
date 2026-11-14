#pragma once

/// @file custom_node_oversampling.hpp
/// One spec that drives both halves of an oversampled custom node: the latency
/// it declares to the graph, and the oversampler it owns and runs.
///
/// A custom node that oversamples its work already has to say two things that
/// must agree: the latency of its anti-aliasing filters (so the graph can line
/// it up with parallel paths) and the oversampler's own configuration. Writing
/// them separately lets them drift apart. `CustomNodeOversampling` is the single
/// description both are derived from.
///
/// Only a constant delay can be declared. The linear-phase filter kind delays
/// every frequency by the same whole number of samples, so the graph can cancel
/// it with a delay line on a parallel path. The two minimum-phase kinds
/// (`fir_biquad` and `polyphase_iir`) delay different frequencies by different
/// amounts, so no single sample count is correct; this helper refuses them
/// instead of declaring a number that would misalign the dry path.
///
/// Threading: `oversampling_latency_samples` and `declare_oversampling_latency`
/// build a throwaway oversampler, so call them from the control thread (type
/// registration or prepare), never from the audio callback. `configure_oversampler`
/// belongs in the node's `prepare`.
///
/// Example:
/// @code
///   CustomNodeOversampling spec;
///   spec.factor = 4;                       // linear-phase, standard quality
///
///   CustomNodeType type;
///   // ... ports, create, destroy, process_instance ...
///   if (!declare_oversampling_latency(type, spec))
///       return false;                      // not a compensable configuration
///   type.prepare = [spec](void* p, double sample_rate, int) {
///       auto* node = static_cast<MyNode*>(p);
///       configure_oversampler(node->oversampler, spec, sample_rate);
///   };
/// @endcode

#include <pulp/host/custom_node_type.hpp>
#include <pulp/signal/oversampling.hpp>

namespace pulp::host {

/// How a custom node oversamples. Defaults to the linear-phase filter at
/// standard quality, the only family whose delay can be declared.
struct CustomNodeOversampling {
    /// 1 (no oversampling), 2, 4, 8 or 16.
    int factor = 1;
    signal::OversamplerT<float>::Kind kind = signal::OversamplerT<float>::Kind::linear_phase_fir;
    signal::OversamplerT<float>::Quality quality = signal::OversamplerT<float>::Quality::standard;
};

namespace detail {

/// Maps a factor of 2, 4, 8 or 16 to the oversampler's enum. Returns false for
/// anything else, including 1, which has no oversampler factor of its own.
inline bool oversampling_factor_enum(int factor,
                                     signal::OversamplerT<float>::Factor& out) noexcept {
    using Factor = signal::OversamplerT<float>::Factor;
    switch (factor) {
    case 2:
        out = Factor::x2;
        return true;
    case 4:
        out = Factor::x4;
        return true;
    case 8:
        out = Factor::x8;
        return true;
    case 16:
        out = Factor::x16;
        return true;
    default:
        return false;
    }
}

} // namespace detail

/// True when the spec is valid and its delay is a constant sample count the
/// graph can compensate: factor 1, or the linear-phase kind at factor 2, 4, 8
/// or 16. Safe on any thread.
inline bool oversampling_is_compensable(const CustomNodeOversampling& spec) noexcept {
    if (spec.factor == 1)
        return true;
    signal::OversamplerT<float>::Factor ignored{};
    if (!detail::oversampling_factor_enum(spec.factor, ignored))
        return false;
    return spec.kind == signal::OversamplerT<float>::Kind::linear_phase_fir;
}

/// Round-trip latency of the spec in base-rate samples: 0 for factor 1, the
/// linear-phase filters' constant delay otherwise. Returns -1 for an invalid
/// factor or a kind whose delay is not constant. The result does not depend on
/// the sample rate. Control thread only: it builds a throwaway oversampler.
inline int oversampling_latency_samples(const CustomNodeOversampling& spec) {
    if (!oversampling_is_compensable(spec))
        return -1;
    if (spec.factor == 1)
        return 0;
    signal::OversamplerT<float>::Factor factor{};
    detail::oversampling_factor_enum(spec.factor, factor);
    signal::OversamplerT<float> probe;
    probe.set_kind(spec.kind);
    probe.set_quality(spec.quality);
    probe.set_factor(factor);
    const auto latency = probe.latency();
    return latency.constant ? latency.input_samples : -1;
}

/// Sets `type.latency_samples` to the spec's oversampling latency plus
/// `extra_latency_samples` (for example a lookahead the node adds on top).
///
/// Returns false and leaves `type` untouched when the spec is not compensable,
/// `extra_latency_samples` is negative, the total exceeds
/// `CustomNodeType::kMaxLatencySamples`, or the type already declares
/// `latency_samples`, `latency_samples_for_block` or
/// `output_port_latency_samples`. Control thread only.
inline bool declare_oversampling_latency(CustomNodeType& type, const CustomNodeOversampling& spec,
                                         int extra_latency_samples = 0) {
    if (type.latency_samples || type.latency_samples_for_block || type.output_port_latency_samples)
        return false;
    if (extra_latency_samples < 0)
        return false;
    const int base = oversampling_latency_samples(spec);
    if (base < 0)
        return false;
    const long long total = static_cast<long long>(base) + extra_latency_samples;
    if (total > CustomNodeType::kMaxLatencySamples)
        return false;
    const int latency = static_cast<int>(total);
    type.latency_samples = [latency](double /*sample_rate*/) { return latency; };
    return true;
}

/// Applies the spec's kind, quality, factor and the sample rate to an
/// oversampler the node owns. Call it from the node's `prepare`. Returns false
/// and leaves the oversampler untouched for factor 1 or an invalid factor: the
/// node then runs at the base rate and should not call the oversampler.
template <typename SampleType>
bool configure_oversampler(signal::OversamplerT<SampleType>& oversampler,
                           const CustomNodeOversampling& spec, double sample_rate) {
    typename signal::OversamplerT<SampleType>::Factor factor{};
    switch (spec.factor) {
    case 2:
        factor = signal::OversamplerT<SampleType>::Factor::x2;
        break;
    case 4:
        factor = signal::OversamplerT<SampleType>::Factor::x4;
        break;
    case 8:
        factor = signal::OversamplerT<SampleType>::Factor::x8;
        break;
    case 16:
        factor = signal::OversamplerT<SampleType>::Factor::x16;
        break;
    default:
        return false;
    }
    using Os = signal::OversamplerT<SampleType>;
    oversampler.set_kind(static_cast<typename Os::Kind>(static_cast<int>(spec.kind)));
    oversampler.set_quality(static_cast<typename Os::Quality>(static_cast<int>(spec.quality)));
    oversampler.set_factor(factor);
    oversampler.set_sample_rate(static_cast<SampleType>(sample_rate));
    return true;
}

} // namespace pulp::host
