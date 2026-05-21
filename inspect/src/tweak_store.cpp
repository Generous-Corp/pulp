// tweak_store.cpp — Phase 0b in-memory tweak table + Phase 1 disk persistence.

#include <pulp/inspect/tweak_store.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace pulp::inspect {

namespace {

// Read the entire file at `path` into a string. Returns false if the
// file does not exist or cannot be opened — `out` is untouched.
bool read_file(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// Atomic-ish write: write `content` to `<path>.tmp`, fsync-best-effort,
// then rename over `path`. Returns an error string on failure.
std::string atomic_write(const std::string& path, const std::string& content) {
    namespace fs = std::filesystem;
    auto target = fs::path(path);
    auto tmp = target;
    tmp += ".tmp";

    // Ensure parent dir exists. Empty parent = current dir, no-op.
    std::error_code ec;
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), ec);
        // Ignore ec — the open below will fail with a clearer error
        // if the directory is genuinely unusable.
    }

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return "Failed to open " + tmp.string() + " for writing: " +
                   std::strerror(errno);
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out) {
            return "Failed while writing " + tmp.string() + ": " +
                   std::strerror(errno);
        }
        out.flush();
        // std::ofstream destructor closes; no explicit fsync from std.
    }

    fs::rename(tmp, target, ec);
    if (ec) {
        // Best-effort cleanup of the orphan .tmp so the next write
        // doesn't trip over it.
        std::error_code ignore;
        fs::remove(tmp, ignore);
        return "Failed to rename " + tmp.string() + " -> " + target.string() +
               ": " + ec.message();
    }
    return {};
}

// Try to find a project root by walking up from `start` looking for a
// directory containing `package.json`. Returns empty path on failure.
std::filesystem::path find_project_root(const std::filesystem::path& start) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto cur = fs::weakly_canonical(start, ec);
    if (ec) cur = start;
    // Walk up at most 64 levels — generous, prevents pathological
    // mounts from spinning.
    for (int i = 0; i < 64; ++i) {
        if (fs::exists(cur / "package.json", ec)) return cur;
        auto parent = cur.parent_path();
        if (parent.empty() || parent == cur) break;
        cur = parent;
    }
    return {};
}

}  // namespace

// ── Mutation ────────────────────────────────────────────────────────────

std::size_t TweakStore::apply_tweak(std::string_view anchor_id,
                                    std::string_view property_path,
                                    choc::value::Value value,
                                    std::string_view source) {
    std::size_t total = 0;
    {
        std::lock_guard lock(mtx_);
        auto& anchor_map = tweaks_[std::string(anchor_id)];
        const auto path_key = std::string(property_path);
        if (auto existing = anchor_map.find(path_key);
            existing != anchor_map.end()) {
            existing->second.value = std::move(value);
            existing->second.source = std::string(source);
        } else {
            Entry entry{std::move(value), std::string(source), next_sequence_++};
            anchor_map.emplace(path_key, std::move(entry));
        }
        for (auto& [_, m] : tweaks_) total += m.size();
    }
    maybe_auto_save_unlocked();
    return total;
}

bool TweakStore::remove_tweak(std::string_view anchor_id,
                              std::string_view property_path) {
    bool removed = false;
    {
        std::lock_guard lock(mtx_);
        auto it = tweaks_.find(std::string(anchor_id));
        if (it == tweaks_.end()) return false;
        removed = it->second.erase(std::string(property_path)) > 0;
        if (it->second.empty()) tweaks_.erase(it);
    }
    if (removed) maybe_auto_save_unlocked();
    return removed;
}

std::size_t TweakStore::remove_anchor(std::string_view anchor_id) {
    std::size_t n = 0;
    {
        std::lock_guard lock(mtx_);
        auto it = tweaks_.find(std::string(anchor_id));
        if (it == tweaks_.end()) return 0;
        n = it->second.size();
        tweaks_.erase(it);
    }
    if (n > 0) maybe_auto_save_unlocked();
    return n;
}

void TweakStore::clear() {
    {
        std::lock_guard lock(mtx_);
        decltype(tweaks_) kept_tweaks;
        decltype(bypassed_) kept_bypassed;
        for (const auto& anchor : locked_) {
            if (auto it = tweaks_.find(anchor); it != tweaks_.end())
                kept_tweaks.emplace(anchor, it->second);
            if (auto it = bypassed_.find(anchor); it != bypassed_.end())
                kept_bypassed.emplace(anchor, it->second);
        }
        tweaks_ = std::move(kept_tweaks);
        bypassed_ = std::move(kept_bypassed);
    }
    maybe_auto_save_unlocked();
}

void TweakStore::set_bypass(std::string_view anchor_id, BypassValue value) {
    {
        std::lock_guard lock(mtx_);
        // Normalize "empty bypass" (false OR empty vector) → erase the
        // overlay so is_bypassed() short-circuits cleanly.
        if (std::holds_alternative<bool>(value) && !std::get<bool>(value)) {
            bypassed_.erase(std::string(anchor_id));
        } else if (std::holds_alternative<std::vector<std::string>>(value) &&
                   std::get<std::vector<std::string>>(value).empty()) {
            bypassed_.erase(std::string(anchor_id));
        } else {
            bypassed_[std::string(anchor_id)] = std::move(value);
        }
    }
    maybe_auto_save_unlocked();
}

void TweakStore::clear_bypass(std::string_view anchor_id) {
    {
        std::lock_guard lock(mtx_);
        bypassed_.erase(std::string(anchor_id));
    }
    maybe_auto_save_unlocked();
}

void TweakStore::set_locked(std::string_view anchor_id, bool locked) {
    {
        std::lock_guard lock(mtx_);
        if (locked) {
            locked_.insert(std::string(anchor_id));
        } else {
            locked_.erase(std::string(anchor_id));
        }
    }
    maybe_auto_save_unlocked();
}

void TweakStore::clear_lock(std::string_view anchor_id) {
    {
        std::lock_guard lock(mtx_);
        locked_.erase(std::string(anchor_id));
    }
    maybe_auto_save_unlocked();
}

// ── Inspection ──────────────────────────────────────────────────────────

std::size_t TweakStore::count() const {
    std::lock_guard lock(mtx_);
    std::size_t total = 0;
    for (auto& [_, m] : tweaks_) total += m.size();
    return total;
}

std::vector<TweakStore::Record> TweakStore::list_tweaks() const {
    std::lock_guard lock(mtx_);
    struct OrderedRecord {
        std::uint64_t sequence = 0;
        const std::string* anchor = nullptr;
        const std::string* path = nullptr;
        const Entry* entry = nullptr;
    };
    std::vector<OrderedRecord> ordered;
    for (auto& [anchor, m] : tweaks_) {
        for (auto& [path, entry] : m) {
            ordered.push_back(OrderedRecord{
                entry.sequence, &anchor, &path, &entry});
        }
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const OrderedRecord& a, const OrderedRecord& b) {
                  return a.sequence < b.sequence;
              });
    std::vector<Record> out;
    out.reserve(ordered.size());
    for (const auto& rec : ordered) {
        out.push_back(Record{
            *rec.anchor, *rec.path, rec.entry->value, rec.entry->source});
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

std::vector<std::string> TweakStore::bypassed_anchors() const {
    std::lock_guard lock(mtx_);
    std::vector<std::string> out;
    out.reserve(bypassed_.size());
    for (auto& [anchor, _] : bypassed_) out.push_back(anchor);
    return out;
}

bool TweakStore::is_locked(std::string_view anchor_id) const {
    std::lock_guard lock(mtx_);
    return locked_.find(std::string(anchor_id)) != locked_.end();
}

std::vector<std::string> TweakStore::locked_anchors() const {
    std::lock_guard lock(mtx_);
    std::vector<std::string> out;
    out.reserve(locked_.size());
    for (auto& anchor : locked_) out.push_back(anchor);
    return out;
}

// ── Disk persistence (Phase 1) ──────────────────────────────────────────

std::string TweakStore::default_tweaks_path() {
    if (const char* env = std::getenv("PULP_TWEAKS_FILE"); env && *env) {
        return env;
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    auto cwd = fs::current_path(ec);
    if (ec) return "pulp-tweaks.json";

    auto project = find_project_root(cwd);
    auto root = project.empty() ? cwd : project;
    return (root / "pulp-tweaks.json").string();
}

std::string TweakStore::to_json_locked() const {
    auto obj = choc::value::createObject("");
    obj.addMember("$schema", choc::value::createString("pulp-tweaks://v1"));
    obj.addMember("version", choc::value::createInt32(kSchemaVersion));

    // tweaks: Record<anchor, Record<path, value>>
    auto tweaks_obj = choc::value::createObject("");
    auto sources_obj = choc::value::createObject("");
    bool any_source = false;
    struct OrderedEntry {
        const std::string* anchor = nullptr;
        const std::string* path = nullptr;
        const Entry* entry = nullptr;
    };
    std::vector<OrderedEntry> ordered;
    for (auto& [anchor, m] : tweaks_) {
        for (auto& [path, entry] : m) {
            ordered.push_back(OrderedEntry{&anchor, &path, &entry});
        }
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const OrderedEntry& a, const OrderedEntry& b) {
                  if (*a.anchor != *b.anchor) return *a.anchor < *b.anchor;
                  return a.entry->sequence < b.entry->sequence;
              });

    std::string current_anchor;
    bool have_current_anchor = false;
    auto anchor_obj = choc::value::createObject("");
    auto src_obj = choc::value::createObject("");
    bool any_anchor_source = false;
    auto flush_anchor = [&]() {
        if (!have_current_anchor) return;
        tweaks_obj.addMember(current_anchor, anchor_obj);
        if (any_anchor_source) {
            sources_obj.addMember(current_anchor, src_obj);
            any_source = true;
        }
    };
    for (const auto& item : ordered) {
        if (!have_current_anchor || current_anchor != *item.anchor) {
            flush_anchor();
            current_anchor = *item.anchor;
            have_current_anchor = true;
            anchor_obj = choc::value::createObject("");
            src_obj = choc::value::createObject("");
            any_anchor_source = false;
        }
        anchor_obj.addMember(*item.path, item.entry->value);
        if (!item.entry->source.empty()) {
            src_obj.addMember(
                *item.path, choc::value::createString(item.entry->source));
            any_anchor_source = true;
        }
    }
    flush_anchor();
    obj.addMember("tweaks", tweaks_obj);

    // bypassed: Record<anchor, true | string[]>
    auto bypassed_obj = choc::value::createObject("");
    for (auto& [anchor, b] : bypassed_) {
        std::visit([&](auto&& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>) {
                bypassed_obj.addMember(anchor, choc::value::createBool(v));
            } else {
                auto arr = choc::value::createEmptyArray();
                for (auto& p : v) arr.addArrayElement(choc::value::createString(p));
                bypassed_obj.addMember(anchor, arr);
            }
        }, b);
    }
    obj.addMember("bypassed", bypassed_obj);

    // locked: string[] — flat list of anchor ids the user marked as
    // protected (Phase 2.5). Only emitted when non-empty so trivial
    // files stay small and v1 readers that don't know about `locked`
    // simply never see the key. Sorted for deterministic round-trips.
    if (!locked_.empty()) {
        std::vector<std::string> sorted(locked_.begin(), locked_.end());
        std::sort(sorted.begin(), sorted.end());
        auto locked_arr = choc::value::createEmptyArray();
        for (auto& a : sorted)
            locked_arr.addArrayElement(choc::value::createString(a));
        obj.addMember("locked", locked_arr);
    }

    // sources: Record<anchor, Record<path, source>> — only when at
    // least one entry has a non-empty source tag. Keeps trivial files
    // small and matches the TS canonical schema (which doesn't define
    // `sources`) when nothing carries provenance.
    if (any_source) obj.addMember("sources", sources_obj);

    // Pretty-print so the file is reviewable in a text editor.
    return choc::json::toString(obj, true);
}

std::string TweakStore::to_json() const {
    std::lock_guard lock(mtx_);
    return to_json_locked();
}

TweakStore::DiskResult
TweakStore::from_json_locked(std::string_view json) {
    DiskResult result;
    choc::value::Value parsed;
    try {
        parsed = choc::json::parse(std::string(json));
    } catch (const std::exception& e) {
        result.error = std::string("JSON parse error: ") + e.what();
        return result;
    } catch (...) {
        result.error = "JSON parse error";
        return result;
    }
    if (!parsed.isObject()) {
        result.error = "Top-level value is not an object";
        return result;
    }

    // Schema-version guard. Missing `version` is tolerated as 1 (the
    // TS canonical TweaksFile doesn't carry an integer version yet —
    // it uses `$schema: "pulp-tweaks://v1"`). An explicit version
    // beyond what we understand is a hard error so we don't silently
    // discard fields we don't know about.
    int version = kSchemaVersion;
    if (parsed.hasObjectMember("version") && parsed["version"].isInt32()) {
        version = parsed["version"].getInt32();
    } else if (parsed.hasObjectMember("version") && parsed["version"].isInt64()) {
        version = static_cast<int>(parsed["version"].getInt64());
    }
    if (version != kSchemaVersion) {
        result.error = "Unsupported pulp-tweaks.json version: " +
                       std::to_string(version) + " (this build understands " +
                       std::to_string(kSchemaVersion) + ")";
        return result;
    }

    // Stage the new state in locals; only commit if everything parses.
    decltype(tweaks_) new_tweaks;
    decltype(bypassed_) new_bypassed;
    decltype(locked_) new_locked;
    std::uint64_t next_sequence = 0;

    if (parsed.hasObjectMember("tweaks") && parsed["tweaks"].isObject()) {
        auto tweaks_obj = parsed["tweaks"];
        for (uint32_t i = 0; i < tweaks_obj.size(); ++i) {
            auto anchor_member = tweaks_obj.getObjectMemberAt(i);
            std::string anchor(anchor_member.name);
            auto& anchor_map = new_tweaks[anchor];
            auto inner = anchor_member.value;
            if (!inner.isObject()) continue;  // skip malformed anchor
            for (uint32_t j = 0; j < inner.size(); ++j) {
                auto path_member = inner.getObjectMemberAt(j);
                Entry entry{
                    choc::value::Value(path_member.value), {}, next_sequence++};
                anchor_map[std::string(path_member.name)] = std::move(entry);
            }
            if (anchor_map.empty()) new_tweaks.erase(anchor);
        }
    }

    // sources are an additive overlay onto `new_tweaks`. Missing
    // sources OR sources without a matching tweak entry are silently
    // ignored — they can't hurt round-trip fidelity.
    if (parsed.hasObjectMember("sources") && parsed["sources"].isObject()) {
        auto sources_obj = parsed["sources"];
        for (uint32_t i = 0; i < sources_obj.size(); ++i) {
            auto anchor_member = sources_obj.getObjectMemberAt(i);
            auto it = new_tweaks.find(std::string(anchor_member.name));
            if (it == new_tweaks.end()) continue;
            auto inner = anchor_member.value;
            if (!inner.isObject()) continue;
            for (uint32_t j = 0; j < inner.size(); ++j) {
                auto path_member = inner.getObjectMemberAt(j);
                auto pit = it->second.find(std::string(path_member.name));
                if (pit == it->second.end()) continue;
                if (path_member.value.isString()) {
                    pit->second.source = std::string(path_member.value.getString());
                }
            }
        }
    }

    if (parsed.hasObjectMember("bypassed") && parsed["bypassed"].isObject()) {
        auto bypassed_obj = parsed["bypassed"];
        for (uint32_t i = 0; i < bypassed_obj.size(); ++i) {
            auto anchor_member = bypassed_obj.getObjectMemberAt(i);
            std::string anchor(anchor_member.name);
            auto v = anchor_member.value;
            if (v.isBool()) {
                if (v.getBool()) new_bypassed[anchor] = true;
                // false → don't insert (matches set_bypass normalization)
            } else if (v.isArray()) {
                std::vector<std::string> paths;
                for (uint32_t j = 0; j < v.size(); ++j) {
                    if (v[j].isString()) {
                        paths.push_back(std::string(v[j].getString()));
                    }
                }
                if (!paths.empty()) new_bypassed[anchor] = std::move(paths);
            }
        }
    }

    // locked: string[] — Phase 2.5. Missing key is fine (v1 files
    // without lock state). Non-string array elements are skipped.
    if (parsed.hasObjectMember("locked") && parsed["locked"].isArray()) {
        auto locked_arr = parsed["locked"];
        for (uint32_t i = 0; i < locked_arr.size(); ++i) {
            if (locked_arr[i].isString())
                new_locked.insert(std::string(locked_arr[i].getString()));
        }
    }

    // Existing locked anchors are protected against bulk import. A
    // missing or stale file must not erase local protected tweaks, bypass
    // overlays, or lock metadata.
    for (const auto& anchor : locked_) {
        if (auto it = tweaks_.find(anchor); it != tweaks_.end()) {
            new_tweaks[anchor] = it->second;
        } else {
            new_tweaks.erase(anchor);
        }
        if (auto it = bypassed_.find(anchor); it != bypassed_.end()) {
            new_bypassed[anchor] = it->second;
        } else {
            new_bypassed.erase(anchor);
        }
        new_locked.insert(anchor);
    }
    for (const auto& [_, m] : new_tweaks) {
        for (const auto& [__, entry] : m) {
            next_sequence = std::max(next_sequence, entry.sequence + 1);
        }
    }

    // All-or-nothing commit.
    tweaks_ = std::move(new_tweaks);
    bypassed_ = std::move(new_bypassed);
    locked_ = std::move(new_locked);
    next_sequence_ = next_sequence;

    for (auto& [_, m] : tweaks_) result.tweak_count += m.size();
    result.bypass_count = bypassed_.size();
    result.ok = true;
    return result;
}

TweakStore::DiskResult TweakStore::from_json(std::string_view json) {
    std::lock_guard lock(mtx_);
    return from_json_locked(json);
}

// ── Drift detection (Phase 2) ───────────────────────────────────────────

const char* TweakStore::drift_reason_str(DriftReason reason) {
    switch (reason) {
        case DriftReason::anchor_not_found:   return "anchor-not-found";
        case DriftReason::property_not_found: return "property-not-found";
    }
    return "unknown";
}

TweakStore::DriftReport
TweakStore::diff(const DesignSnapshot& design) const {
    DriftReport report;
    std::lock_guard lock(mtx_);
    for (auto& [anchor, m] : tweaks_) {
        const bool anchor_live = design.anchors.count(anchor) > 0;
        // properties[anchor] is the set of paths the design still
        // exposes for this anchor. Absent → anchor-only matching:
        // every property is treated as valid (no property-level drift).
        auto prop_it = design.properties.find(anchor);
        const bool have_prop_set = prop_it != design.properties.end();
        for (auto& [path, entry] : m) {
            if (!anchor_live) {
                report.orphaned.push_back(DriftedTweak{
                    anchor, path, entry.value, entry.source,
                    DriftReason::anchor_not_found});
            } else if (have_prop_set &&
                       prop_it->second.count(path) == 0) {
                report.drifted.push_back(DriftedTweak{
                    anchor, path, entry.value, entry.source,
                    DriftReason::property_not_found});
            } else {
                report.clean.push_back(
                    Record{anchor, path, entry.value, entry.source});
            }
        }
    }
    return report;
}

TweakStore::DriftReport
TweakStore::diff(const std::vector<std::string>& live_anchors) const {
    DesignSnapshot snap;
    snap.anchors.insert(live_anchors.begin(), live_anchors.end());
    return diff(snap);
}

std::vector<TweakStore::DriftedTweak>
TweakStore::find_drifted(const DesignSnapshot& design) const {
    auto report = diff(design);
    std::vector<DriftedTweak> out;
    out.reserve(report.orphaned.size() + report.drifted.size());
    // Orphans first — anchor loss is the louder failure mode.
    for (auto& d : report.orphaned) out.push_back(std::move(d));
    for (auto& d : report.drifted)  out.push_back(std::move(d));
    return out;
}

std::vector<TweakStore::DriftedTweak>
TweakStore::find_drifted(const std::vector<std::string>& live_anchors) const {
    DesignSnapshot snap;
    snap.anchors.insert(live_anchors.begin(), live_anchors.end());
    return find_drifted(snap);
}

std::string
TweakStore::drift_report_to_json(const DriftReport& report) {
    auto record_obj = [](const std::string& anchor,
                         const std::string& path,
                         const choc::value::Value& value,
                         const std::string& source) {
        auto o = choc::value::createObject("");
        o.addMember("anchorId", choc::value::createString(anchor));
        o.addMember("propertyPath", choc::value::createString(path));
        o.addMember("value", value);
        if (!source.empty())
            o.addMember("source", choc::value::createString(source));
        return o;
    };

    auto obj = choc::value::createObject("");

    auto clean_arr = choc::value::createEmptyArray();
    for (auto& r : report.clean) {
        clean_arr.addArrayElement(
            record_obj(r.anchor_id, r.property_path, r.value, r.source));
    }
    obj.addMember("clean", clean_arr);

    auto emit_drift = [&](const std::vector<DriftedTweak>& list) {
        auto arr = choc::value::createEmptyArray();
        for (auto& d : list) {
            auto o = record_obj(d.anchor_id, d.property_path,
                                d.value, d.source);
            o.addMember("reason",
                        choc::value::createString(drift_reason_str(d.reason)));
            arr.addArrayElement(o);
        }
        return arr;
    };
    obj.addMember("drifted", emit_drift(report.drifted));
    obj.addMember("orphaned", emit_drift(report.orphaned));

    auto summary = choc::value::createObject("");
    summary.addMember("total",
                      choc::value::createInt64(
                          static_cast<int64_t>(report.total())));
    summary.addMember("clean",
                      choc::value::createInt64(
                          static_cast<int64_t>(report.clean.size())));
    summary.addMember("drifted",
                      choc::value::createInt64(
                          static_cast<int64_t>(report.drifted.size())));
    summary.addMember("orphaned",
                      choc::value::createInt64(
                          static_cast<int64_t>(report.orphaned.size())));
    obj.addMember("summary", summary);

    return choc::json::toString(obj, true);
}

TweakStore::DiskResult
TweakStore::load_from_disk(std::string_view path) {
    DiskResult result;
    auto resolved = path.empty() ? default_tweaks_path() : std::string(path);
    result.path = resolved;

    std::string content;
    if (!read_file(resolved, content)) {
        result.error = "Could not read " + resolved + ": " +
                       std::strerror(errno);
        return result;
    }
    std::lock_guard lock(mtx_);
    auto inner = from_json_locked(content);
    inner.path = resolved;
    return inner;
}

TweakStore::DiskResult
TweakStore::save_locked(std::string_view path) const {
    DiskResult result;
    auto resolved = path.empty() ? default_tweaks_path() : std::string(path);
    result.path = resolved;

    auto json = to_json_locked();
    auto err = atomic_write(resolved, json);
    if (!err.empty()) {
        result.error = std::move(err);
        return result;
    }

    for (auto& [_, m] : tweaks_) result.tweak_count += m.size();
    result.bypass_count = bypassed_.size();
    result.ok = true;
    return result;
}

TweakStore::DiskResult
TweakStore::save_to_disk(std::string_view path) const {
    std::lock_guard lock(mtx_);
    return save_locked(path);
}

void TweakStore::set_auto_save(bool enabled, std::string_view path) {
    std::lock_guard lock(mtx_);
    auto_save_ = enabled;
    if (enabled) {
        auto_save_path_ = path.empty() ? default_tweaks_path() : std::string(path);
    }
    // When disabling we deliberately keep auto_save_path_ so it can be
    // re-armed without re-specifying the path.
}

bool TweakStore::auto_save_enabled() const {
    std::lock_guard lock(mtx_);
    return auto_save_;
}

std::string TweakStore::auto_save_path() const {
    std::lock_guard lock(mtx_);
    return auto_save_path_;
}

void TweakStore::maybe_auto_save_unlocked() const {
    // Capture the auto-save state and flush under one lock acquisition.
    // We don't surface errors here — auto-save is a best-effort
    // background convenience. A failed write will surface on the next
    // explicit save_to_disk() call. (The protocol-level saveTweaks
    // method also returns the error path.)
    std::lock_guard lock(mtx_);
    if (!auto_save_) return;
    (void)save_locked(auto_save_path_);
}

}  // namespace pulp::inspect
