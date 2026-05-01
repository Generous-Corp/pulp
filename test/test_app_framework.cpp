#include <catch2/catch_test_macros.hpp>
#include <pulp/view/app_framework.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

using namespace pulp::view;

namespace {

std::filesystem::path make_temp_root(const std::string& name) {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::temp_directory_path() /
                (name + "-" + std::to_string(stamp));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const std::string& value) : name_(name) {
        if (const char* old = std::getenv(name_)) old_ = std::string(old);
#ifdef _WIN32
        _putenv_s(name_, value.c_str());
#else
        setenv(name_, value.c_str(), 1);
#endif
    }

    ~ScopedEnvVar() {
#ifdef _WIN32
        _putenv_s(name_, old_ ? old_->c_str() : "");
#else
        if (old_) setenv(name_, old_->c_str(), 1);
        else unsetenv(name_);
#endif
    }

private:
    const char* name_;
    std::optional<std::string> old_;
};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream f(path);
    return {std::istreambuf_iterator<char>(f),
            std::istreambuf_iterator<char>()};
}

const MenuBar::Menu* find_menu(const MenuBar& menu, const std::string& title) {
    for (const auto& candidate : menu.menus()) {
        if (candidate.title == title) return &candidate;
    }
    return nullptr;
}

} // namespace

// ── KeyShortcut ──────────────────────────────────────────────────────────

TEST_CASE("KeyShortcut from_string parses Cmd+S", "[view][keys]") {
    auto ks = KeyShortcut::from_string("Cmd+S");
    REQUIRE(ks.key == KeyCode::s);
    REQUIRE((ks.modifiers & kModCmd) != 0);
}

TEST_CASE("KeyShortcut from_string parses Ctrl+Shift+Z", "[view][keys]") {
    auto ks = KeyShortcut::from_string("Ctrl+Shift+Z");
    REQUIRE(ks.key == KeyCode::z);
    REQUIRE((ks.modifiers & kModCtrl) != 0);
    REQUIRE((ks.modifiers & kModShift) != 0);
}

TEST_CASE("KeyShortcut from_string parses F5", "[view][keys]") {
    auto ks = KeyShortcut::from_string("F5");
    REQUIRE(ks.key == KeyCode::f5);
    REQUIRE(ks.modifiers == 0);
}

TEST_CASE("KeyShortcut parses named keys and rejects invalid function keys",
          "[view][keys]") {
    auto f12 = KeyShortcut::from_string("Alt+Ctrl+F12");
    REQUIRE(f12.key == KeyCode::f12);
    REQUIRE((f12.modifiers & kModAlt) != 0);
    REQUIRE((f12.modifiers & kModCtrl) != 0);
    REQUIRE(f12.to_string() == "Ctrl+Alt+F12");

    REQUIRE(KeyShortcut::from_string("F0").key == KeyCode::unknown);
    REQUIRE(KeyShortcut::from_string("F13").key == KeyCode::unknown);
    REQUIRE(KeyShortcut::from_string("Function").key == KeyCode::unknown);

    REQUIRE(KeyShortcut::from_string("Enter").key == KeyCode::enter);
    REQUIRE(KeyShortcut::from_string("Escape").key == KeyCode::escape);
    REQUIRE(KeyShortcut::from_string("Space").key == KeyCode::space);
    REQUIRE(KeyShortcut::from_string("Tab").key == KeyCode::tab);
    REQUIRE(KeyShortcut::from_string("Backspace").key == KeyCode::backspace);
    REQUIRE(KeyShortcut::from_string("Delete").key == KeyCode::delete_);
    REQUIRE(KeyShortcut::from_string("Left").key == KeyCode::left);
    REQUIRE(KeyShortcut::from_string("Right").key == KeyCode::right);
    REQUIRE(KeyShortcut::from_string("Up").key == KeyCode::up);
    REQUIRE(KeyShortcut::from_string("Down").key == KeyCode::down);
}

TEST_CASE("KeyShortcut to_string covers modifiers and named keys",
          "[view][keys]") {
    KeyShortcut ks;
    ks.key = KeyCode::delete_;
    ks.modifiers = kModCmd | kModCtrl | kModShift | kModAlt | kModMeta;
    REQUIRE(ks.to_string() == "Cmd+Ctrl+Shift+Alt+Meta+Delete");

    REQUIRE(KeyShortcut{KeyCode::left, 0}.to_string() == "Left");
    REQUIRE(KeyShortcut{KeyCode::right, 0}.to_string() == "Right");
    REQUIRE(KeyShortcut{KeyCode::up, 0}.to_string() == "Up");
    REQUIRE(KeyShortcut{KeyCode::down, 0}.to_string() == "Down");
    REQUIRE(KeyShortcut{KeyCode::space, 0}.to_string() == "Space");
    REQUIRE(KeyShortcut{KeyCode::tab, 0}.to_string() == "Tab");
    REQUIRE(KeyShortcut{KeyCode::unknown, kModCmd}.to_string() == "Cmd+");
}

TEST_CASE("KeyShortcut to_string round-trips", "[view][keys]") {
    auto ks = KeyShortcut::from_string("Cmd+S");
    REQUIRE(ks.to_string() == "Cmd+S");
}

TEST_CASE("KeyShortcut matches key event", "[view][keys]") {
    auto ks = KeyShortcut::from_string("Cmd+S");

    KeyEvent match;
    match.key = KeyCode::s;
    match.modifiers = kModCmd;
    match.is_down = true;
    REQUIRE(ks.matches(match));

    KeyEvent no_match;
    no_match.key = KeyCode::s;
    no_match.modifiers = 0;
    no_match.is_down = true;
    REQUIRE_FALSE(ks.matches(no_match));

    KeyEvent key_up = match;
    key_up.is_down = false;
    REQUIRE_FALSE(ks.matches(key_up));
}

// ── KeyMapping ───────────────────────────────────────────────────────────

TEST_CASE("KeyMapping add and handle command", "[view][keys]") {
    KeyMapping km;
    bool saved = false;

    km.add_command({1, "Save", "File",
                    KeyShortcut::from_string("Cmd+S"),
                    [&] { saved = true; }, {}, false});

    KeyEvent e;
    e.key = KeyCode::s;
    e.modifiers = kModCmd;
    e.is_down = true;

    REQUIRE(km.handle_key(e));
    REQUIRE(saved);
}

TEST_CASE("KeyMapping unmatched key returns false", "[view][keys]") {
    KeyMapping km;
    km.add_command({1, "Save", "File",
                    KeyShortcut::from_string("Cmd+S"),
                    [] {}, {}, false});

    KeyEvent e;
    e.key = KeyCode::q;
    e.modifiers = kModCmd;
    e.is_down = true;

    REQUIRE_FALSE(km.handle_key(e));
}

TEST_CASE("KeyMapping remove command", "[view][keys]") {
    KeyMapping km;
    km.add_command({1, "Save", "File", {}, [] {}, {}, false});
    km.add_command({2, "Open", "File", {}, [] {}, {}, false});
    REQUIRE(km.commands().size() == 2);

    km.remove_command(1);
    REQUIRE(km.commands().size() == 1);
    REQUIRE(km.commands()[0].id == 2);
}

TEST_CASE("KeyMapping rebind shortcut", "[view][keys]") {
    KeyMapping km;
    km.add_command({1, "Save", "File",
                    KeyShortcut::from_string("Cmd+S"),
                    [] {}, {}, false});

    km.rebind(1, KeyShortcut::from_string("Ctrl+S"));
    REQUIRE((km.commands()[0].shortcut.modifiers & kModCtrl) != 0);
}

TEST_CASE("KeyMapping commands_in_category", "[view][keys]") {
    KeyMapping km;
    km.add_command({1, "New", "File", {}, [] {}, {}, false});
    km.add_command({2, "Open", "File", {}, [] {}, {}, false});
    km.add_command({3, "Undo", "Edit", {}, [] {}, {}, false});

    auto file_cmds = km.commands_in_category("File");
    REQUIRE(file_cmds.size() == 2);

    auto edit_cmds = km.commands_in_category("Edit");
    REQUIRE(edit_cmds.size() == 1);
}

TEST_CASE("KeyMapping disabled command is skipped", "[view][keys]") {
    KeyMapping km;
    bool fired = false;
    km.add_command({1, "Disabled", "Edit",
                    KeyShortcut::from_string("Cmd+D"),
                    [&] { fired = true; },
                    [] { return false; }, false}); // always disabled

    KeyEvent e;
    e.key = KeyCode::d;
    e.modifiers = kModCmd;
    e.is_down = true;
    REQUIRE_FALSE(km.handle_key(e));
    REQUIRE_FALSE(fired);
}

TEST_CASE("KeyMapping disabled match falls through to later enabled command",
          "[view][keys]") {
    KeyMapping km;
    bool disabled_fired = false;
    bool enabled_fired = false;

    km.add_command({1, "Disabled", "Edit",
                    KeyShortcut::from_string("Cmd+K"),
                    [&] { disabled_fired = true; },
                    [] { return false; }, false});
    km.add_command({2, "Enabled", "Edit",
                    KeyShortcut::from_string("Cmd+K"),
                    [&] { enabled_fired = true; },
                    {}, false});

    KeyEvent e;
    e.key = KeyCode::k;
    e.modifiers = kModCmd;
    e.is_down = true;

    REQUIRE(km.handle_key(e));
    REQUIRE_FALSE(disabled_fired);
    REQUIRE(enabled_fired);
}

TEST_CASE("KeyMapping no-op branches leave commands intact", "[view][keys]") {
    KeyMapping km;
    km.add_command({1, "Save", "File",
                    KeyShortcut::from_string("Cmd+S"), {}, {}, false});

    km.remove_command(99);
    km.rebind(99, KeyShortcut::from_string("Ctrl+S"));
    REQUIRE(km.commands().size() == 1);
    REQUIRE(km.commands()[0].shortcut.to_string() == "Cmd+S");

    KeyEvent e;
    e.key = KeyCode::s;
    e.modifiers = kModCmd;
    e.is_down = true;

    REQUIRE(km.handle_key(e));
}

TEST_CASE("KeyMapping save/load binding edge paths", "[view][keys]") {
    KeyMapping km;
    km.add_command({1, "Save", "File",
                    KeyShortcut::from_string("Cmd+S"), [] {}, {}, false});
    km.add_command({2, "Unbound", "File", {}, [] {}, {}, false});
    km.add_command({3, "Help", "Help",
                    KeyShortcut::from_string("Ctrl+Alt+F12"), [] {},
                    {}, false});

    auto root = make_temp_root("pulp-keymapping");
    auto path = root / "bindings.json";

    REQUIRE(km.save_bindings(path));
    auto content = read_file(path);
    REQUIRE(content.find("\"1\": \"Cmd+S\"") != std::string::npos);
    REQUIRE(content.find("\"3\": \"Ctrl+Alt+F12\"") != std::string::npos);
    REQUIRE(content.find("\"2\":") == std::string::npos);

    REQUIRE(km.load_bindings(path));
    REQUIRE_FALSE(km.load_bindings(root / "missing.json"));

    std::filesystem::remove_all(root);
}

// ── MenuBar ──────────────────────────────────────────────────────────────

TEST_CASE("MenuBar add menu", "[view][menu]") {
    MenuBar menu;
    menu.add_menu("File", {
        {"New", [] {}, {}, true, false, {}},
        {"Open", [] {}, {}, true, false, {}},
    });

    REQUIRE(menu.menus().size() == 1);
    REQUIRE(menu.menus()[0].title == "File");
    REQUIRE(menu.menus()[0].items.size() == 2);
}

TEST_CASE("MenuBar set_key_mapping populates from commands", "[view][menu]") {
    KeyMapping km;
    km.add_command({1, "Save", "File", {}, [] {}, {}, false});
    km.add_command({2, "Undo", "Edit", {}, [] {}, {}, false});

    MenuBar menu;
    menu.set_key_mapping(km);

    REQUIRE(menu.menus().size() == 2);
}

TEST_CASE("MenuBar set_key_mapping preserves command metadata",
          "[view][menu]") {
    bool fired = false;
    KeyMapping km;
    km.add_command({1, "Save", "File",
                    KeyShortcut::from_string("Cmd+S"),
                    [&] { fired = true; },
                    {}, false});
    km.add_command({2, "", "File", {}, {}, {}, true});

    MenuBar menu;
    menu.set_key_mapping(km);

    auto* file = find_menu(menu, "File");
    REQUIRE(file != nullptr);
    REQUIRE(file->items.size() == 2);
    REQUIRE(file->items[0].title == "Save");
    REQUIRE(file->items[0].shortcut.to_string() == "Cmd+S");
    REQUIRE_FALSE(file->items[0].is_separator);
    REQUIRE(file->items[1].is_separator);

    file->items[0].action();
    REQUIRE(fired);
}

// ── NativeToolbar ────────────────────────────────────────────────────────

TEST_CASE("NativeToolbar add items and separator", "[view][toolbar]") {
    NativeToolbar tb;
    tb.add_item({"play", "Play", "play.fill", [] {}});
    tb.add_separator();
    tb.add_item({"stop", "Stop", "stop.fill", [] {}});

    REQUIRE(tb.items().size() == 3);
    REQUIRE(tb.items()[1].is_separator);
    tb.install_native(nullptr);
}

// ── AppSettings ──────────────────────────────────────────────────────────

TEST_CASE("AppSettings get/set string", "[view][settings]") {
    AppSettings settings("PulpTest");

    REQUIRE_FALSE(settings.get_string("key").has_value());

    settings.set_string("key", "value");
    REQUIRE(settings.get_string("key").value() == "value");
}

TEST_CASE("AppSettings get/set int", "[view][settings]") {
    AppSettings settings("PulpTest");
    settings.set_int("buffer_size", 512);
    REQUIRE(settings.get_int("buffer_size").value() == 512);
}

TEST_CASE("AppSettings get/set float", "[view][settings]") {
    AppSettings settings("PulpTest");
    settings.set_float("volume", 0.75f);
    auto val = settings.get_float("volume");
    REQUIRE(val.has_value());
    REQUIRE(val.value() > 0.74f);
    REQUIRE(val.value() < 0.76f);
}

TEST_CASE("AppSettings get/set bool", "[view][settings]") {
    AppSettings settings("PulpTest");
    settings.set_bool("dark_mode", true);
    REQUIRE(settings.get_bool("dark_mode").value() == true);

    settings.set_bool("dark_mode", false);
    REQUIRE(settings.get_bool("dark_mode").value() == false);
}

TEST_CASE("AppSettings invalid typed values return null or false",
          "[view][settings]") {
    AppSettings settings("PulpTest");

    settings.set_string("bad_int", "abc");
    settings.set_string("bad_float", "abc");
    settings.set_string("boolish", "yes");

    REQUIRE_FALSE(settings.get_int("bad_int").has_value());
    REQUIRE_FALSE(settings.get_float("bad_float").has_value());
    REQUIRE(settings.get_bool("boolish").value() == false);
    REQUIRE_FALSE(settings.load_window_state().has_value());
}

TEST_CASE("AppSettings window state round-trip", "[view][settings]") {
    AppSettings settings("PulpTest");
    settings.save_window_state(100, 200, 800, 600);

    auto ws = settings.load_window_state();
    REQUIRE(ws.has_value());
    REQUIRE(ws->x == 100);
    REQUIRE(ws->y == 200);
    REQUIRE(ws->width == 800);
    REQUIRE(ws->height == 600);
}

TEST_CASE("AppSettings saves and loads from platform settings root",
          "[view][settings]") {
    auto root = make_temp_root("pulp-app-settings");

#if defined(_WIN32)
    ScopedEnvVar settings_root("APPDATA", root.string());
#elif defined(__APPLE__)
    ScopedEnvVar settings_root("HOME", root.string());
#else
    ScopedEnvVar settings_root("XDG_CONFIG_HOME", root.string());
#endif

    const std::string app_name = "PulpSettingsCoverage";
    AppSettings settings(app_name);
    REQUIRE_FALSE(settings.settings_path().empty());
    REQUIRE(settings.settings_path().string().find(app_name) != std::string::npos);
    REQUIRE_FALSE(settings.load());

    settings.set_string("name", "pulp");
    settings.set_int("buffer", 256);
    settings.save_window_state(1, 2, 300, 400);
    REQUIRE(settings.save());

    AppSettings loaded(app_name);
    REQUIRE(loaded.load());
    REQUIRE(loaded.get_string("name").value() == "pulp");
    REQUIRE(loaded.get_int("buffer").value() == 256);

    auto window = loaded.load_window_state();
    REQUIRE(window.has_value());
    REQUIRE(window->x == 1);
    REQUIRE(window->y == 2);
    REQUIRE(window->width == 300);
    REQUIRE(window->height == 400);

    std::filesystem::remove_all(root);
}
