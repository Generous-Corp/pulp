#include <pulp/host/scan_cache.hpp>

#include <choc/text/choc_JSON.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace pulp::host {

namespace fs = std::filesystem;

namespace {

const char* format_to_string(PluginFormat f) {
    switch (f) {
        case PluginFormat::VST3:         return "vst3";
        case PluginFormat::AudioUnit:    return "au";
        case PluginFormat::AudioUnitV3:  return "auv3";
        case PluginFormat::CLAP:         return "clap";
        case PluginFormat::LV2:          return "lv2";
    }
    return "unknown";
}

std::optional<PluginFormat> format_from_string(const std::string& s) {
    if (s == "vst3") return PluginFormat::VST3;
    if (s == "au") return PluginFormat::AudioUnit;
    if (s == "auv3") return PluginFormat::AudioUnitV3;
    if (s == "clap") return PluginFormat::CLAP;
    if (s == "lv2") return PluginFormat::LV2;
    return std::nullopt;
}

struct FileStamp {
    int64_t mtime = 0;
    int64_t size = 0;
    bool ok = false;
};

FileStamp stamp_of(const std::string& path) {
    FileStamp out;
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) return out;
    auto mtime = fs::last_write_time(path, ec);
    if (ec) return out;
    out.mtime = std::chrono::duration_cast<std::chrono::seconds>(
        mtime.time_since_epoch()).count();
    out.size = static_cast<int64_t>(fs::file_size(path, ec));
    if (ec) return out;
    out.ok = true;
    return out;
}

choc::value::Value entry_to_json(const std::string& path,
                                 const ScanCacheEntry& e) {
    auto obj = choc::value::createObject("");
    obj.addMember("path", path);
    obj.addMember("mtime", static_cast<int64_t>(e.mtime));
    obj.addMember("size", static_cast<int64_t>(e.size));
    obj.addMember("name", e.info.name);
    obj.addMember("manufacturer", e.info.manufacturer);
    obj.addMember("version", e.info.version);
    obj.addMember("plugin_path", e.info.path);
    obj.addMember("unique_id", e.info.unique_id);
    obj.addMember("format", std::string(format_to_string(e.info.format)));
    obj.addMember("is_instrument", e.info.is_instrument);
    obj.addMember("is_effect", e.info.is_effect);
    obj.addMember("num_inputs", e.info.num_inputs);
    obj.addMember("num_outputs", e.info.num_outputs);

    // Richer metadata (workstream 03 slice 3.7). Emit as additive fields
    // so older schema-v1 blobs (without them) still parse via defaults
    // on the reader side.
    obj.addMember("category", e.info.category);
    obj.addMember("description", e.info.description);
    obj.addMember("has_editor", e.info.has_editor);
    obj.addMember("supports_sidechain", e.info.supports_sidechain);
    obj.addMember("supports_midi_in", e.info.supports_midi_in);
    obj.addMember("supports_midi_out", e.info.supports_midi_out);

    auto features_arr = choc::value::createEmptyArray();
    for (const auto& f : e.info.features) features_arr.addArrayElement(f);
    obj.addMember("features", features_arr);
    return obj;
}

bool entry_from_json(const choc::value::ValueView& v,
                     std::string& out_path,
                     ScanCacheEntry& out_entry) {
    if (!v.isObject()) return false;
    if (!v.hasObjectMember("path") || !v.hasObjectMember("format")) return false;
    out_path = v["path"].toString();
    out_entry.mtime = v.hasObjectMember("mtime") ? v["mtime"].getInt64() : 0;
    out_entry.size = v.hasObjectMember("size") ? v["size"].getInt64() : 0;
    out_entry.info.name = v["name"].toString();
    out_entry.info.manufacturer = v["manufacturer"].toString();
    out_entry.info.version = v["version"].toString();
    out_entry.info.path = v["plugin_path"].toString();
    out_entry.info.unique_id = v["unique_id"].toString();
    auto fmt = format_from_string(v["format"].toString());
    if (!fmt) return false;
    out_entry.info.format = *fmt;
    out_entry.info.is_instrument =
        v.hasObjectMember("is_instrument") ? v["is_instrument"].getBool() : false;
    out_entry.info.is_effect =
        v.hasObjectMember("is_effect") ? v["is_effect"].getBool() : true;
    out_entry.info.num_inputs =
        v.hasObjectMember("num_inputs") ? v["num_inputs"].getInt64() : 2;
    out_entry.info.num_outputs =
        v.hasObjectMember("num_outputs") ? v["num_outputs"].getInt64() : 2;

    // Richer metadata (workstream 03 slice 3.7). All additive; missing
    // fields preserve struct defaults for older schema-v1 blobs.
    out_entry.info.category =
        v.hasObjectMember("category") ? v["category"].toString() : "";
    out_entry.info.description =
        v.hasObjectMember("description") ? v["description"].toString() : "";
    out_entry.info.has_editor =
        v.hasObjectMember("has_editor") ? v["has_editor"].getBool() : false;
    out_entry.info.supports_sidechain =
        v.hasObjectMember("supports_sidechain")
            ? v["supports_sidechain"].getBool() : false;
    out_entry.info.supports_midi_in =
        v.hasObjectMember("supports_midi_in")
            ? v["supports_midi_in"].getBool() : false;
    out_entry.info.supports_midi_out =
        v.hasObjectMember("supports_midi_out")
            ? v["supports_midi_out"].getBool() : false;

    out_entry.info.features.clear();
    if (v.hasObjectMember("features") && v["features"].isArray()) {
        auto features = v["features"];
        for (uint32_t i = 0; i < features.size(); ++i) {
            out_entry.info.features.emplace_back(features[i].toString());
        }
    }
    return true;
}

} // namespace

std::optional<PluginInfo> HostScanCache::get(const std::string& path) const {
    auto it = entries_.find(path);
    if (it == entries_.end()) return std::nullopt;
    auto s = stamp_of(path);
    if (!s.ok) return std::nullopt;
    if (s.mtime != it->second.mtime) return std::nullopt;
    if (s.size != it->second.size) return std::nullopt;
    return it->second.info;
}

void HostScanCache::put(const std::string& path, const PluginInfo& info) {
    auto s = stamp_of(path);
    ScanCacheEntry e;
    e.info = info;
    e.mtime = s.ok ? s.mtime : 0;
    e.size = s.ok ? s.size : 0;
    entries_[path] = e;
}

void HostScanCache::erase(const std::string& path) {
    entries_.erase(path);
}

std::string HostScanCache::to_json() const {
    auto root = choc::value::createObject("");
    root.addMember("schema_version", static_cast<int64_t>(kSchemaVersion));
    auto arr = choc::value::createEmptyArray();
    for (const auto& [path, entry] : entries_) {
        arr.addArrayElement(entry_to_json(path, entry));
    }
    root.addMember("entries", arr);
    return choc::json::toString(root, /*pretty=*/true);
}

bool HostScanCache::from_json(const std::string& json) {
    try {
        auto root = choc::json::parse(json);
        if (!root.isObject()) return false;
        if (!root.hasObjectMember("schema_version")) return false;
        if (root["schema_version"].getInt64() != kSchemaVersion) return false;
        std::unordered_map<std::string, ScanCacheEntry> parsed_entries;
        if (!root.hasObjectMember("entries")) {
            entries_ = std::move(parsed_entries);
            return true;
        }
        auto arr = root["entries"];
        if (!arr.isArray()) return false;
        for (uint32_t i = 0; i < arr.size(); ++i) {
            std::string path;
            ScanCacheEntry e;
            if (entry_from_json(arr[i], path, e)) {
                parsed_entries[path] = e;
            }
        }
        entries_ = std::move(parsed_entries);
        return true;
    } catch (...) {
        return false;
    }
}

bool HostScanCache::save_to(const std::string& file_path) const {
    std::error_code ec;
    auto parent = fs::path(file_path).parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        // ignore ec — file open will fail if the directory really is missing
    }
    std::ofstream f(file_path, std::ios::binary);
    if (!f) return false;
    f << to_json();
    return static_cast<bool>(f);
}

bool HostScanCache::load_from(const std::string& file_path) {
    std::ifstream f(file_path, std::ios::binary);
    if (!f) return false;
    std::ostringstream buf;
    buf << f.rdbuf();
    return from_json(buf.str());
}

} // namespace pulp::host
