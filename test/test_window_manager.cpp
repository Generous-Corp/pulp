#include <catch2/catch_test_macros.hpp>
#include <pulp/view/window_manager.hpp>

using namespace pulp::view;

// ── Stub WindowHost for testing (no platform windows) ───────────────────────

class StubWindowHost : public WindowHost {
public:
    bool visible_ = false;
    bool close_requested_ = false;
    std::function<void()> close_cb_;

    void show() override { visible_ = true; }
    void hide() override { visible_ = false; }
    bool is_visible() const override { return visible_; }
    void repaint() override {}
    void request_close() override { close_requested_ = true; }
    void set_close_callback(std::function<void()> cb) override { close_cb_ = std::move(cb); }
    void run_event_loop() override {}
};

// ── Registration and lifecycle ──────────────────────────────────────────────

TEST_CASE("WindowManager registration", "[view][multiwindow]") {
    WindowManager mgr;
    REQUIRE(mgr.window_count() == 0);

    View root;
    StubWindowHost host;
    auto id = mgr.register_window(&host, &root, WindowType::main);

    REQUIRE(id > 0);
    REQUIRE(mgr.window_count() == 1);

    auto* rec = mgr.window(id);
    REQUIRE(rec != nullptr);
    REQUIRE(rec->type == WindowType::main);
    REQUIRE(rec->host == &host);
    REQUIRE(rec->root_view == &root);
    REQUIRE(rec->parent_id == 0);
    REQUIRE(rec->alive);
}

TEST_CASE("WindowManager unregister", "[view][multiwindow]") {
    WindowManager mgr;
    View root;
    StubWindowHost host;
    auto id = mgr.register_window(&host, &root, WindowType::main);

    mgr.unregister_window(id);
    REQUIRE(mgr.window_count() == 0);
    REQUIRE(mgr.window(id) == nullptr);
}

TEST_CASE("WindowManager unregister invokes close callback and ignores missing ids",
          "[view][multiwindow]") {
    WindowManager mgr;
    View root;
    StubWindowHost host;
    auto id = mgr.register_window(&host, &root, WindowType::main);

    std::vector<WindowId> closed;
    mgr.set_on_window_closed([&](WindowId closed_id) {
        closed.push_back(closed_id);
    });

    mgr.unregister_window(id);
    mgr.unregister_window(id);
    mgr.unregister_window(999);

    REQUIRE(closed == std::vector<WindowId>{id});
    REQUIRE(mgr.window_count() == 0);
}

TEST_CASE("WindowManager unique IDs", "[view][multiwindow]") {
    WindowManager mgr;
    View r1, r2, r3;
    StubWindowHost h1, h2, h3;

    auto id1 = mgr.register_window(&h1, &r1, WindowType::main);
    auto id2 = mgr.register_window(&h2, &r2, WindowType::palette);
    auto id3 = mgr.register_window(&h3, &r3, WindowType::inspector);

    REQUIRE(id1 != id2);
    REQUIRE(id2 != id3);
    REQUIRE(id1 != id3);
    REQUIRE(mgr.window_count() == 3);
}

TEST_CASE("WindowManager window_ids returns sorted IDs", "[view][multiwindow]") {
    WindowManager mgr;
    View r1, r2;
    StubWindowHost h1, h2;

    mgr.register_window(&h1, &r1, WindowType::main);
    mgr.register_window(&h2, &r2, WindowType::palette);

    auto ids = mgr.window_ids();
    REQUIRE(ids.size() == 2);
    REQUIRE(ids[0] < ids[1]);
}

// ── Parent-child relationships ──────────────────────────────────────────────

TEST_CASE("WindowManager parent-child", "[view][multiwindow]") {
    WindowManager mgr;
    View root, palette_root, inspector_root;
    StubWindowHost main_host, palette_host, inspector_host;

    auto main_id = mgr.register_window(&main_host, &root, WindowType::main);
    auto palette_id = mgr.register_window(&palette_host, &palette_root,
                                           WindowType::palette, main_id);
    mgr.register_window(&inspector_host, &inspector_root,
                         WindowType::inspector, main_id);

    auto children = mgr.children_of(main_id);
    REQUIRE(children.size() == 2);

    auto* prec = mgr.window(palette_id);
    REQUIRE(prec->parent_id == main_id);
}

// ── Cascading close ─────────────────────────────────────────────────────────

TEST_CASE("WindowManager cascading close", "[view][multiwindow]") {
    WindowManager mgr;
    View r1, r2, r3, r4;
    StubWindowHost h1, h2, h3, h4;

    auto main_id = mgr.register_window(&h1, &r1, WindowType::main);
    auto palette_id = mgr.register_window(&h2, &r2, WindowType::palette, main_id);
    mgr.register_window(&h3, &r3, WindowType::inspector, main_id);
    // Nested child of palette
    auto popup_id = mgr.register_window(&h4, &r4, WindowType::popup, palette_id);

    REQUIRE(mgr.window_count() == 4);

    // Track closed window IDs
    std::vector<WindowId> closed;
    mgr.set_on_window_closed([&](WindowId id) { closed.push_back(id); });

    // Close main → should cascade to all children
    mgr.close_window(main_id);

    REQUIRE(mgr.window_count() == 0);
    REQUIRE(h1.close_requested_);
    REQUIRE(h2.close_requested_);
    REQUIRE(h3.close_requested_);
    REQUIRE(h4.close_requested_);

    // Children should be closed before parent (depth-first)
    REQUIRE(closed.size() == 4);
    // popup_id (leaf) should come before palette_id (its parent)
    auto popup_pos = std::find(closed.begin(), closed.end(), popup_id);
    auto palette_pos = std::find(closed.begin(), closed.end(), palette_id);
    auto main_pos = std::find(closed.begin(), closed.end(), main_id);
    REQUIRE(popup_pos < palette_pos);
    REQUIRE(palette_pos < main_pos);
}

TEST_CASE("WindowManager close leaf only", "[view][multiwindow]") {
    WindowManager mgr;
    View r1, r2;
    StubWindowHost h1, h2;

    auto main_id = mgr.register_window(&h1, &r1, WindowType::main);
    auto palette_id = mgr.register_window(&h2, &r2, WindowType::palette, main_id);

    // Close only the palette
    mgr.close_window(palette_id);

    REQUIRE(mgr.window_count() == 1);
    REQUIRE(mgr.window(main_id) != nullptr);
    REQUIRE(h2.close_requested_);
    REQUIRE_FALSE(h1.close_requested_);
}

TEST_CASE("WindowManager closes windows without host or root view",
          "[view][multiwindow]") {
    WindowManager mgr;
    mgr.set_shared_theme(Theme::dark());

    auto id = mgr.register_window(nullptr, nullptr, WindowType::popup);
    auto* rec = mgr.window(id);
    REQUIRE(rec != nullptr);
    REQUIRE(rec->host == nullptr);
    REQUIRE(rec->root_view == nullptr);

    std::vector<WindowId> closed;
    mgr.set_on_window_closed([&](WindowId closed_id) {
        closed.push_back(closed_id);
    });

    mgr.close_window(id);

    REQUIRE(closed == std::vector<WindowId>{id});
    REQUIRE(mgr.window_count() == 0);
    REQUIRE(mgr.window(id) == nullptr);
}

// ── Theme propagation ───────────────────────────────────────────────────────

TEST_CASE("WindowManager theme propagation", "[view][multiwindow][theme]") {
    WindowManager mgr;
    View r1, r2;
    StubWindowHost h1, h2;

    mgr.register_window(&h1, &r1, WindowType::main);
    mgr.register_window(&h2, &r2, WindowType::palette);

    // Set shared theme
    Theme dark = Theme::dark();
    mgr.set_shared_theme(dark);

    // Both views should have the theme
    auto c1 = r1.resolve_color("bg.primary");
    auto c2 = r2.resolve_color("bg.primary");
    auto expected = dark.color("bg.primary");

    REQUIRE(expected.has_value());
    REQUIRE(c1.r == expected->r);
    REQUIRE(c1.g == expected->g);
    REQUIRE(c1.b == expected->b);
    REQUIRE(c2.r == expected->r);
    REQUIRE(c2.g == expected->g);
    REQUIRE(c2.b == expected->b);
}

TEST_CASE("WindowManager theme applied on registration", "[view][multiwindow][theme]") {
    WindowManager mgr;

    // Set theme first
    Theme light = Theme::light();
    mgr.set_shared_theme(light);

    // Register window after — theme should be applied
    View root;
    StubWindowHost host;
    mgr.register_window(&host, &root, WindowType::main);

    auto expected = light.color("bg.primary");
    auto actual = root.resolve_color("bg.primary");
    REQUIRE(expected.has_value());
    REQUIRE(actual.r == expected->r);
}

// ── Inter-window messaging ──────────────────────────────────────────────────

TEST_CASE("WindowManager send message", "[view][multiwindow]") {
    WindowManager mgr;
    View r1, r2;
    StubWindowHost h1, h2;

    auto id1 = mgr.register_window(&h1, &r1, WindowType::main);
    auto id2 = mgr.register_window(&h2, &r2, WindowType::inspector);

    WindowMessage received;
    mgr.set_message_handler(id2, [&](const WindowMessage& msg) {
        received = msg;
    });

    WindowMessage msg;
    msg.type = "param_changed";
    msg.payload = R"({"id":"gain","value":0.5})";
    msg.source = id1;

    mgr.send_message(id2, msg);

    REQUIRE(received.type == "param_changed");
    REQUIRE(received.source == id1);
    REQUIRE(received.payload.find("gain") != std::string::npos);
}

TEST_CASE("WindowManager broadcast message", "[view][multiwindow]") {
    WindowManager mgr;
    View r1, r2, r3;
    StubWindowHost h1, h2, h3;

    auto id1 = mgr.register_window(&h1, &r1, WindowType::main);
    auto id2 = mgr.register_window(&h2, &r2, WindowType::palette);
    auto id3 = mgr.register_window(&h3, &r3, WindowType::inspector);

    int count = 0;
    auto handler = [&](const WindowMessage&) { ++count; };
    mgr.set_message_handler(id1, handler);
    mgr.set_message_handler(id2, handler);
    mgr.set_message_handler(id3, handler);

    WindowMessage msg;
    msg.type = "theme_changed";
    mgr.broadcast_message(msg);

    REQUIRE(count == 3);
}

TEST_CASE("WindowManager message handler removed on unregister", "[view][multiwindow]") {
    WindowManager mgr;
    View root;
    StubWindowHost host;

    auto id = mgr.register_window(&host, &root, WindowType::main);

    bool called = false;
    mgr.set_message_handler(id, [&](const WindowMessage&) { called = true; });

    mgr.unregister_window(id);

    // Sending to unregistered window should be a no-op
    WindowMessage msg;
    msg.type = "test";
    mgr.send_message(id, msg);
    REQUIRE_FALSE(called);
}

TEST_CASE("WindowManager send and broadcast skip missing handlers",
          "[view][multiwindow]") {
    WindowManager mgr;
    View r1, r2;
    StubWindowHost h1, h2;

    auto id1 = mgr.register_window(&h1, &r1, WindowType::main);
    auto id2 = mgr.register_window(&h2, &r2, WindowType::palette);

    int count = 0;
    mgr.set_message_handler(id1, [&](const WindowMessage& msg) {
        REQUIRE(msg.type == "ping");
        ++count;
    });

    WindowMessage msg;
    msg.type = "ping";
    mgr.send_message(id2, msg);
    REQUIRE(count == 0);

    mgr.broadcast_message(msg);
    REQUIRE(count == 1);
}

TEST_CASE("WindowManager ignores handlers for unknown windows",
          "[view][multiwindow][coverage][phase3]") {
    WindowManager mgr;
    int count = 0;

    mgr.set_message_handler(42, [&](const WindowMessage&) {
        ++count;
    });

    WindowMessage msg;
    msg.type = "orphan";
    mgr.send_message(42, msg);
    mgr.broadcast_message(msg);

    REQUIRE(count == 0);
}

// ── Window state save / restore ─────────────────────────────────────────────

TEST_CASE("WindowManager state save and restore", "[view][multiwindow]") {
    WindowManager mgr;
    View r1, r2;
    StubWindowHost h1, h2;

    auto id1 = mgr.register_window(&h1, &r1, WindowType::main);
    auto id2 = mgr.register_window(&h2, &r2, WindowType::palette);

    // Set some state
    WindowState s1;
    s1.x = 100; s1.y = 200; s1.width = 800; s1.height = 600; s1.screen_id = 0;
    mgr.restore_state(id1, s1);

    WindowState s2;
    s2.x = 900; s2.y = 50; s2.width = 300; s2.height = 400; s2.screen_id = 1;
    mgr.restore_state(id2, s2);

    auto states = mgr.save_all_states();
    REQUIRE(states.size() == 2);
    REQUIRE(states[id1].x == 100);
    REQUIRE(states[id1].width == 800);
    REQUIRE(states[id2].screen_id == 1);
    REQUIRE(states[id2].height == 400);
}

// ── GPU device sharing ──────────────────────────────────────────────────────

TEST_CASE("WindowManager GPU device sharing", "[view][multiwindow]") {
    WindowManager mgr;
    REQUIRE(mgr.shared_gpu_device() == nullptr);

    int fake_device = 42;
    mgr.set_shared_gpu_device(&fake_device);
    REQUIRE(mgr.shared_gpu_device() == &fake_device);
}

// ── Screen enumeration ──────────────────────────────────────────────────────

TEST_CASE("WindowManager screen enumeration", "[view][multiwindow]") {
    auto screens = WindowManager::available_screens();
    REQUIRE_FALSE(screens.empty());

    // At least one screen should be primary
    bool has_primary = false;
    for (auto& s : screens) {
        if (s.is_primary) has_primary = true;
        REQUIRE(s.width > 0);
        REQUIRE(s.height > 0);
        REQUIRE(s.scale_factor > 0);
    }
    REQUIRE(has_primary);

    auto primary = WindowManager::primary_screen();
    REQUIRE(primary.is_primary);
    REQUIRE(primary.width > 0);
}

// ── Multi-window support query ──────────────────────────────────────────────

TEST_CASE("WindowManager multi-window supported", "[view][multiwindow]") {
    // On macOS (where tests run), multi-window should be supported
#if !TARGET_OS_IPHONE
    REQUIRE(WindowManager::is_multi_window_supported());
#endif
}

// ── Graceful degradation ────────────────────────────────────────────────────

TEST_CASE("WindowManager graceful operations on empty manager", "[view][multiwindow]") {
    WindowManager mgr;

    // These should all be no-ops, not crash
    mgr.unregister_window(999);
    mgr.close_window(999);

    REQUIRE(mgr.window(999) == nullptr);
    REQUIRE(mgr.window_ids().empty());
    REQUIRE(mgr.children_of(999).empty());
    REQUIRE(mgr.save_all_states().empty());

    WindowMessage msg;
    msg.type = "test";
    mgr.send_message(999, msg);       // No-op
    mgr.broadcast_message(msg);       // No-op (no windows)

    WindowState state;
    mgr.restore_state(999, state);    // No-op (window not found)
}

// ── Window types ────────────────────────────────────────────────────────────

TEST_CASE("WindowManager all window types", "[view][multiwindow]") {
    WindowManager mgr;

    struct {
        WindowType type;
        const char* name;
    } types[] = {
        { WindowType::main, "main" },
        { WindowType::palette, "palette" },
        { WindowType::inspector, "inspector" },
        { WindowType::popup, "popup" },
        { WindowType::dialog, "dialog" },
    };

    std::vector<View> roots(5);
    std::vector<StubWindowHost> hosts(5);

    for (size_t i = 0; i < 5; ++i) {
        auto id = mgr.register_window(&hosts[i], &roots[i], types[i].type);
        auto* rec = mgr.window(id);
        REQUIRE(rec->type == types[i].type);
    }

    REQUIRE(mgr.window_count() == 5);
}

TEST_CASE("WindowManager replacing a message handler drops the previous callback",
          "[view][multiwindow][coverage][phase3]") {
    WindowManager mgr;
    View root;
    StubWindowHost host;
    auto id = mgr.register_window(&host, &root, WindowType::main);

    int first = 0;
    int second = 0;
    mgr.set_message_handler(id, [&](const WindowMessage&) { ++first; });
    mgr.set_message_handler(id, [&](const WindowMessage& msg) {
        REQUIRE(msg.type == "replace");
        ++second;
    });

    WindowMessage msg;
    msg.type = "replace";
    mgr.send_message(id, msg);
    REQUIRE(first == 0);
    REQUIRE(second == 1);
}

TEST_CASE("WindowManager saved state captures visibility and restore preserves other windows",
          "[view][multiwindow][coverage][phase3]") {
    WindowManager mgr;
    View first_root;
    View second_root;
    StubWindowHost first_host;
    StubWindowHost second_host;
    auto first = mgr.register_window(&first_host, &first_root, WindowType::main);
    auto second = mgr.register_window(&second_host, &second_root, WindowType::dialog);

    WindowState hidden;
    hidden.x = 12.0f;
    hidden.y = 34.0f;
    hidden.width = 640.0f;
    hidden.height = 480.0f;
    hidden.screen_id = 7;
    hidden.is_visible = false;
    mgr.restore_state(first, hidden);

    WindowState ignored;
    ignored.x = 99.0f;
    mgr.restore_state(99999, ignored);

    auto states = mgr.save_all_states();
    REQUIRE(states.size() == 2);
    REQUIRE(states.at(first).x == 12.0f);
    REQUIRE(states.at(first).screen_id == 7);
    REQUIRE_FALSE(states.at(first).is_visible);
    REQUIRE(states.at(second).is_visible);
    REQUIRE(states.at(second).width == 400.0f);
}

TEST_CASE("WindowManager shared theme getter and later registration stay in sync",
          "[view][multiwindow][coverage][phase3]") {
    WindowManager mgr;
    Theme theme = Theme::dark();
    theme.colors["bg.primary"] = color_from_hex(0x123456);
    mgr.set_shared_theme(theme);

    REQUIRE(mgr.shared_theme().color("bg.primary")->r8() == 0x12);

    View root;
    StubWindowHost host;
    mgr.register_window(&host, &root, WindowType::palette);
    auto resolved = root.resolve_color("bg.primary");
    REQUIRE(resolved.r8() == 0x12);
    REQUIRE(resolved.g8() == 0x34);
    REQUIRE(resolved.b8() == 0x56);
}
