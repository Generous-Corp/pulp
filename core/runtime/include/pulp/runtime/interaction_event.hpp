#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace pulp::runtime {

// Provider-neutral phases for bounded interactions such as a press, pluck,
// drag, strum, drawing trace, or tracked-object release.
enum class InteractionPhase : std::uint8_t { press, hold, move, release, cancel };

struct InteractionEvent {
    std::uint32_t source_id = 0;
    std::uint32_t epoch = 0;
    std::uint64_t sequence = 0;
    std::uint32_t object_id = 0;
    InteractionPhase phase = InteractionPhase::cancel;
    float x = 0.0f;
    float y = 0.0f;
    float velocity = 0.0f;
    float confidence = 0.0f;
};

static_assert(std::is_trivially_copyable_v<InteractionEvent>);

template <std::size_t Capacity = 16> struct InteractionBatch {
    static_assert(Capacity >= 1 && Capacity <= 64);
    std::array<InteractionEvent, Capacity> events{};
    std::size_t count = 0;

    bool append(const InteractionEvent& event) noexcept {
        if (count == Capacity) return false;
        events[count++] = event;
        return true;
    }
};

} // namespace pulp::runtime
