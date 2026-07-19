#pragma once

#include <pulp/timeline/model.hpp>

#include <optional>
#include <span>
#include <vector>

namespace pulp::timeline::detail {

void append_automation_owned_ids(std::span<const AutomationLane> lanes, std::vector<ItemId>& ids);

std::optional<ModelError>
validate_attached_automation(std::span<const AutomationLane> lanes,
                             std::span<const DevicePlacement> device_chain,
                             std::span<const ItemId> other_owned_ids = {});

runtime::Result<AutomationLane, ModelError>
remap_attached_automation_lane(const AutomationLane& lane, const IdRemapTable& table);

} // namespace pulp::timeline::detail
