#include <pulp/state/preset_manager.hpp>
#include <pulp/runtime/system.hpp>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace fs = std::filesystem;

namespace pulp::state {

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
        user_dir_ = root / manufacturer / plugin_name / "Presets";
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

    // Serialize state
    auto state_data = store_.serialize();

    // Write JSON preset file
    std::ofstream f(path);
    if (!f.is_open()) return false;

    f << "{\n";
    f << "  \"name\": \"" << name << "\",\n";
    f << "  \"manufacturer\": \"" << manufacturer_ << "\",\n";
    f << "  \"plugin\": \"" << plugin_name_ << "\",\n";
    f << "  \"version\": " << store_.state_version() << ",\n";
    f << "  \"parameters\": {";

    auto params = store_.all_params();
    for (size_t i = 0; i < params.size(); ++i) {
        if (i > 0) f << ",";
        f << "\n    \"" << params[i].name << "\": " << store_.get_value(params[i].id);
    }
    f << "\n  }\n";
    f << "}\n";

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
            float value = std::stof(val_str);
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
