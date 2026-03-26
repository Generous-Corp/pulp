#include <catch2/catch_test_macros.hpp>
#include <pulp/view/app_framework.hpp>

using namespace pulp::view;

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
}

// ── KeyMapping ───────────────────────────────────────────────────────────

TEST_CASE("KeyMapping add and handle command", "[view][keys]") {
    KeyMapping km;
    bool saved = false;

    km.add_command({1, "Save", "File",
                    KeyShortcut::from_string("Cmd+S"),
                    [&] { saved = true; }});

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
                    [] {}});

    KeyEvent e;
    e.key = KeyCode::q;
    e.modifiers = kModCmd;
    e.is_down = true;

    REQUIRE_FALSE(km.handle_key(e));
}

TEST_CASE("KeyMapping remove command", "[view][keys]") {
    KeyMapping km;
    km.add_command({1, "Save", "File", {}, [] {}});
    km.add_command({2, "Open", "File", {}, [] {}});
    REQUIRE(km.commands().size() == 2);

    km.remove_command(1);
    REQUIRE(km.commands().size() == 1);
    REQUIRE(km.commands()[0].id == 2);
}

TEST_CASE("KeyMapping rebind shortcut", "[view][keys]") {
    KeyMapping km;
    km.add_command({1, "Save", "File",
                    KeyShortcut::from_string("Cmd+S"),
                    [] {}});

    km.rebind(1, KeyShortcut::from_string("Ctrl+S"));
    REQUIRE((km.commands()[0].shortcut.modifiers & kModCtrl) != 0);
}

TEST_CASE("KeyMapping commands_in_category", "[view][keys]") {
    KeyMapping km;
    km.add_command({1, "New", "File", {}, [] {}});
    km.add_command({2, "Open", "File", {}, [] {}});
    km.add_command({3, "Undo", "Edit", {}, [] {}});

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
                    [] { return false; }}); // always disabled

    KeyEvent e;
    e.key = KeyCode::d;
    e.modifiers = kModCmd;
    e.is_down = true;
    REQUIRE_FALSE(km.handle_key(e));
    REQUIRE_FALSE(fired);
}

// ── MenuBar ──────────────────────────────────────────────────────────────

TEST_CASE("MenuBar add menu", "[view][menu]") {
    MenuBar menu;
    menu.add_menu("File", {
        {"New", [] {}, {}},
        {"Open", [] {}, {}},
    });

    REQUIRE(menu.menus().size() == 1);
    REQUIRE(menu.menus()[0].title == "File");
    REQUIRE(menu.menus()[0].items.size() == 2);
}

TEST_CASE("MenuBar set_key_mapping populates from commands", "[view][menu]") {
    KeyMapping km;
    km.add_command({1, "Save", "File", {}, [] {}});
    km.add_command({2, "Undo", "Edit", {}, [] {}});

    MenuBar menu;
    menu.set_key_mapping(km);

    REQUIRE(menu.menus().size() == 2);
}

// ── Toolbar ──────────────────────────────────────────────────────────────

TEST_CASE("Toolbar add items and separator", "[view][toolbar]") {
    Toolbar tb;
    tb.add_item({"play", "Play", "play.fill", [] {}});
    tb.add_separator();
    tb.add_item({"stop", "Stop", "stop.fill", [] {}});

    REQUIRE(tb.items().size() == 3);
    REQUIRE(tb.items()[1].is_separator);
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
