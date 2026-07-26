#pragma once

#include <pulp/timeline/model.hpp>

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace pulp::timeline::detail {

runtime::Result<std::shared_ptr<const LauncherStore>, ModelError>
build_launcher(std::vector<Scene> scenes, std::span<const Track> tracks);
runtime::Result<std::shared_ptr<const LauncherStore>, ModelError>
insert_scene_store(const std::shared_ptr<const LauncherStore>& source, Scene scene,
                   std::optional<ItemId> before, std::span<const Track> tracks);
runtime::Result<std::shared_ptr<const LauncherStore>, ModelError>
erase_scene_store(const std::shared_ptr<const LauncherStore>& source, ItemId id);
runtime::Result<std::shared_ptr<const LauncherStore>, ModelError>
insert_slot_store(const std::shared_ptr<const LauncherStore>& source, ItemId scene_id, Slot slot,
                  std::optional<ItemId> before, std::span<const Track> tracks);
runtime::Result<std::shared_ptr<const LauncherStore>, ModelError>
erase_slot_store(const std::shared_ptr<const LauncherStore>& source, ItemId scene_id,
                 ItemId slot_id);

std::size_t launcher_scene_count(const LauncherStore& store) noexcept;
ItemId launcher_head(const LauncherStore& store) noexcept;
const Scene* launcher_find_scene(const LauncherStore& store, ItemId id) noexcept;
ItemId launcher_next_scene(const LauncherStore& store, ItemId id) noexcept;
const Slot* launcher_find_slot(const LauncherStore& store, ItemId id) noexcept;
std::optional<ItemId> launcher_clip_source(const LauncherStore& store, ItemId clip_id) noexcept;
std::size_t shared_launcher_nodes(const LauncherStore& lhs, const LauncherStore& rhs);
LauncherIndexStats launcher_index_stats() noexcept;

} // namespace pulp::timeline::detail
