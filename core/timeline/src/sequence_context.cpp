#include <pulp/timeline/model.hpp>

#include "chord_scale_names.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>

namespace pulp::timeline {
namespace {

template <typename T>
runtime::Result<T, ModelError> fail(ModelErrorCode code, ItemId item = {}, ItemId related = {}) {
    return runtime::Err(ModelError{code, item, related});
}

constexpr std::uint8_t kPitchClassCount = 12;

constexpr bool valid_chord_quality(ChordQuality quality) noexcept {
    return detail::music_chord_quality(quality).has_value();
}

constexpr bool valid_scale_mode(ScaleMode mode) noexcept {
    return detail::music_scale_mode(mode).has_value();
}

constexpr bool valid_chord_voicing(ChordVoicing voicing) noexcept {
    switch (voicing) {
    case ChordVoicing::Close:
    case ChordVoicing::Open:
    case ChordVoicing::Drop2:
    case ChordVoicing::Drop3:
    case ChordVoicing::Rootless:
    case ChordVoicing::Shell:
        return true;
    }
    return false;
}

} // namespace

runtime::Result<ChordScaleLane, ModelError>
ChordScaleLane::create(std::vector<ChordScaleEvent> events) {
    for (std::size_t index = 0; index < events.size(); ++index) {
        const auto& event = events[index];
        if (event.position.value < 0 || event.chord_root >= kPitchClassCount ||
            event.scale_root >= kPitchClassCount || !valid_chord_quality(event.chord_quality) ||
            !valid_scale_mode(event.scale_mode))
            return fail<ChordScaleLane>(ModelErrorCode::InvalidChordScaleEvent);
        // An undefined extension bit would survive a round trip and mean
        // something different to the next reader that defines it, so the mask
        // is closed rather than open.
        if ((event.chord_bass && *event.chord_bass >= kPitchClassCount) ||
            (event.chord_extensions & ~kChordExtensionMask) != 0 ||
            (event.voicing && !valid_chord_voicing(*event.voicing)))
            return fail<ChordScaleLane>(ModelErrorCode::InvalidChordScaleEvent);
        // Authored order is the document's order. Sorting a caller's events
        // here would silently accept a lane whose harmony the caller did not
        // mean, so an out-of-order or duplicated position is a rejection.
        if (index != 0 && events[index - 1].position.value >= event.position.value)
            return fail<ChordScaleLane>(ModelErrorCode::UnorderedChordScaleLane);
    }
    return runtime::Result<ChordScaleLane, ModelError>(runtime::Ok(
        ChordScaleLane(std::make_shared<const std::vector<ChordScaleEvent>>(std::move(events)))));
}

const ChordScaleEvent* ChordScaleLane::at(timebase::TickPosition position) const noexcept {
    const auto found =
        std::upper_bound(events_->begin(), events_->end(), position,
                         [](timebase::TickPosition wanted, const ChordScaleEvent& event) {
                             return wanted.value < event.position.value;
                         });
    return found == events_->begin() ? nullptr : &*(found - 1);
}

bool ChordScaleLane::operator==(const ChordScaleLane& other) const noexcept {
    return events_.get() == other.events_.get() || *events_ == *other.events_;
}

namespace {

// Scale `value` by `strength` per-mille, rounding halves away from zero so a
// positive and a negative offset of the same size are attenuated by the same
// amount. Truncation would bias every groove toward zero displacement.
//
// Both call sites pass a value the template already bounded at construction (an
// offset smaller than a step, or a velocity deviation smaller than the scale
// ceiling), so negating the magnitude and multiplying by the strength stay well
// inside the signed domain.
std::int64_t scaled_by_strength(std::int64_t value, std::int32_t strength) noexcept {
    const auto magnitude = value < 0 ? -value : value;
    const auto scaled = (magnitude * strength + kGrooveUnitScale / 2) / kGrooveUnitScale;
    return value < 0 ? -scaled : scaled;
}

// Which entry of a repeating table `position` falls in. The table repeats in
// both directions, so the index floors toward negative infinity and the modulus
// is corrected into range rather than inheriting the sign of the dividend.
std::size_t groove_slot(std::int64_t position, std::int64_t step, std::size_t size) noexcept {
    const auto count = static_cast<std::int64_t>(size);
    auto index = position / step;
    if (position % step != 0 && (position < 0) != (step < 0))
        --index;
    auto slot = index % count;
    if (slot < 0)
        slot += count;
    return static_cast<std::size_t>(slot);
}

} // namespace

runtime::Result<GrooveTemplate, ModelError> GrooveTemplate::create(GrooveTemplateInput input) {
    const auto swings = input.swing_grid.value != 0;
    if (swings && !timebase::valid_swing_grid(input.swing_grid))
        return fail<GrooveTemplate>(ModelErrorCode::InvalidGrooveTemplate);
    if (!timebase::valid_swing_ratio(input.swing))
        return fail<GrooveTemplate>(ModelErrorCode::InvalidGrooveTemplate);
    // A step width and a table imply each other: a width with no entries names
    // nothing, and entries with no width have no position to be read at.
    if ((input.step.value != 0) != !input.steps.empty())
        return fail<GrooveTemplate>(ModelErrorCode::InvalidGrooveTemplate);
    if (input.step.value < 0 || input.step.value > timebase::kMaxSwingGridTicks ||
        input.steps.size() > kMaxGrooveSteps)
        return fail<GrooveTemplate>(ModelErrorCode::InvalidGrooveTemplate);
    if (input.timing_strength < 0 || input.timing_strength > kGrooveUnitScale ||
        input.velocity_strength < 0 || input.velocity_strength > kGrooveUnitScale)
        return fail<GrooveTemplate>(ModelErrorCode::InvalidGrooveTemplate);
    for (const auto& step : input.steps) {
        // An offset of a whole step or more would move material past the step
        // beyond its neighbour, which is a different table written wrong rather
        // than an extreme feel.
        const auto offset = step.timing_offset.value;
        if (offset <= -input.step.value || offset >= input.step.value)
            return fail<GrooveTemplate>(ModelErrorCode::InvalidGrooveTemplate);
        if (step.velocity_scale < 0 || step.velocity_scale > kMaxGrooveVelocityScale)
            return fail<GrooveTemplate>(ModelErrorCode::InvalidGrooveTemplate);
    }
    return runtime::Result<GrooveTemplate, ModelError>(
        runtime::Ok(GrooveTemplate(std::make_shared<const Data>(
            Data{std::move(input.name), input.swing_grid, input.swing, input.step,
                 std::move(input.steps), input.timing_strength, input.velocity_strength}))));
}

bool GrooveTemplate::states_no_feel() const noexcept {
    return data_->swing_grid.value == 0 && data_->steps.empty();
}

bool GrooveTemplate::is_canonical_default() const noexcept {
    return data_->name.empty() && data_->swing_grid.value == 0 &&
           data_->swing == timebase::kStraightSwing && data_->step.value == 0 &&
           data_->steps.empty() && data_->timing_strength == kGrooveUnitScale &&
           data_->velocity_strength == kGrooveUnitScale;
}

const GrooveStep* GrooveTemplate::step_at(timebase::TickPosition position) const noexcept {
    if (data_->steps.empty() || data_->step.value <= 0)
        return nullptr;
    return &data_->steps[groove_slot(position.value, data_->step.value, data_->steps.size())];
}

timebase::TickPosition
GrooveTemplate::apply_timing(timebase::TickPosition position) const noexcept {
    std::int64_t displacement = 0;
    if (data_->swing_grid.value != 0) {
        displacement = scaled_by_strength(
            timebase::swing_displacement(position, data_->swing_grid, data_->swing).value,
            data_->timing_strength);
    }
    // The table is indexed by the authored position, not the swung one, so a
    // change of swing setting never re-assigns material to a different step.
    const auto* step = step_at(position);
    if (step)
        displacement += scaled_by_strength(step->timing_offset.value, data_->timing_strength);
    // Swing and table offsets can oppose one another at the signed boundary.
    // Combine their bounded deltas before the one saturating position add so
    // cancellation is preserved.
    return position + timebase::TickDuration{displacement};
}

std::int32_t GrooveTemplate::velocity_scale_at(timebase::TickPosition position) const noexcept {
    const auto* step = step_at(position);
    if (!step)
        return kGrooveUnitScale;
    const auto deviation = static_cast<std::int64_t>(step->velocity_scale) -
                           static_cast<std::int64_t>(kGrooveUnitScale);
    return static_cast<std::int32_t>(kGrooveUnitScale +
                                     scaled_by_strength(deviation, data_->velocity_strength));
}

bool GrooveTemplate::operator==(const GrooveTemplate& other) const noexcept {
    return data_.get() == other.data_.get() || *data_ == *other.data_;
}

} // namespace pulp::timeline
