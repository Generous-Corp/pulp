#pragma once

#include <pulp/state/store.hpp>

#include <algorithm>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pulp::format {

/// Validated, registration-ordered projection of StateStore parameter groups.
/// Invalid groups and every descendant that depends on one are omitted.
class ParamGroupProjection {
  public:
    struct Entry {
        int id = 0;
        int parent_id = 0;
        std::string name;
        std::string path;
    };

    explicit ParamGroupProjection(std::span<const state::ParamGroup> groups) {
        std::unordered_map<int, const state::ParamGroup*> by_id;
        std::unordered_set<int> duplicate_ids;
        by_id.reserve(groups.size());
        for (const auto& group : groups) {
            if (group.id <= 0 || group.name.empty())
                continue;
            if (!by_id.emplace(group.id, &group).second)
                duplicate_ids.insert(group.id);
        }

        enum class Mark { visiting, valid, invalid };
        std::unordered_map<int, Mark> marks;
        std::unordered_map<int, std::string> paths;
        marks.reserve(by_id.size());
        paths.reserve(by_id.size());

        const auto validate = [&](const auto& self, int id) -> bool {
            if (duplicate_ids.contains(id))
                return false;
            if (const auto mark = marks.find(id); mark != marks.end()) {
                if (mark->second == Mark::valid)
                    return true;
                return false;
            }
            const auto found = by_id.find(id);
            if (found == by_id.end())
                return false;
            marks[id] = Mark::visiting;
            const auto& group = *found->second;
            std::string path;
            if (group.parent_id == 0) {
                path = group.name;
            } else {
                const auto parent_mark = marks.find(group.parent_id);
                if ((parent_mark != marks.end() && parent_mark->second == Mark::visiting) ||
                    !self(self, group.parent_id)) {
                    marks[id] = Mark::invalid;
                    return false;
                }
                path = paths[group.parent_id] + "/" + group.name;
            }
            paths[id] = std::move(path);
            marks[id] = Mark::valid;
            return true;
        };

        std::unordered_set<int> emitted;
        entries_.reserve(groups.size());
        emitted.reserve(groups.size());
        // Keep registration order among siblings while ensuring every parent
        // precedes its children in host flat-list enumeration.
        bool made_progress = true;
        while (made_progress) {
            made_progress = false;
            for (const auto& group : groups) {
                if (!validate(validate, group.id))
                    continue;
                if (emitted.contains(group.id))
                    continue;
                if (group.parent_id != 0 && !emitted.contains(group.parent_id))
                    continue;
                entries_.push_back({group.id, group.parent_id, group.name, paths[group.id]});
                emitted.insert(group.id);
                made_progress = true;
            }
        }
    }

    std::span<const Entry> entries() const noexcept {
        return entries_;
    }

    const Entry* find(int id) const noexcept {
        const auto found = std::find_if(entries_.begin(), entries_.end(),
                                        [id](const Entry& entry) { return entry.id == id; });
        return found == entries_.end() ? nullptr : &*found;
    }

  private:
    std::vector<Entry> entries_;
};

} // namespace pulp::format
