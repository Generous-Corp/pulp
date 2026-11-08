#include <cctype>
#include <pulp/state/preset_manager.hpp>
#include <pulp/platform/special_dir.hpp>
#include <pulp/runtime/system.hpp>
#include <choc/text/choc_JSON.h>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <sstream>
#include <algorithm>
#include <string_view>
#include <utility>

namespace fs = std::filesystem;

namespace pulp::state {

namespace {

// A preset name or folder is a single path component chosen in the UI. A
// separator, a parent reference, an embedded NUL, or (on Windows) a drive
// colon would resolve outside the presets directory.
// Windows reserves device names (CON, NUL, COM1, ...) with or without an
// extension; a preset so named cannot be written there, so refuse it everywhere.
bool is_windows_device_name(std::string_view value) {
    std::string stem(value.substr(0, value.find('.')));
    for (char& c : stem)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL")
        return true;
    if (stem.size() == 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0) &&
        stem[3] >= '1' && stem[3] <= '9')
        return true;
    return false;
}

bool is_safe_path_component(std::string_view value) {
    if (value.empty() || value == "." || value == ".." || is_windows_device_name(value))
        return false;
    for (char c : value) {
        if (c == '/' || c == '\0')
            return false;
#ifdef _WIN32
        // Backslash and the drive colon are separators only on Windows; a
        // POSIX preset name may legitimately contain either.
        if (c == '\\' || c == ':')
            return false;
#endif
    }
    return true;
}

// A folder may nest ("Keys/Analog"); each component must be safe and the
// path must stay relative, so no leading separator or empty segment.
bool is_safe_relative_folder(std::string_view folder) {
    if (folder.empty())
        return true;
    if (folder.front() == '/' || folder.front() == '\\')
        return false;
    std::size_t start = 0;
    while (start <= folder.size()) {
        std::size_t end = folder.find('/', start);
        if (end == std::string_view::npos)
            end = folder.size();
        if (!is_safe_path_component(folder.substr(start, end - start)))
            return false;
        start = end + 1;
    }
    return true;
}

bool has_only_trailing_space(const std::string& value, std::size_t pos) {
    return value.find_first_not_of(" \t\r\n", pos) == std::string::npos;
}

bool contains_string(const std::vector<std::string>& values, std::string_view needle) {
    return std::find(values.begin(), values.end(), needle) != values.end();
}

#ifdef _WIN32
std::string sanitize_windows_path_component(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        const bool invalid = c < 32 || c == '<' || c == '>' || c == ':' ||
                             c == '"' || c == '/' || c == '\\' || c == '|' ||
                             c == '?' || c == '*';
        out.push_back(invalid ? '_' : static_cast<char>(c));
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        out.pop_back();
    }
    return out.empty() ? "_" : out;
}
#endif

} // namespace

// ── Platform paths ───────────────────────────────────────────────────────

fs::path PresetManager::platform_presets_root() {
    return pulp::platform::special_dir(pulp::platform::SpecialDir::user_presets);
}

PresetManager::PresetManager(StateStore& store, const std::string& manufacturer,
                             const std::string& plugin_name)
    : store_(store), manufacturer_(manufacturer), plugin_name_(plugin_name)
{
    auto root = platform_presets_root();
    if (!root.empty()) {
#ifdef _WIN32
        user_dir_ = root / sanitize_windows_path_component(manufacturer) /
                    sanitize_windows_path_component(plugin_name) / "Presets";
#else
        user_dir_ = root / manufacturer / plugin_name;
#endif
    }

    // Factory presets live inside the plug-in bundle, whose layout is
    // format- and platform-specific and which a headless host does not have at
    // all — so this class cannot resolve it. It stays empty (factory_presets()
    // then finds nothing) until a caller supplies it through
    // set_factory_presets_dir(); the AU adapters derive it from the loaded
    // binary in the format adapter's factory-preset table implementation.
    factory_dir_.clear();
}

void PresetManager::ensure_user_dir() {
    if (!user_dir_.empty() && !fs::exists(user_dir_)) {
        std::error_code ec;
        fs::create_directories(user_dir_, ec);
    }
}

// ── Save/Load ────────────────────────────────────────────────────────────

bool PresetManager::save(const std::string& name, const std::string& folder) {
    return save(name, folder, PresetMetadata{});
}

bool PresetManager::save(const std::string& name, const std::string& folder,
                         const PresetMetadata& metadata) {
    if (!is_safe_path_component(name) || !is_safe_relative_folder(folder)) {
        return false;
    }
    ensure_user_dir();
    if (user_dir_.empty()) return false;

    auto dir = folder.empty() ? user_dir_ : user_dir_ / folder;
    if (!fs::exists(dir)) {
        std::error_code ec;
        fs::create_directories(dir, ec);
    }

    auto path = dir / (name + ".json");

    // Serialize the preset into a buffer first so the file can be written
    // atomically below. Metadata strings are emitted through choc's JSON string
    // escaper so a name / manufacturer / plugin / parameter name containing a
    // quote, backslash, or control character can never produce a corrupt
    // (unparseable) file.
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": 2,\n";
    out << "  \"name\": " << choc::json::getEscapedQuotedString(name) << ",\n";
    out << "  \"manufacturer\": " << choc::json::getEscapedQuotedString(manufacturer_) << ",\n";
    out << "  \"plugin\": " << choc::json::getEscapedQuotedString(plugin_name_) << ",\n";
    out << "  \"version\": " << store_.state_version() << ",\n";
    if (!metadata.author.empty())
        out << "  \"author\": " << choc::json::getEscapedQuotedString(metadata.author) << ",\n";
    if (!metadata.description.empty())
        out << "  \"description\": " << choc::json::getEscapedQuotedString(metadata.description)
            << ",\n";
    if (!metadata.tags.empty()) {
        out << "  \"tags\": [";
        for (std::size_t i = 0; i < metadata.tags.size(); ++i) {
            if (i > 0)
                out << ", ";
            out << choc::json::getEscapedQuotedString(metadata.tags[i]);
        }
        out << "],\n";
    }
    if (metadata.program && *metadata.program >= 0)
        out << "  \"program\": " << *metadata.program << ",\n";
    out << "  \"parameters\": {";

    auto params = store_.all_params();
    for (size_t i = 0; i < params.size(); ++i) {
        if (i > 0) out << ",";
        // Emit a finite number always: a non-finite param value would stream as
        // the bare token `nan`/`inf`, producing an unparseable preset file.
        // std::clamp turns ±inf into a finite range bound but returns NaN
        // unchanged, so a NaN set_value (misbehaving setter / corrupt restore)
        // survives into get_value() here. Substitute the parameter's registered
        // default (the plugin's intended safe value, not a blanket 0 which could
        // be out-of-range / clamp to the range min on reload); guard the default
        // itself too. The isfinite check also covers any ±inf that reaches here
        // via a path that bypassed clamping.
        float value = store_.get_value(params[i].id);
        if (!std::isfinite(value)) {
            const float def = params[i].range.default_value;
            value = std::isfinite(def) ? def : 0.0f;
        }
        out << "\n    " << choc::json::getEscapedQuotedString(params[i].name) << ": " << value;
    }
    out << "\n  }\n";
    out << "}\n";
    const std::string body = out.str();

    // Atomic write: stage to a sibling temp file, fully flush it, then rename it
    // onto the destination. A crash / power loss / full disk mid-write therefore
    // leaves any previously-saved preset intact instead of truncating it.
    // Mirrors PropertiesFile::save_to and the skp_capture / waveform_overview_cache
    // temp-then-rename convention.
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp);
        if (!f.is_open()) return false;
        f << body;
        f.flush();
        if (!f.good()) {
            f.close();
            std::error_code ec;
            fs::remove(tmp, ec);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        std::error_code rm_ec;
        fs::remove(tmp, rm_ec);
        return false;
    }

    current_name_ = name;
    unsaved_changes_ = false;
    cache_valid_ = false;

    if (on_list_changed) on_list_changed();
    return true;
}

bool PresetManager::load(const PresetInfo& preset) {
    return load(preset.path);
}

namespace {

PresetMetadata metadata_from_document(const choc::value::ValueView& doc) {
    PresetMetadata meta;
    if (!doc.isObject())
        return meta;
    auto text = [&](const char* key) -> std::string {
        if (!doc.hasObjectMember(key))
            return {};
        auto v = doc[key];
        return v.isString() ? std::string(v.getString()) : std::string{};
    };
    meta.author = text("author");
    meta.description = text("description");
    if (doc.hasObjectMember("tags")) {
        auto tags = doc["tags"];
        if (tags.isArray()) {
            for (std::uint32_t i = 0; i < tags.size(); ++i) {
                auto t = tags[i];
                if (t.isString())
                    meta.tags.emplace_back(t.getString());
            }
        }
    }
    if (doc.hasObjectMember("program")) {
        auto v = doc["program"];
        if (v.isInt32())
            meta.program = v.getInt32();
        else if (v.isInt64())
            meta.program = static_cast<int>(v.getInt64());
    }
    return meta;
}

bool number_from_value(const choc::value::ValueView& v, double& out) {
    if (v.isInt32())
        out = v.getInt32();
    else if (v.isInt64())
        out = static_cast<double>(v.getInt64());
    else if (v.isFloat32())
        out = v.getFloat32();
    else if (v.isFloat64())
        out = v.getFloat64();
    else
        return false;
    return std::isfinite(out);
}

} // namespace

void PreparedPreset::apply_rt(StateStore& store) const {
    store.set_values_rt(writes);
}

std::optional<PreparedPreset> PresetManager::prepare(const fs::path& path) const {
    std::ifstream f(path);
    if (!f.is_open())
        return std::nullopt;

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    PreparedPreset out;
    out.path = path;
    out.name = path.stem().string();

    const auto all_params = store_.all_params();
    auto clamp_to = [&](const ParamInfo& param, double value) {
        return std::clamp(static_cast<float>(value), param.range.min, param.range.max);
    };

    // Strict JSON first: handles escaped parameter names and metadata. A file
    // that is not valid JSON (hand-edited, truncated) falls back to the legacy
    // key scan below so previously loadable files keep loading.
    bool structured = false;
    try {
        auto doc = choc::json::parse(content);
        if (doc.isObject() && doc.hasObjectMember("parameters")) {
            auto params = doc["parameters"];
            if (!params.isObject())
                return std::nullopt;
            structured = true;
            out.schema = (doc.hasObjectMember("schema") && doc["schema"].isInt64())
                             ? static_cast<int>(doc["schema"].getInt64())
                             : (doc.hasObjectMember("schema") && doc["schema"].isInt32()
                                    ? doc["schema"].getInt32()
                                    : 1);
            out.metadata = metadata_from_document(doc.getView());

            std::vector<bool> seen(all_params.size(), false);
            const auto count = params.size();
            for (std::uint32_t i = 0; i < count; ++i) {
                auto member = params.getObjectMemberAt(i);
                const std::string key(member.name);
                std::size_t index = all_params.size();
                for (std::size_t k = 0; k < all_params.size(); ++k)
                    if (all_params[k].name == key) {
                        index = k;
                        break;
                    }
                if (index == all_params.size()) {
                    out.unknown_params.push_back(key);
                    continue;
                }
                double number = 0.0;
                if (!number_from_value(member.value, number))
                    continue;
                seen[index] = true;
                out.writes.push_back({all_params[index].id, clamp_to(all_params[index], number)});
            }
            for (std::size_t k = 0; k < all_params.size(); ++k)
                if (!seen[k])
                    out.missing_params.push_back(all_params[k].name);
        } else if (doc.isObject()) {
            return std::nullopt;
        }
    } catch (...) {
        structured = false;
    }
    if (structured)
        return out;

    // Legacy raw-key scan: look for "parameters": { "name": value, ... }.
    out.writes.clear();
    auto params_pos = content.find("\"parameters\"");
    if (params_pos == std::string::npos)
        return std::nullopt;

    auto brace = content.find('{', params_pos);
    if (brace == std::string::npos)
        return std::nullopt;

    for (const auto& param : all_params) {
        auto key_pos = content.find("\"" + param.name + "\"", brace);
        if (key_pos == std::string::npos) continue;

        auto colon = content.find(':', key_pos);
        if (colon == std::string::npos) continue;

        auto val_start = colon + 1;
        while (val_start < content.size() && content[val_start] == ' ') ++val_start;

        auto val_end = content.find_first_of(",}\n", val_start);
        if (val_end == std::string::npos) val_end = content.size();

        std::string val_str = content.substr(val_start, val_end - val_start);
        try {
            std::size_t parsed = 0;
            float value = std::stof(val_str, &parsed);
            if (!has_only_trailing_space(val_str, parsed)) continue;
            out.writes.push_back({param.id, std::isfinite(value) ? clamp_to(param, value) : value});
        } catch (...) {
            continue;
        }
    }
    return out;
}

bool PresetManager::apply(const PreparedPreset& prepared) {
    for (const auto& write : prepared.writes)
        store_.set_value(write.id, write.value);

    current_name_ = prepared.name;
    unsaved_changes_ = false;

    PresetInfo info;
    info.name = current_name_;
    info.path = prepared.path;
    info.author = prepared.metadata.author;
    info.description = prepared.metadata.description;
    info.tags = prepared.metadata.tags;
    if (on_preset_loaded) on_preset_loaded(info);
    return true;
}

std::optional<std::vector<ParamSetting>> PresetManager::read_values(const fs::path& path) const {
    auto prepared = prepare(path);
    if (!prepared)
        return std::nullopt;
    std::vector<ParamSetting> values;
    values.reserve(prepared->writes.size());
    for (const auto& write : prepared->writes)
        values.push_back({write.id, write.value});
    return values;
}

bool PresetManager::load(const fs::path& path) {
    auto prepared = prepare(path);
    if (!prepared)
        return false;
    return apply(*prepared);
}

bool PresetManager::delete_preset(const PresetInfo& preset) {
    if (preset.is_factory) return false; // can't delete factory presets
    std::error_code ec;
    bool ok = fs::remove(preset.path, ec);
    if (ok) {
        cache_valid_ = false;
        if (on_list_changed) on_list_changed();
    }
    return ok;
}

bool PresetManager::rename(const PresetInfo& preset, const std::string& new_name) {
    if (preset.is_factory || !is_safe_path_component(new_name))
        return false;
    auto new_path = preset.path.parent_path() / (new_name + ".json");
    std::error_code ec;
    fs::rename(preset.path, new_path, ec);
    if (ec) return false;

    if (current_name_ == preset.name) current_name_ = new_name;
    cache_valid_ = false;
    if (on_list_changed) on_list_changed();
    return true;
}

std::optional<PresetInfo> PresetManager::import_file(const fs::path& source) {
    if (!fs::exists(source)) return std::nullopt;
    ensure_user_dir();

    auto dest = user_dir_ / source.filename();
    std::error_code ec;
    fs::copy_file(source, dest, fs::copy_options::skip_existing, ec);
    if (ec) return std::nullopt;

    cache_valid_ = false;
    if (on_list_changed) on_list_changed();

    PresetInfo info;
    info.name = dest.stem().string();
    info.path = dest;
    info.is_factory = false;
    return info;
}

// ── Discovery ────────────────────────────────────────────────────────────

void PresetManager::scan_directory(const fs::path& dir, bool is_factory,
                                    std::vector<PresetInfo>& out) const {
    if (!fs::exists(dir)) return;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;

        PresetInfo info;
        info.name = entry.path().stem().string();
        info.path = entry.path();
        info.is_factory = is_factory;

        // Author metadata is parsed once per (path, mtime, size); an unchanged
        // file is not re-read on later listings.
        {
            std::error_code ec;
            const auto mtime = fs::last_write_time(entry.path(), ec);
            const auto size = ec ? std::uintmax_t{0} : fs::file_size(entry.path(), ec);
            const std::string cache_key = entry.path().string();
            auto it = metadata_cache_.find(cache_key);
            if (it == metadata_cache_.end() || it->second.mtime != mtime ||
                it->second.size != size) {
                MetadataCacheEntry fresh;
                fresh.mtime = mtime;
                fresh.size = size;
                ++metadata_parse_count_;
                std::ifstream in(entry.path());
                if (in.is_open()) {
                    std::string body((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
                    try {
                        fresh.metadata = metadata_from_document(choc::json::parse(body).getView());
                    } catch (...) {
                    }
                }
                it = metadata_cache_.insert_or_assign(cache_key, std::move(fresh)).first;
            }
            info.author = it->second.metadata.author;
            info.description = it->second.metadata.description;
            info.tags = it->second.metadata.tags;
        }

        // Compute relative folder path
        auto rel = fs::relative(entry.path().parent_path(), dir);
        if (rel != ".") info.folder = rel.string();

        out.push_back(info);
    }
}

std::vector<PresetInfo> PresetManager::all_presets() const {
    std::vector<PresetInfo> result;
    scan_directory(factory_dir_, true, result);
    auto content = content_presets();
    result.insert(result.end(), content.begin(), content.end());
    scan_directory(user_dir_, false, result);
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });
    return result;
}

std::vector<PresetInfo> PresetManager::factory_presets() const {
    std::vector<PresetInfo> result;
    scan_directory(factory_dir_, true, result);
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });
    return result;
}

std::vector<PresetInfo> PresetManager::user_presets() const {
    std::vector<PresetInfo> result;
    scan_directory(user_dir_, false, result);
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });
    return result;
}

std::vector<PresetInfo> PresetManager::content_presets() const {
    if (content_plugin_id_.empty() || !content_source_)
        return {};
    if (!content_kinds_.empty() && !contains_string(content_kinds_, "presets")) return {};

    ContentCapabilityManifest manifest;
    manifest.plugin_id = content_plugin_id_;
    manifest.capabilities = content_capabilities_;
    manifest.content_kinds = content_kinds_.empty() ? std::vector<std::string>{"presets"}
                                                    : content_kinds_;

    return content_source_(manifest, content_data_root_);
}

void PresetManager::set_content_preset_source(ContentPresetSource source) {
    content_source_ = std::move(source);
    cache_valid_ = false;
}

void PresetManager::set_content_plugin_id(std::string plugin_id) {
    content_plugin_id_ = std::move(plugin_id);
    cache_valid_ = false;
}

void PresetManager::set_content_capabilities(std::vector<std::string> capabilities) {
    content_capabilities_ = std::move(capabilities);
    content_kinds_.clear();
    cache_valid_ = false;
}

void PresetManager::set_content_data_root(fs::path data_root) {
    content_data_root_ = std::move(data_root);
    cache_valid_ = false;
}

void PresetManager::set_content_manifest(const ContentCapabilityManifest& manifest) {
    content_plugin_id_ = manifest.plugin_id;
    content_capabilities_ = manifest.capabilities;
    content_kinds_ = manifest.content_kinds;
    cache_valid_ = false;
}

void PresetManager::refresh() { cache_valid_ = false; }

// ── Navigation ───────────────────────────────────────────────────────────

bool PresetManager::load_next() {
    auto presets = all_presets();
    if (presets.empty()) return false;

    int current_idx = -1;
    for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
        if (presets[static_cast<size_t>(i)].name == current_name_) {
            current_idx = i;
            break;
        }
    }

    int next = (current_idx + 1) % static_cast<int>(presets.size());
    return load(presets[static_cast<size_t>(next)]);
}

bool PresetManager::load_previous() {
    auto presets = all_presets();
    if (presets.empty()) return false;

    int current_idx = 0;
    for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
        if (presets[static_cast<size_t>(i)].name == current_name_) {
            current_idx = i;
            break;
        }
    }

    int prev = (current_idx - 1 + static_cast<int>(presets.size())) % static_cast<int>(presets.size());
    return load(presets[static_cast<size_t>(prev)]);
}

} // namespace pulp::state
