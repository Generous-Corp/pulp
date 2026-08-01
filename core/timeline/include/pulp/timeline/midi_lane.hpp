#pragma once

#include <pulp/timebase/tick.hpp>
#include <pulp/timeline/item_id.hpp>

#include <compare>
#include <cstdint>
#include <vector>

// Storage shape for the continuous controller and expression streams a MIDI
// clip carries alongside its notes.
//
// Every address field below is the MIDI wire value it names, not a Pulp-local
// enumeration of controller families: `group` and `channel` are the address a
// UMP channel-voice message carries, `status` is that message's status nibble,
// and `bank`/`index` are the controller's own bank and index numbers. A plain
// continuous controller leaves `bank` zero; a registered or assignable
// controller fills it. Keeping the wire encoding means a lane is emitted
// without a translation table, and a controller family nobody anticipated
// needs new *values*, never a new C++ type.
//
// The layout is per-stream rather than one interleaved event list, because the
// question a player asks on every seek is "what did this one stream last say
// at or before t" — answerable by binary search within a single lane, but only
// a linear scan across an interleaved list.
namespace pulp::timeline {

/** @addtogroup timeline_model
 * @{
 */

/// Largest admitted UMP group and channel address component.
inline constexpr std::uint8_t midi_lane_address_maximum = 15u;

/// Address of one continuous controller stream within a MidiContent.
///
/// Two lanes may not share an address: a stream with two lanes has two
/// authored answers for its value at a position, and no rule picks between
/// them. Which addresses carry meaning is a playback question, not a storage
/// one, so only the wire's own structural bounds are enforced here.
struct MidiLaneAddress {
    /// UMP group carrying the stream.
    std::uint8_t group = 0;
    /// Channel within the group.
    std::uint8_t channel = 0;
    /// Channel-voice status nibble identifying the controller family.
    std::uint8_t status = 0;
    /// Controller bank; zero for controllers that have none.
    std::uint8_t bank = 0;
    /// Controller index within its bank.
    std::uint8_t index = 0;

    constexpr auto operator<=>(const MidiLaneAddress&) const = default;
};

/// Returns whether every address component fits the width the wire gives it.
constexpr bool midi_lane_address_well_formed(const MidiLaneAddress& address) noexcept {
    return address.group <= midi_lane_address_maximum &&
           address.channel <= midi_lane_address_maximum &&
           address.status <= midi_lane_address_maximum;
}

/// One authored value on a controller lane, positioned in canonical ticks.
///
/// The value is the full 32-bit channel-voice data width, which is what a
/// 7-bit or 14-bit MIDI 1.0 controller value scales into, so a lane never
/// needs to record which resolution it was authored at.
struct MidiLanePoint {
    ItemId id;
    timebase::TickPosition position;
    std::uint32_t value = 0;

    constexpr auto operator<=>(const MidiLanePoint&) const = default;
};

/// One controller stream's authored points, ordered by `(position, id)`.
///
/// A player seeking to `t` finds the sounding value by binary-searching
/// `points` for the last entry at or before `t`; nothing else in the lane has
/// to be read, and nothing outside it can change the answer.
struct MidiExpressionLane {
    ItemId id;
    MidiLaneAddress address;
    std::vector<MidiLanePoint> points;

    auto operator<=>(const MidiExpressionLane&) const = default;
};

/// @}

} // namespace pulp::timeline
