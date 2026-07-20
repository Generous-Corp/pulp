#pragma once

#include <cstdint>
#include <limits>

namespace pulp::format {

/// Independently optional fields in a per-block host transport snapshot.
enum class TransportField : std::uint8_t {
    Playing = 0,
    Recording,
    Looping,
    Tempo,
    BeatPosition,
    SamplePosition,
    TimeSignature,
    LoopRange,
    Bar,
    HostTime,
    FrameRate,
    Count,
};

/// Compact validity mask for the transport values carried by ProcessContext.
///
/// A field can contain a useful default while still being unavailable. In
/// particular, zero is a valid beat, sample, bar, loop, and host-time value.
/// Consumers should query this mask instead of interpreting value sentinels.
class TransportValidity {
public:
    using storage_type = std::uint16_t;

    constexpr bool has(TransportField field) const noexcept {
        return (bits_ & mask(field)) != 0;
    }

    constexpr void set(TransportField field, bool valid = true) noexcept {
        if (valid) {
            bits_ = static_cast<storage_type>(bits_ | mask(field));
        } else {
            bits_ = static_cast<storage_type>(bits_ & ~mask(field));
        }
    }

    constexpr bool empty() const noexcept { return bits_ == 0; }

    friend constexpr bool operator==(const TransportValidity&,
                                     const TransportValidity&) noexcept = default;

private:
    static constexpr storage_type mask(TransportField field) noexcept {
        return static_cast<storage_type>(
            storage_type{1} << static_cast<std::uint8_t>(field));
    }

    storage_type bits_ = 0;
};

static_assert(sizeof(TransportValidity) == sizeof(TransportValidity::storage_type));
static_assert(static_cast<std::uint8_t>(TransportField::Count) <=
              std::numeric_limits<TransportValidity::storage_type>::digits);

} // namespace pulp::format
