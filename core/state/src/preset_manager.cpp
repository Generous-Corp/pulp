#include <pulp/state/preset_manager.hpp>
#include <pulp/state/content_registry.hpp>
#include <pulp/runtime/system.hpp>
#include <choc/text/choc_JSON.h>
#include <cmath>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <string_view>
#include <utility>

namespace fs = std::filesystem;

namespace pulp::state {

namespace {

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
#ifdef __APPLE__
    // ~/Library/Audio/Presets/
    if (auto home = runtime::get_env("HOME"))
        return fs::path(*home) / "Library" / "Audio" / "Presets";
    return {};
#elif defined(_WIN32)
    // %APPDATA%/
    if (auto appdata = runtime::get_env("APPDATA"))
        return fs::path(*appdata);
    return {};
#else
    // ~/.config/
    if (auto xdg = runtime::get_env("XDG_CONFIG_HOME"))
        return fs::path(*xdg);
    if (auto home = runtime::get_env("HOME"))
        return fs::path(*home) / ".config";
    return {};
#endif
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

    // Factory presets: look in the executable's bundle Resources
#ifdef __APPLE__
    // macOS: MyPlugin.component/Contents/Resources/Presets/
    // This is a simplification — in practice, use NSBundle
    factory_dir_ = ""; // Set by the format adapter if applicable
#endif
}

void PresetManager::ensure_user_dir() {
    if (!user_dir_.empty() && !fs::exists(user_dir_)) {
        std::error_code ec;
        fs::create_directories(user_dir_, ec);
    }
}

// ── Save/Load ────────────────────────────────────────────────────────────

bool PresetManager::save(const std::string& name, const std::string& folder) {
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
    // escaper so a name / manufacturer / plugin containing a quote, backslash,
    // or control character can never produce a corrupt (unparseable) file.
    // Parameter-name keys are left raw to stay compatible with load()'s
    // raw-key string scan.
    std::ostringstream out;
    out << "{\n";
    out << "  \"name\": " << choc::json::getEscapedQuotedString(name) << ",\n";
    out << "  \"manufacturer\": " << choc::json::getEscapedQuotedString(manufacturer_) << ",\n";
    out << "  \"plugin\": " << choc::json::getEscapedQuotedString(plugin_name_) << ",\n";
    out << "  \"version\": " << store_.state_version() << ",\n";
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
        out << "\n    \"" << params[i].name << "\": " << value;
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

bool PresetManager::load(const fs::path& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    // Simple JSON parameter extraction
    // Look for "parameters": { "name": value, ... }
    auto params_pos = content.find("\"parameters\"");
    if (params_pos == std::string::npos) return false;

    auto brace = content.find('{', params_pos);
    if (brace == std::string::npos) return false;

    // Parse each "name": value pair
    auto all_params = store_.all_params();
    for (const auto& param : all_params) {
        auto key_pos = content.find("\"" + param.name + "\"", brace);
        if (key_pos == std::string::npos) continue;

        auto colon = content.find(':', key_pos);
        if (colon == std::string::npos) continue;

        // Read the numeric value
        auto val_start = colon + 1;
        while (val_start < content.size() && content[val_start] == ' ') ++val_start;

        auto val_end = content.find_first_of(",}\n", val_start);
        if (val_end == std::string::npos) val_end = content.size();

        std::string val_str = content.substr(val_start, val_end - val_start);
        try {
            std::size_t parsed = 0;
            float value = std::stof(val_str, &parsed);
            if (!has_only_trailing_space(val_str, parsed)) continue;
            store_.set_value(param.id, value);
        } catch (...) {
            continue;
        }
    }

    // Extract preset name from path
    current_name_ = path.stem().string();
    unsaved_changes_ = false;

    PresetInfo info;
    info.name = current_name_;
    info.path = path;
    if (on_preset_loaded) on_preset_loaded(info);

    return true;
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
    if (preset.is_factory) return false;
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
    if (content_plugin_id_.empty()) return {};
    if (!content_kinds_.empty() && !contains_string(content_kinds_, "presets")) return {};

    ContentCapabilityManifest manifest;
    manifest.plugin_id = content_plugin_id_;
    manifest.capabilities = content_capabilities_;
    manifest.content_kinds = content_kinds_.empty() ? std::vector<std::string>{"presets"}
                                                    : content_kinds_;

    ContentRegistry registry(content_data_root_.empty()
                                 ? ContentRegistry::platform_data_root()
                                 : content_data_root_);
    return registry.presets_for_plugin(manifest);
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
