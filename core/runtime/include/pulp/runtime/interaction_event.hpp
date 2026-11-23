#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <cmath>
#include <algorithm>

namespace pulp::runtime {

// Provider-neutral phases for bounded interactions such as a press, pluck,
// drag, strum, drawing trace, or tracked-object release.
enum class InteractionPhase : std::uint8_t { invalid, press, hold, move, release, cancel };

struct InteractionEvent {
    std::uint32_t source_id = 0;
    std::uint32_t epoch = 0;
    // Sequence is monotonic within a source and epoch.
    std::uint64_t sequence = 0;
    std::uint64_t provider_time_ns = 0;
    std::uint32_t object_id = 0;
    // hold is a provider keep-alive. It does not imply a new musical attack.
    InteractionPhase phase = InteractionPhase::invalid;
    // Coordinates are finite and normalized to the provider's declared 0..1
    // surface, with origin and y direction documented by that provider.
    float x = 0.0f;
    float y = 0.0f;
    float confidence = 0.0f;
};

static_assert(std::is_trivially_copyable_v<InteractionEvent>);

/// Normalize a provider point into the declared finite surface bounds.
inline bool normalize_coordinates(float point_x, float point_y, float width, float height,
                                  float& normalized_x, float& normalized_y) noexcept {
    if (!std::isfinite(point_x) || !std::isfinite(point_y) || !std::isfinite(width) ||
        !std::isfinite(height) || width <= 0.0f || height <= 0.0f)
        return false;
    normalized_x = std::clamp(point_x / width, 0.0f, 1.0f);
    normalized_y = std::clamp(point_y / height, 0.0f, 1.0f);
    return true;
}

inline bool validate_event(const InteractionEvent& event) noexcept {
    return event.source_id != 0 && event.epoch != 0 && event.sequence != 0 &&
           event.phase != InteractionPhase::invalid && std::isfinite(event.x) &&
           std::isfinite(event.y) && std::isfinite(event.confidence) && event.x >= 0.0f &&
           event.x <= 1.0f && event.y >= 0.0f && event.y <= 1.0f && event.confidence >= 0.0f &&
           event.confidence <= 1.0f;
}

template <std::size_t Capacity = 16> struct InteractionBatch {
    static_assert(Capacity >= 1 && Capacity <= 64);
    std::array<InteractionEvent, Capacity> events{};
    std::uint8_t count = 0;

    bool append(const InteractionEvent& event) noexcept {
        if (count >= Capacity)
            return false;
        events[count++] = event;
        return true;
    }

    std::span<const InteractionEvent> events_view() const noexcept {
        const auto bounded_count = count > Capacity ? Capacity : count;
        return {events.data(), bounded_count};
    }
};

} // namespace pulp::runtime
