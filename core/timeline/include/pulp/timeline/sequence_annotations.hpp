#pragma once

#include <pulp/runtime/result.hpp>
#include <pulp/timebase/rational_time.hpp>
#include <pulp/timebase/tick.hpp>
#include <pulp/timeline/item_id.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace pulp::timeline {

enum class MarkerTypeIdError : std::uint8_t { InvalidValue };

class MarkerTypeId {
  public:
    static runtime::Result<MarkerTypeId, MarkerTypeIdError> create(std::string value);
    static MarkerTypeId cue();

    const std::string& value() const noexcept { return value_; }
    auto operator<=>(const MarkerTypeId&) const = default;

  private:
    explicit MarkerTypeId(std::string value) : value_(std::move(value)) {}
    std::string value_;
};

struct MusicalSequencePoint {
    timebase::TickPosition position;
    constexpr auto operator<=>(const MusicalSequencePoint&) const = default;
};

struct AbsoluteSequencePoint {
    timebase::SamplePosition position;
    timebase::RationalRate sample_rate;
    constexpr auto operator<=>(const AbsoluteSequencePoint&) const = default;
};

using SequencePoint = std::variant<MusicalSequencePoint, AbsoluteSequencePoint>;

struct MusicalSequenceRange {
    timebase::TickPosition start;
    timebase::TickDuration duration;
    constexpr auto operator<=>(const MusicalSequenceRange&) const = default;
};

struct AbsoluteSequenceRange {
    timebase::SamplePosition start;
    std::uint64_t sample_count = 0;
    timebase::RationalRate sample_rate;
    constexpr auto operator<=>(const AbsoluteSequenceRange&) const = default;
};

using SequenceRange = std::variant<MusicalSequenceRange, AbsoluteSequenceRange>;

struct SequenceMarker {
    ItemId id;
    MarkerTypeId type = MarkerTypeId::cue();
    std::string name;
    SequencePoint point;
    auto operator<=>(const SequenceMarker&) const = default;
};

struct SequenceRegion {
    ItemId id;
    std::string name;
    SequenceRange range;
    auto operator<=>(const SequenceRegion&) const = default;
};

} // namespace pulp::timeline
