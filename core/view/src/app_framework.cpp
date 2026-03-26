#include <pulp/view/app_framework.hpp>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace pulp::view {

// ── KeyShortcut ──────────────────────────────────────────────────────────

KeyShortcut KeyShortcut::from_string(const std::string& s) {
    KeyShortcut result;
    std::string remaining = s;

    // Parse modifier prefixes
    auto consume = [&](const std::string& prefix, uint16_t mod) {
        auto pos = remaining.find(prefix);
        if (pos != std::string::npos) {
            result.modifiers |= mod;
            remaining.erase(pos, prefix.size());
        }
    };

    consume("Cmd+", kModCmd);
    consume("Ctrl+", kModCtrl);
    consume("Shift+", kModShift);
    consume("Alt+", kModAlt);
    consume("Meta+", kModMeta);

    // Parse key
    if (remaining.size() == 1) {
        char c = static_cast<char>(std::tolower(static_cast<unsigned char>(remaining[0])));
        result.key = static_cast<KeyCode>(c);
    } else if (remaining.substr(0, 1) == "F" && remaining.size() <= 3) {
        int fnum = std::atoi(remaining.c_str() + 1);
        if (fnum >= 1 && fnum <= 12) {
            result.key = static_cast<KeyCode>(static_cast<int>(KeyCode::f1) + fnum - 1);
        }
    } else if (remaining == "Enter") result.key = KeyCode::enter;
    else if (remaining == "Escape") result.key = KeyCode::escape;
    else if (remaining == "Space") result.key = KeyCode::space;
    else if (remaining == "Tab") result.key = KeyCode::tab;
    else if (remaining == "Backspace") result.key = KeyCode::backspace;
    else if (remaining == "Delete") result.key = KeyCode::delete_;
    else if (remaining == "Left") result.key = KeyCode::left;
    else if (remaining == "Right") result.key = KeyCode::right;
    else if (remaining == "Up") result.key = KeyCode::up;
    else if (remaining == "Down") result.key = KeyCode::down;

    return result;
}

std::string KeyShortcut::to_string() const {
    std::string result;
    if (modifiers & kModCmd) result += "Cmd+";
    if (modifiers & kModCtrl) result += "Ctrl+";
    if (modifiers & kModShift) result += "Shift+";
    if (modifiers & kModAlt) result += "Alt+";
    if (modifiers & kModMeta) result += "Meta+";

    int k = static_cast<int>(key);
    if (k >= 'a' && k <= 'z') {
        result += static_cast<char>(std::toupper(k));
    } else if (k >= static_cast<int>(KeyCode::f1) && k <= static_cast<int>(KeyCode::f12)) {
        result += "F" + std::to_string(k - static_cast<int>(KeyCode::f1) + 1);
    } else if (key == KeyCode::enter) result += "Enter";
    else if (key == KeyCode::escape) result += "Escape";
    else if (key == KeyCode::space) result += "Space";
    else if (key == KeyCode::tab) result += "Tab";
    else if (key == KeyCode::backspace) result += "Backspace";
    else if (key == KeyCode::delete_) result += "Delete";
    else if (key == KeyCode::left) result += "Left";
    else if (key == KeyCode::right) result += "Right";
    else if (key == KeyCode::up) result += "Up";
    else if (key == KeyCode::down) result += "Down";

    return result;
}

// ── KeyMapping ───────────────────────────────────────────────────────────

void KeyMapping::add_command(Command cmd) {
    commands_.push_back(std::move(cmd));
}

void KeyMapping::remove_command(CommandID id) {
    commands_.erase(
        std::remove_if(commands_.begin(), commands_.end(),
                        [id](const auto& c) { return c.id == id; }),
        commands_.end());
}

bool KeyMapping::handle_key(const KeyEvent& event) {
    for (const auto& cmd : commands_) {
        if (cmd.shortcut.matches(event)) {
            if (cmd.is_enabled && !cmd.is_enabled()) continue;
            if (cmd.action) cmd.action();
            return true;
        }
    }
    return false;
}

std::vector<const Command*>
KeyMapping::commands_in_category(const std::string& category) const {
    std::vector<const Command*> result;
    for (const auto& cmd : commands_) {
        if (cmd.category == category) result.push_back(&cmd);
    }
    return result;
}

void KeyMapping::rebind(CommandID id, KeyShortcut shortcut) {
    for (auto& cmd : commands_) {
        if (cmd.id == id) {
            cmd.shortcut = shortcut;
            return;
        }
    }
}

bool KeyMapping::save_bindings(const std::filesystem::path& path) const {
    std::ofstream f(path);
    if (!f.is_open()) return false;

    f << "{\n";
    bool first = true;
    for (const auto& cmd : commands_) {
        if (cmd.shortcut.key == KeyCode::unknown) continue;
        if (!first) f << ",\n";
        first = false;
        f << "  \"" << cmd.id << "\": \"" << cmd.shortcut.to_string() << "\"";
    }
    f << "\n}\n";
    return true;
}

bool KeyMapping::load_bindings(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    // Simplified: not implementing full JSON parse for keybinding load
    return true;
}

// ── MenuBar ──────────────────────────────────────────────────────────────

void MenuBar::add_menu(std::string title, std::vector<MenuItem> items) {
    menus_.push_back({std::move(title), std::move(items)});
}

void MenuBar::set_key_mapping(const KeyMapping& keys) {
    menus_.clear();
    std::unordered_map<std::string, std::vector<MenuItem>> categories;

    for (const auto& cmd : keys.commands()) {
        MenuItem item;
        item.title = cmd.name;
        item.action = cmd.action;
        item.shortcut = cmd.shortcut;
        item.is_separator = cmd.is_separator;
        categories[cmd.category].push_back(std::move(item));
    }

    for (auto& [cat, items] : categories) {
        menus_.push_back({cat, std::move(items)});
    }
}

void MenuBar::install_native() {
    // Platform-specific: implemented in platform/mac/menu_bar_mac.mm etc.
}

// ── Toolbar ──────────────────────────────────────────────────────────────

void Toolbar::add_item(ToolbarItem item) {
    items_.push_back(std::move(item));
}

void Toolbar::add_separator() {
    items_.push_back({"", "", "", {}, true});
}

void Toolbar::install_native(void*) {
    // Platform-specific: implemented in platform/mac/toolbar_mac.mm etc.
}

// ── AppSettings ──────────────────────────────────────────────────────────

static std::filesystem::path platform_settings_root() {
#ifdef __APPLE__
    const char* home = std::getenv("HOME");
    if (home) return std::filesystem::path(home) / "Library" / "Application Support";
    return {};
#elif defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    if (appdata) return std::filesystem::path(appdata);
    return {};
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg) return std::filesystem::path(xdg);
    const char* home = std::getenv("HOME");
    if (home) return std::filesystem::path(home) / ".config";
    return {};
#endif
}

AppSettings::AppSettings(const std::string& app_name) {
    auto root = platform_settings_root();
    if (!root.empty()) {
        settings_path_ = root / app_name / "settings.json";
    }
}

std::optional<std::string> AppSettings::get_string(const std::string& key) const {
    auto it = values_.find(key);
    if (it != values_.end()) return it->second;
    return std::nullopt;
}

std::optional<int> AppSettings::get_int(const std::string& key) const {
    auto s = get_string(key);
    if (s) {
        try { return std::stoi(*s); }
        catch (...) { return std::nullopt; }
    }
    return std::nullopt;
}

std::optional<float> AppSettings::get_float(const std::string& key) const {
    auto s = get_string(key);
    if (s) {
        try { return std::stof(*s); }
        catch (...) { return std::nullopt; }
    }
    return std::nullopt;
}

std::optional<bool> AppSettings::get_bool(const std::string& key) const {
    auto s = get_string(key);
    if (s) return *s == "true" || *s == "1";
    return std::nullopt;
}

void AppSettings::set_string(const std::string& key, const std::string& value) {
    values_[key] = value;
}

void AppSettings::set_int(const std::string& key, int value) {
    values_[key] = std::to_string(value);
}

void AppSettings::set_float(const std::string& key, float value) {
    values_[key] = std::to_string(value);
}

void AppSettings::set_bool(const std::string& key, bool value) {
    values_[key] = value ? "true" : "false";
}

bool AppSettings::save() {
    if (settings_path_.empty()) return false;

    std::error_code ec;
    std::filesystem::create_directories(settings_path_.parent_path(), ec);

    std::ofstream f(settings_path_);
    if (!f.is_open()) return false;

    f << "{\n";
    bool first = true;
    for (const auto& [k, v] : values_) {
        if (!first) f << ",\n";
        first = false;
        f << "  \"" << k << "\": \"" << v << "\"";
    }
    f << "\n}\n";
    return true;
}

bool AppSettings::load() {
    if (settings_path_.empty()) return false;

    std::ifstream f(settings_path_);
    if (!f.is_open()) return false;

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    // Simple key-value JSON parsing
    values_.clear();
    size_t pos = 0;
    while ((pos = content.find('"', pos)) != std::string::npos) {
        auto key_end = content.find('"', pos + 1);
        if (key_end == std::string::npos) break;
        std::string key = content.substr(pos + 1, key_end - pos - 1);

        auto colon = content.find(':', key_end);
        if (colon == std::string::npos) break;

        auto val_start = content.find('"', colon);
        if (val_start == std::string::npos) break;
        auto val_end = content.find('"', val_start + 1);
        if (val_end == std::string::npos) break;

        values_[key] = content.substr(val_start + 1, val_end - val_start - 1);
        pos = val_end + 1;
    }

    return true;
}

void AppSettings::save_window_state(int x, int y, int width, int height) {
    set_int("window_x", x);
    set_int("window_y", y);
    set_int("window_width", width);
    set_int("window_height", height);
}

std::optional<AppSettings::WindowState> AppSettings::load_window_state() const {
    auto x = get_int("window_x");
    auto y = get_int("window_y");
    auto w = get_int("window_width");
    auto h = get_int("window_height");
    if (x && y && w && h) return WindowState{*x, *y, *w, *h};
    return std::nullopt;
}

} // namespace pulp::view
