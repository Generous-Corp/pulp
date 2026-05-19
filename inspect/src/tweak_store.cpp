// tweak_store.cpp — Phase 0b in-memory tweak table.

#include <pulp/inspect/tweak_store.hpp>

#include <algorithm>

namespace pulp::inspect {

std::size_t TweakStore::apply_tweak(std::string_view anchor_id,
                                    std::string_view property_path,
                                    choc::value::Value value,
                                    std::string_view source) {
    std::lock_guard lock(mtx_);
    auto& anchor_map = tweaks_[std::string(anchor_id)];
    Entry entry{std::move(value), std::string(source)};
    anchor_map[std::string(property_path)] = std::move(entry);

    std::size_t total = 0;
    for (auto& [_, m] : tweaks_) total += m.size();
    return total;
}

bool TweakStore::remove_tweak(std::string_view anchor_id,
                              std::string_view property_path) {
    std::lock_guard lock(mtx_);
    auto it = tweaks_.find(std::string(anchor_id));
    if (it == tweaks_.end()) return false;
    auto removed = it->second.erase(std::string(property_path));
    if (it->second.empty()) tweaks_.erase(it);
    return removed > 0;
}

std::size_t TweakStore::remove_anchor(std::string_view anchor_id) {
    std::lock_guard lock(mtx_);
    auto it = tweaks_.find(std::string(anchor_id));
    if (it == tweaks_.end()) return 0;
    auto n = it->second.size();
    tweaks_.erase(it);
    return n;
}

void TweakStore::clear() {
    std::lock_guard lock(mtx_);
    tweaks_.clear();
    bypassed_.clear();
}

void TweakStore::set_bypass(std::string_view anchor_id, BypassValue value) {
    std::lock_guard lock(mtx_);
    // Normalize "empty bypass" (false OR empty vector) → erase the
    // overlay so is_bypassed() short-circuits cleanly.
    if (std::holds_alternative<bool>(value) && !std::get<bool>(value)) {
        bypassed_.erase(std::string(anchor_id));
        return;
    }
    if (std::holds_alternative<std::vector<std::string>>(value) &&
        std::get<std::vector<std::string>>(value).empty()) {
        bypassed_.erase(std::string(anchor_id));
        return;
    }
    bypassed_[std::string(anchor_id)] = std::move(value);
}

void TweakStore::clear_bypass(std::string_view anchor_id) {
    std::lock_guard lock(mtx_);
    bypassed_.erase(std::string(anchor_id));
}

std::size_t TweakStore::count() const {
    std::lock_guard lock(mtx_);
    std::size_t total = 0;
    for (auto& [_, m] : tweaks_) total += m.size();
    return total;
}

std::vector<TweakStore::Record> TweakStore::list_tweaks() const {
    std::lock_guard lock(mtx_);
    std::vector<Record> out;
    for (auto& [anchor, m] : tweaks_) {
        for (auto& [path, entry] : m) {
            out.push_back(Record{anchor, path, entry.value, entry.source});
        }
    }
    return out;
}

std::optional<choc::value::Value>
TweakStore::lookup(std::string_view anchor_id,
                   std::string_view property_path) const {
    std::lock_guard lock(mtx_);
    auto it = tweaks_.find(std::string(anchor_id));
    if (it == tweaks_.end()) return std::nullopt;
    auto pit = it->second.find(std::string(property_path));
    if (pit == it->second.end()) return std::nullopt;
    return pit->second.value;
}

bool TweakStore::is_bypassed(std::string_view anchor_id,
                             std::string_view property_path) const {
    std::lock_guard lock(mtx_);
    auto it = bypassed_.find(std::string(anchor_id));
    if (it == bypassed_.end()) return false;
    return std::visit([&](auto&& v) -> bool {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, bool>) {
            return v;  // true = bypass all paths under this anchor
        } else {
            // Path-list — only bypassed if property_path is in the list.
            auto path_str = std::string(property_path);
            return std::find(v.begin(), v.end(), path_str) != v.end();
        }
    }, it->second);
}

std::optional<TweakStore::BypassValue>
TweakStore::bypass_for(std::string_view anchor_id) const {
    std::lock_guard lock(mtx_);
    auto it = bypassed_.find(std::string(anchor_id));
    if (it == bypassed_.end()) return std::nullopt;
    return it->second;
}

}  // namespace pulp::inspect
