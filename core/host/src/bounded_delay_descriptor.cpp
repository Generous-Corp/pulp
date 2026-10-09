#include <pulp/host/bounded_delay_descriptor.hpp>

#include <pulp/host/signal_graph.hpp>

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace pulp::host {
namespace {

struct DelayState {
    std::vector<float> ring;
    std::size_t cursor = 0;
};

} // namespace

BoundedDelayRefusalReason BoundedDelayDescriptor::validate() const noexcept {
    if (kind != BoundedDelayKind::Integer)
        return BoundedDelayRefusalReason::UnsupportedKind;
    if (interpolation != BoundedDelayInterpolation::None)
        return BoundedDelayRefusalReason::UnsupportedInterpolation;
    if (minimum_delay_samples > maximum_delay_samples || maximum_delay_samples > kMaxDelaySamples)
        return BoundedDelayRefusalReason::InvalidDelayBounds;
    if (delay_samples < minimum_delay_samples || delay_samples > maximum_delay_samples)
        return BoundedDelayRefusalReason::DelayOutOfBounds;
    if (tap_count != 1)
        return BoundedDelayRefusalReason::InvalidTapCount;
    if (delay_samples > std::numeric_limits<std::uint32_t>::max() / sizeof(float))
        return BoundedDelayRefusalReason::ArithmeticOverflow;
    const auto state_bytes = (static_cast<std::uint64_t>(delay_samples) + 1u) * sizeof(float);
    if (state_bytes > state_budget_bytes || state_bytes > kMaxStateBytes)
        return BoundedDelayRefusalReason::StateBudgetExceeded;
    return BoundedDelayRefusalReason::None;
}

BoundedDelayRefusalReason make_bounded_delay_registration(const BoundedDelayDescriptor& descriptor,
                                                          BoundedDelayRegistration& registration) {
    const auto refusal = descriptor.validate();
    if (refusal != BoundedDelayRefusalReason::None)
        return refusal;

    const auto delay = descriptor.delay_samples;
    CustomNodeType type;
    type.type_id = BoundedDelayDescriptor::kTypeId;
    type.version = BoundedDelayDescriptor::kVersion;
    type.num_input_ports = 1;
    type.num_output_ports = 1;
    type.default_name = "Bounded Delay";
    type.create = [delay]() -> void* {
        auto* state = new DelayState;
        state->ring.assign(static_cast<std::size_t>(delay) + 1u, 0.0f);
        return state;
    };
    type.destroy = [](void* opaque) { delete static_cast<DelayState*>(opaque); };
    type.reset = [](void* opaque) {
        auto& state = *static_cast<DelayState*>(opaque);
        std::fill(state.ring.begin(), state.ring.end(), 0.0f);
        state.cursor = 0;
    };
    type.process_instance = [delay](void* opaque, audio::BufferView<float>& output,
                                    const audio::BufferView<const float>& input, int samples) {
        auto& state = *static_cast<DelayState*>(opaque);
        auto* in = input.channel_ptr(0);
        auto* out = output.channel_ptr(0);
        for (int i = 0; i < samples; ++i) {
            const auto read = (state.cursor + state.ring.size() - delay) % state.ring.size();
            out[i] = state.ring[read];
            state.ring[state.cursor] = in[i];
            state.cursor = (state.cursor + 1) % state.ring.size();
        }
    };

    registration.custom_type = std::move(type);
    return BoundedDelayRefusalReason::None;
}

BoundedDelayRefusalReason register_bounded_delay(SignalGraph& graph,
                                                 const BoundedDelayDescriptor& descriptor) {
    BoundedDelayRegistration registration;
    const auto refusal = make_bounded_delay_registration(descriptor, registration);
    if (refusal != BoundedDelayRefusalReason::None)
        return refusal;
    return graph.register_custom_node_type(std::move(registration.custom_type))
               ? BoundedDelayRefusalReason::None
               : BoundedDelayRefusalReason::RegistrationRejected;
}

} // namespace pulp::host
