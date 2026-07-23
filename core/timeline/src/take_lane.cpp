#include <pulp/timeline/model.hpp>

#include <algorithm>
#include <limits>
#include <optional>

namespace pulp::timeline {
namespace {

template <typename T>
runtime::Result<T, ModelError> fail(ModelErrorCode code, ItemId item = {},
                                    ItemId related = {}) {
    return runtime::Err(ModelError{code, item, related});
}

runtime::Result<std::vector<TakeCompSegment>, ModelError>
canonical_comp(ItemId lane_id, std::span<const Take> takes,
               std::vector<TakeCompSegment> comp) {
    std::optional<timebase::RationalRate> comp_rate;
    for (auto& segment : comp) {
        const auto found = std::lower_bound(
            takes.begin(), takes.end(), segment.take_id,
            [](const Take& candidate, ItemId wanted) { return candidate.id() < wanted; });
        const auto* take =
            found != takes.end() && found->id() == segment.take_id ? &*found : nullptr;
        const auto normalized = segment.range.sample_rate.normalized();
        if (!take || segment.range.start.value < take->placement_start().value ||
            segment.range.sample_count == 0 || normalized != segment.range.sample_rate ||
            normalized != take->sample_rate().normalized() ||
            (comp_rate && normalized != *comp_rate))
            return fail<std::vector<TakeCompSegment>>(ModelErrorCode::InvalidTakeComp, lane_id,
                                                      segment.take_id);
        comp_rate = normalized;
        const auto representable_tail = static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max() - segment.range.start.value);
        if (segment.range.sample_count > representable_tail)
            return fail<std::vector<TakeCompSegment>>(ModelErrorCode::InvalidTakeComp, lane_id,
                                                      segment.take_id);
        const auto offset = static_cast<std::uint64_t>(
            segment.range.start.value - take->placement_start().value);
        if (offset > take->media().frame_count ||
            segment.range.sample_count > take->media().frame_count - offset)
            return fail<std::vector<TakeCompSegment>>(ModelErrorCode::InvalidTakeComp, lane_id,
                                                      segment.take_id);
    }
    std::sort(comp.begin(), comp.end(), [](const auto& lhs, const auto& rhs) {
        return std::pair(lhs.range.start.value, lhs.take_id.value) <
               std::pair(rhs.range.start.value, rhs.take_id.value);
    });
    for (std::size_t index = 1; index < comp.size(); ++index) {
        const auto& previous = comp[index - 1];
        const auto& current = comp[index];
        const auto distance = static_cast<std::uint64_t>(current.range.start.value -
                                                         previous.range.start.value);
        if (previous.range.sample_count > distance)
            return fail<std::vector<TakeCompSegment>>(ModelErrorCode::OverlappingTakeComp,
                                                      lane_id, current.take_id);
    }
    return runtime::Ok(std::move(comp));
}

} // namespace

runtime::Result<Take, ModelError> Take::create(ItemId id, MediaRef media,
                                               timebase::SamplePosition placement_start,
                                               timebase::RationalRate sample_rate) {
    if (!id.valid())
        return fail<Take>(ModelErrorCode::InvalidItemId, id);
    if (!media.asset_id.valid())
        return fail<Take>(ModelErrorCode::InvalidTake, id, media.asset_id);
    if (media.frame_count == 0 || placement_start.value < 0 || !sample_rate.valid())
        return fail<Take>(ModelErrorCode::InvalidTake, id);
    return runtime::Ok(Take(id, media, placement_start, sample_rate));
}

struct TakeLane::Data {
    ItemId id;
    std::string name;
    std::vector<Take> takes;
    std::vector<TakeCompSegment> comp;
};

runtime::Result<TakeLane, ModelError> TakeLane::create(ItemId id, std::string name,
                                                       std::vector<Take> takes,
                                                       std::vector<TakeCompSegment> comp) {
    if (!id.valid())
        return fail<TakeLane>(ModelErrorCode::InvalidItemId, id);
    for (const auto& take : takes)
        if (!take.id().valid())
            return fail<TakeLane>(ModelErrorCode::InvalidTake, take.id());
    std::sort(takes.begin(), takes.end(),
              [](const Take& lhs, const Take& rhs) { return lhs.id() < rhs.id(); });
    if (const auto duplicate = std::adjacent_find(
            takes.begin(), takes.end(),
            [](const Take& lhs, const Take& rhs) { return lhs.id() == rhs.id(); });
        duplicate != takes.end())
        return fail<TakeLane>(ModelErrorCode::DuplicateTake, duplicate->id());
    auto canonical = canonical_comp(id, takes, std::move(comp));
    if (!canonical)
        return runtime::Err(canonical.error());
    return runtime::Ok(TakeLane(std::make_shared<const Data>(
        Data{.id = id,
             .name = std::move(name),
             .takes = std::move(takes),
             .comp = std::move(canonical).value()})));
}

ItemId TakeLane::id() const noexcept {
    return data_->id;
}

const std::string& TakeLane::name() const noexcept {
    return data_->name;
}

std::span<const Take> TakeLane::takes() const noexcept {
    return data_->takes;
}

std::span<const TakeCompSegment> TakeLane::comp_segments() const noexcept {
    return data_->comp;
}

const Take* TakeLane::find_take(ItemId id) const noexcept {
    const auto found =
        std::lower_bound(data_->takes.begin(), data_->takes.end(), id,
                         [](const Take& take, ItemId wanted) { return take.id() < wanted; });
    return found != data_->takes.end() && found->id() == id ? &*found : nullptr;
}

runtime::Result<TakeLane, ModelError> TakeLane::insert_take(Take take) const {
    auto takes = data_->takes;
    const auto found = std::lower_bound(
        takes.begin(), takes.end(), take.id(),
        [](const Take& candidate, ItemId wanted) { return candidate.id() < wanted; });
    if (found != takes.end() && found->id() == take.id())
        return fail<TakeLane>(ModelErrorCode::DuplicateTake, take.id());
    takes.insert(found, std::move(take));
    return runtime::Ok(TakeLane(std::make_shared<const Data>(
        Data{.id = data_->id,
             .name = data_->name,
             .takes = std::move(takes),
             .comp = data_->comp})));
}

runtime::Result<TakeLane, ModelError> TakeLane::erase_take(ItemId id) const {
    if (std::any_of(data_->comp.begin(), data_->comp.end(),
                    [id](const auto& segment) { return segment.take_id == id; }))
        return fail<TakeLane>(ModelErrorCode::ActiveCompTakeRemoval, id, data_->id);
    auto takes = data_->takes;
    const auto found =
        std::lower_bound(takes.begin(), takes.end(), id, [](const Take& candidate, ItemId wanted) {
            return candidate.id() < wanted;
        });
    if (found == takes.end() || found->id() != id)
        return fail<TakeLane>(ModelErrorCode::MissingItem, id, data_->id);
    takes.erase(found);
    return runtime::Ok(TakeLane(std::make_shared<const Data>(
        Data{.id = data_->id,
             .name = data_->name,
             .takes = std::move(takes),
             .comp = data_->comp})));
}

runtime::Result<TakeLane, ModelError>
TakeLane::with_comp_segments(std::vector<TakeCompSegment> comp) const {
    auto canonical = canonical_comp(data_->id, data_->takes, std::move(comp));
    if (!canonical)
        return runtime::Err(canonical.error());
    return runtime::Ok(TakeLane(std::make_shared<const Data>(
        Data{.id = data_->id,
             .name = data_->name,
             .takes = data_->takes,
             .comp = std::move(canonical).value()})));
}

} // namespace pulp::timeline
