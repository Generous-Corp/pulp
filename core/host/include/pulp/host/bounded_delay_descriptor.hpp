#pragma once

#include <pulp/host/custom_node_type.hpp>

#include <cstdint>
#include <string>

namespace pulp::host {

class SignalGraph;

enum class BoundedDelayKind : std::uint8_t {
    Integer,
    Fractional,
    MultiTap,
};

enum class BoundedDelayInterpolation : std::uint8_t {
    None,
    Linear,
    Lagrange3,
    Lagrange5,
    Thiran1,
    Hermite4,
};

enum class BoundedDelayRefusalReason : std::uint8_t {
    None,
    UnsupportedKind,
    UnsupportedInterpolation,
    InvalidDelayBounds,
    DelayOutOfBounds,
    InvalidTapCount,
    StateBudgetExceeded,
    ArithmeticOverflow,
    RegistrationRejected,
};

struct BoundedDelayDescriptor {
    static constexpr const char* kTypeId = "pulp.dspx02.bounded-delay";
    static constexpr int kVersion = 1;
    static constexpr std::uint32_t kMaxDelaySamples = 65535;
    static constexpr std::uint32_t kMaxStateBytes = 1u << 20;

    BoundedDelayKind kind = BoundedDelayKind::Integer;
    BoundedDelayInterpolation interpolation = BoundedDelayInterpolation::None;
    std::uint32_t delay_samples = 1;
    std::uint32_t minimum_delay_samples = 0;
    std::uint32_t maximum_delay_samples = kMaxDelaySamples;
    std::uint32_t tap_count = 1;
    std::uint32_t state_budget_bytes = kMaxStateBytes;

    BoundedDelayRefusalReason validate() const noexcept;
    bool is_supported() const noexcept {
        return validate() == BoundedDelayRefusalReason::None;
    }
};

struct BoundedDelayRegistration {
    CustomNodeType custom_type;
};

// Builds the graph custom-node identity. The descriptor is validated before
// any callbacks or state are allocated.
BoundedDelayRefusalReason make_bounded_delay_registration(const BoundedDelayDescriptor& descriptor,
                                                          BoundedDelayRegistration& registration);

// Registers the canonical graph descriptor and makes it reachable through
// SignalGraph::add_custom_node(kTypeId, kVersion).
BoundedDelayRefusalReason register_bounded_delay(SignalGraph& graph,
                                                 const BoundedDelayDescriptor& descriptor);

} // namespace pulp::host
