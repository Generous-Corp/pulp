// #299: Non-Apple view hosts route through host-registered
// callbacks (ScreenshotProvider, WindowHost::Factory,
// PluginViewHost::Factory). Without a registration, the APIs return
// explicit "unsupported" (empty vector / nullptr) and the has_*()
// probes let callers tell that state apart from genuine failure.
//
// The registration APIs are public on every platform; on Apple
// platforms the native impl takes precedence so installing a
// factory is a no-op — but the registration itself must remain
// safe.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/inspector_window.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/window_host.hpp>

#include <utility>

using namespace pulp::view;

namespace {

struct EmbeddedBounds {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

class EmbeddingWindowHost final : public WindowHost {
public:
    void show() override { visible_ = true; }
    void hide() override { visible_ = false; }
    bool is_visible() const override { return visible_; }
    void repaint() override { ++repaint_count_; }
    void set_close_callback(std::function<void()> cb) override {
        close_callback_ = std::move(cb);
    }
    void run_event_loop() override {}

    void* native_window_handle() const override { return window_handle_; }
    void* native_content_view_handle() const override { return content_handle_; }

    bool attach_native_child_view(void* child_view,
                                  float x,
                                  float y,
                                  float width,
                                  float height) override {
        if (!child_view) return false;
        child_ = child_view;
        bounds_ = {x, y, width, height};
        attached_ = true;
        return true;
    }

    bool set_native_child_view_bounds(void* child_view,
                                      float x,
                                      float y,
                                      float width,
                                      float height) override {
        if (!attached_ || child_view != child_) return false;
        bounds_ = {x, y, width, height};
        return true;
    }

    void detach_native_child_view(void* child_view) override {
        if (child_view == child_) {
            child_ = nullptr;
            attached_ = false;
        }
    }

    bool attached() const { return attached_; }
    void* child() const { return child_; }
    const EmbeddedBounds& bounds() const { return bounds_; }

private:
    inline static int window_sentinel_;
    inline static int content_sentinel_;

    void* window_handle_ = &window_sentinel_;
    void* content_handle_ = &content_sentinel_;
    void* child_ = nullptr;
    EmbeddedBounds bounds_{};
    std::function<void()> close_callback_;
    int repaint_count_ = 0;
    bool visible_ = false;
    bool attached_ = false;
};

class EmbeddingPluginViewHost final : public PluginViewHost {
public:
    NativeViewHandle native_handle() override { return host_handle_; }
    void attach_to_parent(NativeViewHandle parent) override { parent_ = parent; }
    void detach() override { parent_ = nullptr; attached_ = false; child_ = nullptr; }
    void repaint() override { ++repaint_count_; }
    void set_size(uint32_t width, uint32_t height) override { size_ = {width, height}; }
    Size get_size() const override { return size_; }

    bool attach_native_child_view(NativeViewHandle child_view,
                                  float x,
                                  float y,
                                  float width,
                                  float height) override {
        if (!child_view) return false;
        child_ = child_view;
        bounds_ = {x, y, width, height};
        attached_ = true;
        return true;
    }

    bool set_native_child_view_bounds(NativeViewHandle child_view,
                                      float x,
                                      float y,
                                      float width,
                                      float height) override {
        if (!attached_ || child_view != child_) return false;
        bounds_ = {x, y, width, height};
        return true;
    }

    void detach_native_child_view(NativeViewHandle child_view) override {
        if (child_view == child_) {
            child_ = nullptr;
            attached_ = false;
        }
    }

    bool attached() const { return attached_; }
    NativeViewHandle child() const { return child_; }
    const EmbeddedBounds& bounds() const { return bounds_; }

private:
    inline static int host_sentinel_;

    NativeViewHandle host_handle_ = &host_sentinel_;
    NativeViewHandle parent_ = nullptr;
    NativeViewHandle child_ = nullptr;
    EmbeddedBounds bounds_{};
    Size size_{400, 300};
    int repaint_count_ = 0;
    bool attached_ = false;
};

} // namespace

TEST_CASE("View host bridges: registration APIs are safe on all platforms",
          "[view][hosts][issue-299]") {
    clear_screenshot_provider();
    WindowHost::clear_factory();
    PluginViewHost::clear_factory();

    REQUIRE_FALSE(has_screenshot_provider());
    REQUIRE_FALSE(WindowHost::has_factory());
    REQUIRE_FALSE(PluginViewHost::has_factory());
}

TEST_CASE("Host child embedding contract is implementable by concrete hosts",
          "[view][hosts][native-child]") {
    EmbeddingWindowHost window_host;
    int window_child_sentinel = 0;
    auto child = static_cast<void*>(&window_child_sentinel);
    REQUIRE(window_host.native_window_handle() != nullptr);
    REQUIRE(window_host.native_content_view_handle() != nullptr);
    REQUIRE_FALSE(window_host.attach_native_child_view(nullptr, 1.0f, 2.0f, 3.0f, 4.0f));
    REQUIRE(window_host.attach_native_child_view(child, 1.0f, 2.0f, 3.0f, 4.0f));
    REQUIRE(window_host.attached());
    REQUIRE(window_host.child() == child);
    REQUIRE(window_host.bounds().x == 1.0f);
    REQUIRE(window_host.bounds().y == 2.0f);
    REQUIRE(window_host.bounds().width == 3.0f);
    REQUIRE(window_host.bounds().height == 4.0f);
    int wrong_window_child_sentinel = 0;
    REQUIRE_FALSE(window_host.set_native_child_view_bounds(
        static_cast<void*>(&wrong_window_child_sentinel), 5.0f, 6.0f, 7.0f, 8.0f));
    REQUIRE(window_host.set_native_child_view_bounds(child, 5.0f, 6.0f, 7.0f, 8.0f));
    REQUIRE(window_host.bounds().x == 5.0f);
    REQUIRE(window_host.bounds().y == 6.0f);
    REQUIRE(window_host.bounds().width == 7.0f);
    REQUIRE(window_host.bounds().height == 8.0f);
    window_host.detach_native_child_view(child);
    REQUIRE_FALSE(window_host.attached());

    EmbeddingPluginViewHost plugin_host;
    int plugin_child_sentinel = 0;
    auto plugin_child = static_cast<NativeViewHandle>(&plugin_child_sentinel);
    REQUIRE(plugin_host.native_handle() != nullptr);
    REQUIRE_FALSE(plugin_host.attach_native_child_view(
        nullptr, 9.0f, 10.0f, 11.0f, 12.0f));
    REQUIRE(plugin_host.attach_native_child_view(plugin_child, 9.0f, 10.0f, 11.0f, 12.0f));
    REQUIRE(plugin_host.attached());
    REQUIRE(plugin_host.child() == plugin_child);
    REQUIRE(plugin_host.bounds().x == 9.0f);
    REQUIRE(plugin_host.bounds().y == 10.0f);
    REQUIRE(plugin_host.bounds().width == 11.0f);
    REQUIRE(plugin_host.bounds().height == 12.0f);
    int wrong_plugin_child_sentinel = 0;
    REQUIRE_FALSE(plugin_host.set_native_child_view_bounds(
        static_cast<NativeViewHandle>(&wrong_plugin_child_sentinel), 13.0f, 14.0f, 15.0f, 16.0f));
    REQUIRE(plugin_host.set_native_child_view_bounds(plugin_child, 13.0f, 14.0f, 15.0f, 16.0f));
    REQUIRE(plugin_host.bounds().x == 13.0f);
    REQUIRE(plugin_host.bounds().y == 14.0f);
    REQUIRE(plugin_host.bounds().width == 15.0f);
    REQUIRE(plugin_host.bounds().height == 16.0f);
    plugin_host.detach_native_child_view(plugin_child);
    REQUIRE_FALSE(plugin_host.attached());
}

TEST_CASE("InspectorWindow show is safe when host creation returns nullptr",
          "[view][hosts][native-child]") {
    View inspected_root;
    bool factory_called = false;
    InspectorWindow inspector(
        inspected_root,
        nullptr,
        nullptr,
        [&](View& root, const WindowOptions& options) -> std::unique_ptr<WindowHost> {
            factory_called = true;
            REQUIRE(&root != &inspected_root);
            REQUIRE(options.title == "Inspector");
            return nullptr;
        });

    inspector.show();

    REQUIRE(factory_called);
    REQUIRE_FALSE(inspector.is_visible());
}

#if !defined(__APPLE__)

namespace {

class StubWindowHost final : public WindowHost {
public:
    void show() override {}
    void hide() override {}
    bool is_visible() const override { return false; }
    void repaint() override {}
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}
};

class StubPluginViewHost final : public PluginViewHost {
public:
    NativeViewHandle native_handle() override { return nullptr; }
    void attach_to_parent(NativeViewHandle) override {}
    void detach() override {}
    void repaint() override {}
    void set_size(uint32_t width, uint32_t height) override { size_ = {width, height}; }
    Size get_size() const override { return size_; }

private:
    Size size_{};
};

} // namespace

TEST_CASE("Non-Apple screenshot: no provider -> empty bytes + false file",
          "[view][hosts][issue-299]") {
    clear_screenshot_provider();
    View root;
    REQUIRE(render_to_png(root, 64, 64).empty());
    REQUIRE_FALSE(render_to_file(root, 64, 64, "/tmp/should-not-exist.png"));
}

TEST_CASE("Non-Apple screenshot: provider routes through and carries data",
          "[view][hosts][issue-299]") {
    int calls = 0;
    set_screenshot_provider([&](View&, uint32_t w, uint32_t h, float, ScreenshotBackend) {
        calls++;
        std::vector<uint8_t> fake(w * h, 0x42);
        return fake;
    });

    View root;
    auto bytes = render_to_png(root, 10, 10);
    REQUIRE(bytes.size() == 100);
    REQUIRE(bytes[0] == 0x42);
    REQUIRE(calls == 1);

    clear_screenshot_provider();
    REQUIRE_FALSE(has_screenshot_provider());
    REQUIRE(render_to_png(root, 10, 10).empty());
}

TEST_CASE("Non-Apple WindowHost::create: no factory -> nullptr",
          "[view][hosts][issue-299]") {
    WindowHost::clear_factory();
    View root;
    WindowOptions opts;
    auto win = WindowHost::create(root, opts);
    REQUIRE(win == nullptr);
}

TEST_CASE("Non-Apple PluginViewHost::create: no factory -> nullptr",
          "[view][hosts][issue-299]") {
    PluginViewHost::clear_factory();
    View root;
    PluginViewHost::Size size;
    REQUIRE(PluginViewHost::create(root, size) == nullptr);
    PluginViewHost::Options opts;
    REQUIRE(PluginViewHost::create(root, opts) == nullptr);
}

TEST_CASE("Non-Apple InspectorWindow show is safe without WindowHost factory",
          "[view][hosts][native-child]") {
    WindowHost::clear_factory();
    View inspected_root;
    InspectorWindow inspector(inspected_root);

    inspector.show();

    REQUIRE_FALSE(inspector.is_visible());
}

TEST_CASE("Non-Apple host factories propagate root host references",
          "[view][hosts][issue-651]") {
    WindowHost::clear_factory();
    PluginViewHost::clear_factory();

    View window_root;
    WindowHost::set_factory([](View&, const WindowOptions&) {
        return std::make_unique<StubWindowHost>();
    });
    auto window_host = WindowHost::create(window_root, WindowOptions{});
    REQUIRE(window_host != nullptr);
    REQUIRE(window_root.window_host() == window_host.get());
    int child_sentinel = 0;
    auto child = static_cast<void*>(&child_sentinel);
    REQUIRE_FALSE(window_host->attach_native_child_view(child, 1.0f, 2.0f, 3.0f, 4.0f));
    REQUIRE_FALSE(window_host->set_native_child_view_bounds(child, 5.0f, 6.0f, 7.0f, 8.0f));
    window_host->detach_native_child_view(child);

    View plugin_root;
    PluginViewHost::set_factory([](View&, const PluginViewHost::Options&) {
        return std::make_unique<StubPluginViewHost>();
    });
    auto plugin_host = PluginViewHost::create(plugin_root, PluginViewHost::Size{640, 360});
    REQUIRE(plugin_host != nullptr);
    REQUIRE(plugin_root.plugin_view_host() == plugin_host.get());

    int plugin_child_sentinel = 0;
    auto plugin_child = static_cast<NativeViewHandle>(&plugin_child_sentinel);
    REQUIRE_FALSE(plugin_host->attach_native_child_view(plugin_child, 1.0f, 2.0f, 3.0f, 4.0f));
    REQUIRE_FALSE(plugin_host->set_native_child_view_bounds(plugin_child, 5.0f, 6.0f, 7.0f, 8.0f));
    plugin_host->detach_native_child_view(plugin_child);

    PluginViewHost::clear_factory();
    WindowHost::clear_factory();
}

TEST_CASE("Non-Apple host factories can supply native child embedding",
          "[view][hosts][native-child]") {
    WindowHost::clear_factory();
    PluginViewHost::clear_factory();

    View window_root;
    WindowHost::set_factory([](View&, const WindowOptions&) {
        return std::make_unique<EmbeddingWindowHost>();
    });
    auto window_host = WindowHost::create(window_root, WindowOptions{});
    REQUIRE(window_host != nullptr);
    REQUIRE(window_root.window_host() == window_host.get());
    auto* embedding_window = dynamic_cast<EmbeddingWindowHost*>(window_host.get());
    REQUIRE(embedding_window != nullptr);
    REQUIRE(embedding_window->native_window_handle() != nullptr);
    REQUIRE(embedding_window->native_content_view_handle() != nullptr);

    int child_sentinel = 0;
    auto child = static_cast<void*>(&child_sentinel);
    REQUIRE(window_host->attach_native_child_view(child, 1.0f, 2.0f, 3.0f, 4.0f));
    REQUIRE(embedding_window->attached());
    REQUIRE(window_host->set_native_child_view_bounds(child, 5.0f, 6.0f, 7.0f, 8.0f));
    REQUIRE(embedding_window->bounds().x == 5.0f);
    REQUIRE(embedding_window->bounds().y == 6.0f);
    REQUIRE(embedding_window->bounds().width == 7.0f);
    REQUIRE(embedding_window->bounds().height == 8.0f);
    window_host->detach_native_child_view(child);
    REQUIRE_FALSE(embedding_window->attached());

    View plugin_root;
    PluginViewHost::set_factory([](View&, const PluginViewHost::Options&) {
        return std::make_unique<EmbeddingPluginViewHost>();
    });
    auto plugin_host = PluginViewHost::create(plugin_root, PluginViewHost::Size{640, 360});
    REQUIRE(plugin_host != nullptr);
    REQUIRE(plugin_root.plugin_view_host() == plugin_host.get());
    auto* embedding_plugin = dynamic_cast<EmbeddingPluginViewHost*>(plugin_host.get());
    REQUIRE(embedding_plugin != nullptr);
    REQUIRE(embedding_plugin->native_handle() != nullptr);

    int plugin_child_sentinel = 0;
    auto plugin_child = static_cast<NativeViewHandle>(&plugin_child_sentinel);
    REQUIRE(plugin_host->attach_native_child_view(plugin_child, 9.0f, 10.0f, 11.0f, 12.0f));
    REQUIRE(embedding_plugin->attached());
    REQUIRE(plugin_host->set_native_child_view_bounds(plugin_child, 13.0f, 14.0f, 15.0f, 16.0f));
    REQUIRE(embedding_plugin->bounds().x == 13.0f);
    REQUIRE(embedding_plugin->bounds().y == 14.0f);
    REQUIRE(embedding_plugin->bounds().width == 15.0f);
    REQUIRE(embedding_plugin->bounds().height == 16.0f);
    plugin_host->detach_native_child_view(plugin_child);
    REQUIRE_FALSE(embedding_plugin->attached());

    PluginViewHost::clear_factory();
    WindowHost::clear_factory();
}

// #313 Codex P2: providers must be invoked OUTSIDE the registration
// mutex so they can safely re-enter the bridge API (check state,
// take long actions, etc.). If the mutex were still held, calling
// set_screenshot_provider from inside a running provider would
// deadlock. This test would hang before the fix; it returns quickly
// after.
TEST_CASE("Non-Apple screenshot provider can re-enter the bridge API",
          "[view][hosts][issue-313]") {
    clear_screenshot_provider();

    bool reentered = false;
    set_screenshot_provider([&](View&, uint32_t, uint32_t, float, ScreenshotBackend) {
        // If the old code held g_provider_mu during this callback, the
        // has_screenshot_provider() call below would deadlock on a
        // recursive acquire (or, with std::mutex on some platforms,
        // UB). The fix copies the provider out before release.
        REQUIRE(has_screenshot_provider());
        reentered = true;
        return std::vector<uint8_t>{};
    });

    View root;
    (void)render_to_png(root, 1, 1);
    REQUIRE(reentered);
    clear_screenshot_provider();
}

#endif // !defined(__APPLE__)
