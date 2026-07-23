#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <pulp/format/gpu_host_select.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/runtime/message_channel.hpp>
#include <pulp/state/store.hpp>
#include <pulp/state/listener_token.hpp>
#include <pulp/view/auto_ui.hpp>
#include <pulp/view/design_frame_view.hpp>
#include <pulp/view/host_param_surface.hpp>
#include <pulp/view/scripted_ui.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/canvas/canvas.hpp>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace pulp;

namespace {

int set_env_var(const char* name, const char* value) {
#if defined(_WIN32)
    return _putenv_s(name, value);
#else
    return ::setenv(name, value, 1);
#endif
}

int unset_env_var(const char* name) {
#if defined(_WIN32)
    return _putenv_s(name, "");
#else
    return ::unsetenv(name);
#endif
}

class ScopedEnvVar {
public:
    explicit ScopedEnvVar(const char* name)
        : name_(name) {
        if (const char* value = std::getenv(name)) previous_ = value;
    }

    ~ScopedEnvVar() {
        if (previous_) {
            static_cast<void>(set_env_var(name_.c_str(), previous_->c_str()));
        } else {
            static_cast<void>(unset_env_var(name_.c_str()));
        }
    }

    void set(const char* value) const {
        REQUIRE(set_env_var(name_.c_str(), value) == 0);
    }

    void unset() const {
        REQUIRE(unset_env_var(name_.c_str()) == 0);
    }

private:
    std::string name_;
    std::optional<std::string> previous_;
};

class StubProcessor : public format::Processor {
public:
    int opened_count = 0;
    int closed_count = 0;
    int resize_count = 0;
    uint32_t last_w = 0, last_h = 0;
    std::unique_ptr<view::View> custom_view;

    format::PluginDescriptor descriptor() const override {
        return {"Stub", "Acme", "com.acme.stub", "1.0.0", format::PluginCategory::Effect};
    }
    void define_parameters(state::StateStore& s) override {
        s.add_parameter({1, "Gain", "dB", {-60.0f, 12.0f, 0.0f}});
    }
    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>&, const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext&) override {}

    format::ViewSize view_size() const override {
        return {480, 320, 320, 240, 1024, 768};
    }

    std::unique_ptr<view::View> create_view() override {
        return std::move(custom_view);
    }

    void on_view_opened(view::View&) override { ++opened_count; }
    void on_view_closed(view::View&) override { ++closed_count; }
    void on_view_resized(view::View&, uint32_t w, uint32_t h) override {
        ++resize_count;
        last_w = w;
        last_h = h;
    }
};

class ScriptedCustomViewProcessor : public StubProcessor {
public:
    view::ScriptedUiSession* active_scripted_ui() override {
        return scripted_session.get();
    }

    const view::ScriptedUiSession* active_scripted_ui() const override {
        return scripted_session.get();
    }

    std::unique_ptr<view::View> create_view() override {
        auto root = std::make_unique<view::View>();
        scripted_session = std::make_unique<view::ScriptedUiSession>(
            *root, state(), view::ScriptedUiOptions{});
        return root;
    }

    std::unique_ptr<view::ScriptedUiSession> scripted_session;
};

// Uses the AutoUi default editor with NO processor-declared size (unlike
// StubProcessor, which overrides view_size()). Exercises the seam where the
// bridge adopts AutoUi's fitting size and makes the editor proportionally
// resizable.
class DefaultAutoUiProcessor : public StubProcessor {
public:
    int param_count = 7;
    void define_parameters(state::StateStore& s) override {
        for (int i = 1; i <= param_count; ++i) {
            s.add_parameter({static_cast<uint32_t>(i), "P" + std::to_string(i),
                             "", {0.0f, 1.0f, 0.5f}});
        }
    }
    // Fall through to the SDK default (no explicit size) so AutoUi fills it in.
    format::ViewSize view_size() const override {
        return format::Processor::view_size();
    }
};

// Uses AutoUi but declares a custom editor_size(). The SDK default view_size()
// surfaces that as a non-default preferred with no min/aspect — the bridge must
// treat it as an explicit choice and NOT substitute the AutoUi fit.
class CustomEditorSizeProcessor : public DefaultAutoUiProcessor {
public:
    std::pair<uint32_t, uint32_t> editor_size() const override { return {520, 360}; }
};

} // namespace

TEST_CASE("ViewBridge sizes the AutoUi default editor to fit its parameters",
          "[view_bridge][auto_ui]") {
    DefaultAutoUiProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    const auto fit = view::AutoUi::preferred_size(store);

    format::ViewBridge bridge(p, store);
    REQUIRE(bridge.open());
    bridge.notify_attached();

    const auto& hints = bridge.size_hints();
    // Preferred adopts the AutoUi fit — opens large enough to show all 7 knobs.
    CHECK(hints.preferred_width == fit.width);
    CHECK(hints.preferred_height == fit.height);
    CHECK(bridge.width() == fit.width);
    CHECK(bridge.height() == fit.height);
    // Derived bounds make it resizable + aspect-locked, so the adapters pin the
    // design viewport and the grid scales uniformly (min-clamped, no truncation).
    CHECK(hints.min_width > 0);
    CHECK(hints.min_height > 0);
    CHECK(hints.aspect_ratio > 0.0);
    CHECK(format::should_pin_design_viewport(hints));

    bridge.close();
}

TEST_CASE("ViewBridge keeps a processor-declared size over the AutoUi fit",
          "[view_bridge][auto_ui]") {
    // StubProcessor uses AutoUi (create_view returns null) but declares an
    // explicit view_size() with min/max bounds. That declaration must win — the
    // AutoUi fit only fills the otherwise-unset default.
    StubProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store);
    REQUIRE(bridge.open());
    const auto& hints = bridge.size_hints();
    CHECK(hints.preferred_width == 480);
    CHECK(hints.preferred_height == 320);
    CHECK(hints.min_width == 320);
    CHECK(hints.max_width == 1024);
    bridge.close();
}

TEST_CASE("ViewBridge re-applies the AutoUi fit on close + reopen",
          "[view_bridge][auto_ui]") {
    // Daniel's exact repro: the default editor fit correctly on the FIRST open,
    // then reset/clipped on a later close + reopen. The fit is recomputed each
    // open() (gated on the untouched-default size), so it must re-apply
    // identically on the second open — not silently drop back to 400x300.
    DefaultAutoUiProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);
    const auto fit = view::AutoUi::preferred_size(store);

    format::ViewBridge bridge(p, store);

    auto assert_fitted = [&](const char* which) {
        INFO(which);
        const auto& h = bridge.size_hints();
        CHECK(h.preferred_width == fit.width);
        CHECK(h.preferred_height == fit.height);
        CHECK(h.min_width > 0);
        CHECK(h.min_height > 0);
        CHECK(h.aspect_ratio > 0.0);
        CHECK(format::should_pin_design_viewport(h));
    };

    REQUIRE(bridge.open());
    bridge.notify_attached();
    assert_fitted("first open");
    bridge.close();

    // Second open — the failure Daniel saw was here.
    REQUIRE(bridge.open());
    bridge.notify_attached();
    assert_fitted("second open");

    // Layout probe on the REOPENED view: at the design size the content fits
    // (no scroll needed → nothing clipped) and the first tile sits at the scroll
    // origin, reachable.
    auto* root = bridge.view();
    REQUIRE(root != nullptr);
    root->set_bounds({0.0f, 0.0f, static_cast<float>(fit.width),
                      static_cast<float>(fit.height)});
    root->layout_children();
    auto* body = dynamic_cast<view::ScrollView*>(root->child_at(1));
    REQUIRE(body != nullptr);
    CHECK(body->content_size().height <= body->local_bounds().height + 1.0f);
    auto* grid = body->child_at(0);
    REQUIRE(grid->child_count() == p.param_count);
    CHECK(grid->child_at(0)->bounds().y >= -0.5f);
    CHECK(grid->child_at(0)->bounds().y < 1.0f);
    bridge.close();
}

TEST_CASE("ViewBridge keeps a custom editor_size() over the AutoUi fit",
          "[view_bridge][auto_ui]") {
    CustomEditorSizeProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store);
    REQUIRE(bridge.open());
    const auto& hints = bridge.size_hints();
    // The explicit 520x360 wins; AutoUi does not override it.
    CHECK(hints.preferred_width == 520);
    CHECK(hints.preferred_height == 360);
    bridge.close();
}

TEST_CASE("ViewBridge falls back to AutoUi when create_view returns nullptr", "[view_bridge]") {
    StubProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store);
    REQUIRE_FALSE(bridge.is_open());
    REQUIRE(bridge.view_count() == 0);
    REQUIRE(bridge.size_hints().preferred_width == 480);
    REQUIRE(bridge.size_hints().min_height == 240);

    std::string err;
    REQUIRE(bridge.open(&err));
    bridge.notify_attached();
    REQUIRE(bridge.is_open());
    REQUIRE(bridge.view() != nullptr);
    REQUIRE(bridge.view_count() == 1);
    REQUIRE(p.opened_count == 1);

    bridge.resize(600, 400);
    REQUIRE(p.resize_count == 1);
    REQUIRE(p.last_w == 600);
    REQUIRE(p.last_h == 400);
    REQUIRE(bridge.width() == 600);

    bridge.close();
    REQUIRE_FALSE(bridge.is_open());
    REQUIRE(p.closed_count == 1);
}

TEST_CASE("ViewBridge honors custom create_view()", "[view_bridge]") {
    StubProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);
    auto custom = std::make_unique<view::View>();
    auto* raw = custom.get();
    p.custom_view = std::move(custom);

    format::ViewBridge bridge(p, store);
    REQUIRE(bridge.open());
    bridge.notify_attached();
    REQUIRE(bridge.view() == raw);
    REQUIRE_FALSE(bridge.uses_script_ui());
}

TEST_CASE("ViewBridge detects processor-owned scripted custom views",
          "[view_bridge][scripted-ui]") {
    ScriptedCustomViewProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store);
    REQUIRE(bridge.open());
    REQUIRE(bridge.uses_script_ui());
    REQUIRE(p.scripted_session != nullptr);
    REQUIRE(bridge.scripted_ui() == p.scripted_session.get());

    const auto& const_bridge = bridge;
    REQUIRE(const_bridge.scripted_ui() == p.scripted_session.get());
}

TEST_CASE("Processor scripted UI accessors default to null", "[view_bridge][scripted-ui]") {
    StubProcessor p;
    REQUIRE(p.active_scripted_ui() == nullptr);

    const auto& const_processor = p;
    REQUIRE(const_processor.active_scripted_ui() == nullptr);
}

TEST_CASE("ViewBridge primary helpers are idempotent and role-aware",
          "[view_bridge][issue-493]") {
    StubProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    auto custom = std::make_unique<view::View>();
    auto* raw = custom.get();
    p.custom_view = std::move(custom);

    format::ViewBridge::Options options;
    options.role = format::ViewRole::Inspector;
    format::ViewBridge bridge(p, store, options);

    REQUIRE(bridge.role() == format::ViewRole::Inspector);
    REQUIRE(bridge.view_at(0) == nullptr);
    REQUIRE(bridge.role_at(0) == format::ViewRole::Editor);

    REQUIRE(bridge.open());
    REQUIRE(bridge.view() == raw);
    REQUIRE(bridge.view_count() == 1);
    REQUIRE(bridge.role_at(0) == format::ViewRole::Inspector);
    REQUIRE(bridge.view_at(1) == nullptr);
    REQUIRE(bridge.role_at(1) == format::ViewRole::Editor);

    p.custom_view = std::make_unique<view::View>();
    REQUIRE(bridge.open());
    REQUIRE(bridge.view() == raw);
    REQUIRE(p.custom_view != nullptr);

    auto released = bridge.release_view();
    REQUIRE(released.get() == raw);
    REQUIRE(bridge.view() == raw);
    REQUIRE(bridge.view_count() == 1);

    auto second_release = bridge.release_view();
    REQUIRE(second_release == nullptr);

    bridge.close();
    REQUIRE_FALSE(bridge.is_open());
    REQUIRE(bridge.view_count() == 0);
    REQUIRE(p.closed_count == 0);
}

TEST_CASE("ViewBridge supports secondary views", "[view_bridge]") {
    StubProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store);
    REQUIRE(bridge.open());
    bridge.notify_attached();
    REQUIRE(bridge.view_count() == 1);

    auto inspector = std::make_unique<view::View>();
    auto* insp = bridge.attach_secondary_view(std::move(inspector), format::ViewRole::Inspector);
    REQUIRE(insp != nullptr);
    REQUIRE(bridge.view_count() == 2);
    REQUIRE(bridge.view_at(0) == bridge.view());
    REQUIRE(bridge.view_at(1) == insp);
    REQUIRE(bridge.role_at(1) == format::ViewRole::Inspector);

    REQUIRE(bridge.detach_secondary_view(insp));
    REQUIRE(bridge.view_count() == 1);
    REQUIRE_FALSE(bridge.detach_secondary_view(insp));
}

TEST_CASE("ViewBridge secondary helpers handle nulls, bounds, and close cleanup",
          "[view_bridge][issue-493]") {
    StubProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store);
    REQUIRE(bridge.attach_secondary_view(nullptr, format::ViewRole::Remote) == nullptr);
    REQUIRE(bridge.view_count() == 0);

    REQUIRE(bridge.open());
    auto* primary = bridge.view();

    auto secondary = std::make_unique<view::View>();
    auto* secondary_raw = secondary.get();
    auto* attached = bridge.attach_secondary_view(std::move(secondary), format::ViewRole::Remote);
    REQUIRE(attached == secondary_raw);
    REQUIRE(bridge.view_count() == 2);
    REQUIRE(bridge.view_at(0) == primary);
    REQUIRE(bridge.view_at(1) == secondary_raw);
    REQUIRE(bridge.role_at(1) == format::ViewRole::Remote);
    REQUIRE(bridge.view_at(2) == nullptr);
    REQUIRE(bridge.role_at(2) == format::ViewRole::Editor);
    REQUIRE_FALSE(bridge.detach_secondary_view(nullptr));

    bridge.close();
    REQUIRE_FALSE(bridge.is_open());
    REQUIRE(bridge.view_count() == 0);
    REQUIRE(bridge.view_at(0) == nullptr);
    REQUIRE(bridge.role_at(0) == format::ViewRole::Editor);
    REQUIRE_FALSE(bridge.detach_secondary_view(secondary_raw));
}

TEST_CASE("ViewBridge defers on_view_opened until notify_attached", "[view_bridge]") {
    StubProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store);
    REQUIRE(bridge.open());
    // open() must NOT fire on_view_opened — only notify_attached() does.
    REQUIRE(p.opened_count == 0);

    // Resize before attach is a no-op for lifecycle (no on_view_resized dispatched).
    bridge.resize(500, 300);
    REQUIRE(p.resize_count == 0);

    bridge.notify_attached();
    REQUIRE(p.opened_count == 1);

    // Second notify_attached is idempotent.
    bridge.notify_attached();
    REQUIRE(p.opened_count == 1);

    bridge.resize(600, 400);
    REQUIRE(p.resize_count == 1);

    bridge.close();
    REQUIRE(p.closed_count == 1);
}

TEST_CASE("ViewBridge stores detached resize state and resets on reopen",
          "[view_bridge]") {
    StubProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store);
    REQUIRE(bridge.width() == 480);
    REQUIRE(bridge.height() == 320);

    bridge.resize(700, 500);
    REQUIRE(bridge.width() == 700);
    REQUIRE(bridge.height() == 500);
    REQUIRE(p.resize_count == 0);

    REQUIRE(bridge.open());
    REQUIRE(bridge.width() == 480);
    REQUIRE(bridge.height() == 320);

    bridge.resize(900, 600);
    REQUIRE(bridge.width() == 900);
    REQUIRE(bridge.height() == 600);
    REQUIRE(p.resize_count == 0);

    bridge.notify_attached();
    bridge.resize(1024, 768);
    REQUIRE(p.resize_count == 1);
    REQUIRE(p.last_w == 1024);
    REQUIRE(p.last_h == 768);

    bridge.close();
    REQUIRE(bridge.width() == 1024);
    REQUIRE(bridge.height() == 768);

    REQUIRE(bridge.open());
    REQUIRE(bridge.width() == 480);
    REQUIRE(bridge.height() == 320);
}

TEST_CASE("ViewBridge close without attach does not fire on_view_closed", "[view_bridge]") {
    // Simulates: adapter called open(), then host attach failed, so
    // notify_attached() was never invoked. close() must NOT fire
    // on_view_closed because on_view_opened never fired — keeps
    // open/close dispatch balanced.
    StubProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store);
    REQUIRE(bridge.open());
    REQUIRE(p.opened_count == 0);

    bridge.close();
    REQUIRE(p.closed_count == 0);
}

TEST_CASE("ViewBridge reports null remote channels without blocking later open",
          "[view_bridge][issue-493]") {
    StubProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store);
    REQUIRE(bridge.last_error().empty());

    std::unique_ptr<runtime::MessageChannel> channel;
    REQUIRE(bridge.attach_remote_channel(std::move(channel), "missing") == nullptr);
    REQUIRE(bridge.last_error() == "attach_remote_channel: null channel");
    REQUIRE_FALSE(bridge.detach_remote(nullptr));

    REQUIRE(bridge.open());
    REQUIRE(bridge.last_error().empty());
    REQUIRE(bridge.is_open());
}

// Simulates each format adapter's call sequence against ViewBridge and
// asserts the same on_view_opened → on_view_resized* → on_view_closed
// ordering fires regardless of which adapter-specific flow is used.
// These are unit-level cross-format invariants; the real cross-host
// harness (loading .vst3/.clap/.component bundles) is a separate track
// documented in the planning next-features file.
TEST_CASE("ViewBridge cross-format lifecycle invariants", "[view_bridge][cross_format]") {
    auto run_scenario = [](std::function<void(format::ViewBridge&, StubProcessor&)> adapter_flow,
                           int expected_opened, int expected_closed, int expected_resized) {
        StubProcessor p;
        state::StateStore store;
        p.set_state_store(&store);
        p.define_parameters(store);
        format::ViewBridge bridge(p, store);
        adapter_flow(bridge, p);
        REQUIRE(p.opened_count == expected_opened);
        REQUIRE(p.closed_count == expected_closed);
        REQUIRE(p.resize_count == expected_resized);
    };

    SECTION("VST3-style: attached → resize → removed") {
        run_scenario([](format::ViewBridge& b, StubProcessor&) {
            REQUIRE(b.open());
            b.notify_attached();     // CPluginView::attached success
            b.resize(700, 500);      // CPluginView::onSize
            b.close();               // CPluginView::removed
        }, 1, 1, 1);
    }

    SECTION("CLAP-style: gui_create → gui_set_parent → gui_set_size → gui_destroy") {
        run_scenario([](format::ViewBridge& b, StubProcessor&) {
            REQUIRE(b.open());       // gui_create
            b.notify_attached();     // gui_set_parent succeeded on supported API
            b.resize(1000, 600);     // gui_set_size
            b.close();               // gui_destroy
        }, 1, 1, 1);
    }

    SECTION("AU v2-style: uiViewForAudioUnit → dealloc") {
        run_scenario([](format::ViewBridge& b, StubProcessor&) {
            REQUIRE(b.open());
            b.notify_attached();     // PluginViewHost attached to NSView
            b.close();               // owner dealloc
        }, 1, 1, 0);
    }

    SECTION("AU v3-style: viewDidLoad → viewDidLayoutSubviews → dealloc") {
        run_scenario([](format::ViewBridge& b, StubProcessor&) {
            REQUIRE(b.open());
            b.notify_attached();     // PluginViewHost::attach_to_parent in viewDidLoad
            b.resize(500, 375);      // viewDidLayoutSubviews
            b.resize(600, 450);
            b.close();               // dealloc
        }, 1, 1, 2);
    }

    SECTION("Standalone-style: open → release_view → notify_attached → close") {
        StubProcessor p;
        state::StateStore store;
        p.set_state_store(&store);
        p.define_parameters(store);
        format::ViewBridge bridge(p, store);

        REQUIRE(bridge.open());
        auto released = bridge.release_view();
        REQUIRE(released != nullptr);
        REQUIRE(bridge.view() == released.get());   // raw ptr retained
        bridge.notify_attached();
        bridge.resize(820, 640);
        REQUIRE(p.resize_count == 1);
        bridge.close();
        REQUIRE(p.opened_count == 1);
        REQUIRE(p.closed_count == 1);
        // Caller still owns `released` here; bridge no longer touches it.
    }

    SECTION("Failed-attach: open → close (no on_view_opened)") {
        run_scenario([](format::ViewBridge& b, StubProcessor&) {
            REQUIRE(b.open());
            // Adapter's attach_to_parent / CPluginView::attached returned failure
            // — notify_attached was never called.
            b.close();
        }, 0, 0, 0);
    }
}

TEST_CASE("ViewBridge destructor closes view", "[view_bridge]") {
    StubProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    {
        format::ViewBridge bridge(p, store);
        REQUIRE(bridge.open());
        bridge.notify_attached();
        REQUIRE(p.opened_count == 1);
    }
    REQUIRE(p.closed_count == 1);
}

TEST_CASE("ViewBridge tolerates the host freeing the Processor before the bridge",
          "[view_bridge][crash][lifecycle]") {
    // AU gives the audio unit (the Processor) and the view controller (which
    // owns this bridge) independent, host-ordered lifetimes. Ableton Live 12
    // freed the Processor while the editor bridge was still alive and its
    // display-link idle pump was firing -> EXC_BAD_ACCESS inside the pump. Every
    // processor_ dereference reachable from the pump or teardown must become a
    // no-op once the adapter reports the Processor gone. Under a sanitizer build
    // this is a heap-use-after-free if any deref is left unguarded -- which is
    // how this class of bug is caught, and why the earlier idle-pump test (which
    // only freed the BRIDGE) missed it.
    state::StateStore store;
    auto proc = std::make_unique<StubProcessor>();
    proc->set_state_store(&store);
    proc->define_parameters(store);

    format::ViewBridge bridge(*proc, store);
    REQUIRE(bridge.open());
    bridge.notify_attached();  // editor open + attached, as in a host

    auto pump = format::make_scripted_idle_pump(bridge);

    // The adapter's contract: signal death BEFORE freeing the Processor.
    bridge.notify_processor_destroyed();
    proc.reset();  // Processor gone; bridge + store still alive

    // A display-link idle tick fires after the Processor is gone. Every path the
    // pump touches -- poll_editor_reload, scripted_ui -- must be a no-op.
    pump();
    REQUIRE_FALSE(bridge.poll_editor_reload());
    REQUIRE(bridge.scripted_ui() == nullptr);
    // ~ViewBridge runs at scope exit: close() must not call on_view_closed on the
    // freed Processor either.
}

// item 1.3: in-DAW editors enable scripted-UI hot reload only when the developer
// opts in via PULP_DEV_HOT_RELOAD; default (unset) is OFF so a shipping plugin
// never watches + reloads from disk inside a host.
TEST_CASE("dev_editor_hot_reload_enabled honors PULP_DEV_HOT_RELOAD", "[format][view-bridge][issue-1-3]") {
    ScopedEnvVar hot_reload("PULP_DEV_HOT_RELOAD");

    hot_reload.unset();
    CHECK_FALSE(format::dev_editor_hot_reload_enabled());     // default OFF

    for (const char* on : {"1", "true", "yes", "T", "Y"}) {
        hot_reload.set(on);
        CHECK(format::dev_editor_hot_reload_enabled());
    }
    for (const char* off : {"0", "false", "no", ""}) {
        hot_reload.set(off);
        CHECK_FALSE(format::dev_editor_hot_reload_enabled());
    }
}

// ── Live editor reload (live-swap 1.9) ───────────────────────────────────────
// A processor whose editor rebuilds in place when its logic hot-swaps: it
// reports supports_editor_reload() and bumps editor_reload_generation(), and its
// create_view() returns different content per "variant". The ViewBridge must
// rebuild the OPEN editor into the SAME root View object (the host holds it by
// reference) when poll_editor_reload() sees the generation change.
namespace {
class ReloadingStubProcessor : public StubProcessor {
public:
    std::uint64_t gen = 0;
    int variant = 0;  // 0 = A (blue/"A"), 1 = B (red/"B")

    bool supports_editor_reload() const override { return true; }
    std::uint64_t editor_reload_generation() const override { return gen; }

    std::unique_ptr<view::View> create_view() override {
        auto root = std::make_unique<view::View>();
        if (variant == 0) {
            root->set_background_color(canvas::Color::rgba8(0, 0, 255, 255));  // blue
            root->add_child(std::make_unique<view::Label>("A"));
        } else {
            root->set_background_color(canvas::Color::rgba8(255, 0, 0, 255));  // red
            root->add_child(std::make_unique<view::Label>("B"));
        }
        return root;
    }

    // Simulate a hot-swap: new logic content + a bumped generation.
    void hot_swap_to(int v) { variant = v; ++gen; }
};
}  // namespace

TEST_CASE("ViewBridge rebuilds the open editor in place on reload", "[view_bridge][reload][issue-1_9]") {
    state::StateStore store;
    ReloadingStubProcessor proc;
    proc.define_parameters(store);

    format::ViewBridge bridge(proc, store);
    REQUIRE(bridge.open());

    view::View* root = bridge.view();  // the stable root the host references
    REQUIRE(root != nullptr);
    REQUIRE(root->child_count() == 1);
    REQUIRE(root->has_background_color());
    const auto bg_a = root->background_color();
    auto* label_a = dynamic_cast<view::Label*>(root->child_at(0));
    REQUIRE(label_a != nullptr);
    REQUIRE(label_a->text() == "A");

    // No reload yet → poll is a no-op and the view is untouched.
    REQUIRE_FALSE(bridge.poll_editor_reload());
    REQUIRE(bridge.view() == root);
    REQUIRE(dynamic_cast<view::Label*>(root->child_at(0))->text() == "A");

    // Hot-swap the logic (new create_view content + bumped generation).
    proc.hot_swap_to(1);
    REQUIRE(bridge.poll_editor_reload());  // a rebuild happened

    // SAME root object (the host's View& stays valid) but NEW content.
    REQUIRE(bridge.view() == root);
    REQUIRE(root->child_count() == 1);
    auto* label_b = dynamic_cast<view::Label*>(root->child_at(0));
    REQUIRE(label_b != nullptr);
    REQUIRE(label_b->text() == "B");
    const auto bg_b = root->background_color();
    REQUIRE((bg_b.r != bg_a.r || bg_b.b != bg_a.b));  // background changed (blue → red)

    // Idempotent: a second poll with no further generation change is a no-op.
    REQUIRE_FALSE(bridge.poll_editor_reload());
    REQUIRE(dynamic_cast<view::Label*>(root->child_at(0))->text() == "B");
}

TEST_CASE("ViewBridge editor reload is inert for a normal processor", "[view_bridge][reload][issue-1_9]") {
    state::StateStore store;
    StubProcessor proc;  // supports_editor_reload() == false by default
    proc.define_parameters(store);
    proc.custom_view = std::make_unique<view::View>();

    format::ViewBridge bridge(proc, store);
    REQUIRE(bridge.open());
    // A non-reloadable processor never rebuilds — poll is always false, no wrapper.
    REQUIRE_FALSE(bridge.poll_editor_reload());
    REQUIRE_FALSE(bridge.poll_editor_reload());
}

// Regression: the GPU display-link scripted-idle pump is dispatched to the main
// queue and can run AFTER its ViewBridge is destroyed — a host reloading the
// embedded view replaces the bridge while the host still holds the pump, and
// CVDisplayLinkStop does not join an in-flight callback. Before the bridge
// liveness token, the pump dereferenced the freed bridge (store()/scripted_ui())
// → EXC_BAD_ACCESS crash embedding the AU in Ableton Live. The pump must now
// read the token and no-op once the bridge is gone.
TEST_CASE("scripted idle pump no-ops after its bridge is destroyed (no UAF)",
          "[view_bridge][idle-pump][crash]") {
    StubProcessor p;
    state::StateStore store;
    auto bridge = std::make_unique<format::ViewBridge>(p, store);
    auto pump = format::make_scripted_idle_pump(*bridge);
    pump();                 // bridge alive: safe
    bridge.reset();         // destroy the bridge out from under the pump
    pump();                 // must NOT touch the freed bridge — token is false
    SUCCEED("idle pump no-oped after the bridge was destroyed");
}

// The AU/VST3/CLAP instance owns both Processor and StateStore, while a host may
// retain its editor after destroying that instance. The bridge itself is still
// alive in this ordering, so its bridge-lifetime token cannot protect the store.
// The adapter-owned token must fail closed before either referenced object dies.
TEST_CASE("scripted idle pump no-ops after its processor owner is destroyed",
          "[view_bridge][idle-pump][crash][owner-lifetime][lifecycle]") {
    struct Owner {
        state::StateStore store;
        StubProcessor processor;
        runtime::AliveToken alive;  // destroyed first; declared last
    };

    auto owner = std::make_unique<Owner>();
    auto bridge = std::make_unique<format::ViewBridge>(
        owner->processor, owner->store, owner->alive.capture());
    auto pump = format::make_scripted_idle_pump(*bridge);

    pump();
    owner.reset();  // Processor + StateStore are now dangling bridge references.
    pump();         // must reject the entire owner-facing tick before either use
    REQUIRE_FALSE(bridge->owner_is_alive());
    REQUIRE_FALSE(bridge->open());
    bridge.reset(); // teardown must also avoid Processor lifecycle callbacks
}

// Hosts are allowed to release the format instance, editor, native host, and
// queued idle callback in different orders. Exercise every independently-owned
// participant instead of pinning only the owner-first ordering that originally
// crashed Ableton. The explicit retire() mirrors the format adapters' teardown
// contract: no referenced Processor/StateStore object may die while the shared
// owner token still reports live.
TEST_CASE("scripted editor teardown-order matrix rejects every stale callback",
          "[view_bridge][idle-pump][crash][owner-lifetime][lifecycle][matrix]") {
    struct Fixture {
        runtime::AliveToken owner_alive;
        std::unique_ptr<StubProcessor> processor = std::make_unique<StubProcessor>();
        std::unique_ptr<state::StateStore> store =
            std::make_unique<state::StateStore>();
        std::unique_ptr<format::ViewBridge> bridge =
            std::make_unique<format::ViewBridge>(
                *processor, *store, owner_alive.capture());
        std::function<void()> pump = format::make_scripted_idle_pump(*bridge);

        void retire_owner() { owner_alive.retire(); }
    };

    SECTION("processor first") {
        Fixture f;
        f.pump();
        f.retire_owner();
        f.processor.reset();
        f.pump();
        REQUIRE_FALSE(f.bridge->owner_is_alive());
        f.bridge.reset();
        f.store.reset();
    }

    SECTION("store first") {
        Fixture f;
        f.pump();
        f.retire_owner();
        f.store.reset();
        f.pump();
        REQUIRE_FALSE(f.bridge->owner_is_alive());
        f.bridge.reset();
        f.processor.reset();
    }

    SECTION("processor and store owner first") {
        Fixture f;
        f.pump();
        f.retire_owner();
        f.processor.reset();
        f.store.reset();
        f.pump();
        REQUIRE_FALSE(f.bridge->open());
        f.bridge.reset();
    }

    SECTION("view bridge first") {
        Fixture f;
        f.pump();
        f.bridge.reset();
        f.pump();
        f.retire_owner();
        f.processor.reset();
        f.store.reset();
    }

    SECTION("native host callback first") {
        Fixture f;
        f.pump();
        f.pump = {};
        f.bridge.reset();
        f.retire_owner();
        f.processor.reset();
        f.store.reset();
        SUCCEED("native host released its callback before every referenced owner");
    }
}

// ── Runtime host-parameter surface (W3) ──────────────────────────────────────
//
// An imported design turns its knobs against `View::host_params()`. Nothing in
// production ever installed one, so `route_changes_to_host_params(true)` — which
// the importer emits for every bound control — resolved to a null surface and
// the control drove nothing. ViewBridge is the SDK's own view owner, so it is
// where the StateStore-backed surface belongs: every native Pulp editor now gets
// it with no per-plugin wiring.
//
// Exactly-once matters here. The surface is the ONLY writer on the routed path;
// a consumer that ALSO forwarded on_element_changed into the same store would
// double every gesture. These tests pin one write and one gesture bracket.

namespace {

// A processor whose editor is a faithful-import-shaped DesignFrameView: one knob
// bound to param_key "gain", routing its changes to the host-param surface.
class RoutedFrameProcessor : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {"RoutedFrame", "Acme", "com.acme.routed", "1.0.0",
                format::PluginCategory::Effect};
    }
    // The design's param_key ("gain") is matched against ParamInfo::name — the
    // convention the design-param generator already emits.
    void define_parameters(state::StateStore& s) override {
        s.add_parameter({1, "gain", "", {0.0f, 1.0f, 0.0f}});
    }
    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>&, const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext&) override {}

    std::unique_ptr<view::View> create_view() override {
        const std::string svg =
            R"(<svg width="100" height="100"><rect x="0" y="0" width="100" height="100"/></svg>)";
        view::DesignFrameElement knob;
        knob.kind = view::DesignFrameElement::Kind::knob;
        knob.x = 10; knob.y = 10; knob.w = 20; knob.h = 20;
        knob.cx = 20; knob.cy = 20; knob.hit_radius = 20.0f;
        knob.value = 0.0f;
        knob.param_key = "gain";
        auto frame = std::make_unique<view::DesignFrameView>(svg, std::vector{knob},
                                                             0, 0, 100, 100);
        frame->route_changes_to_host_params(true);   // what the importer emits
        frame->set_bounds({0, 0, 100, 100});
        last_frame = frame.get();
        return frame;
    }

    view::DesignFrameView* last_frame = nullptr;
};

}  // namespace

TEST_CASE("ViewBridge installs a StateStore-backed host-param surface on the view",
          "[view-bridge][host-param]") {
    RoutedFrameProcessor proc;
    state::StateStore store;
    proc.define_parameters(store);
    format::ViewBridge bridge(proc, store);

    REQUIRE(bridge.host_params() == nullptr);   // nothing before open()
    REQUIRE(bridge.open());
    REQUIRE(bridge.view() != nullptr);

    // The tree can now resolve design keys against the processor's parameters.
    REQUIRE(bridge.host_params() != nullptr);
    REQUIRE(bridge.view()->host_params() == bridge.host_params());
    CHECK(bridge.host_params()->has_param("gain"));
    CHECK_FALSE(bridge.host_params()->has_param("nope"));

    // ...and it is detached before the view dies, so nothing dangles.
    bridge.close();
    CHECK(bridge.host_params() == nullptr);
}

TEST_CASE("routed host-param UI fails closed after its owner is destroyed",
          "[view-bridge][host-param][crash][owner-lifetime][lifecycle]") {
    struct Owner {
        state::StateStore store;
        RoutedFrameProcessor processor;
        runtime::AliveToken alive;  // retires before processor/store destruct
    };

    auto owner = std::make_unique<Owner>();
    owner->processor.define_parameters(owner->store);
    auto bridge = std::make_unique<format::ViewBridge>(
        owner->processor, owner->store, owner->alive.capture());
    REQUIRE(bridge->open());

    auto* frame = owner->processor.last_frame;
    auto* surface = bridge->host_params();
    REQUIRE(frame != nullptr);
    REQUIRE(surface != nullptr);
    REQUIRE(surface->has_param("gain"));

    owner.reset();  // frame + surface remain, their Processor/StateStore do not

    CHECK_FALSE(surface->has_param("gain"));
    CHECK(surface->get_param("gain") == 0.0);
    CHECK(surface->param_display_text("gain", 0.5).empty());
    surface->begin_gesture("gain");
    surface->set_param("gain", 0.75);
    surface->end_gesture("gain");
    frame->simulate_drag({20, 20}, {20, 5});

    bridge.reset();
}

TEST_CASE("a routed design control drives the store exactly once per gesture",
          "[view-bridge][host-param]") {
    RoutedFrameProcessor proc;
    state::StateStore store;
    proc.define_parameters(store);

    int writes = 0, begins = 0, ends = 0;
    auto token = store.add_listener([&](state::ParamID id, float) {
                                        if (id == 1) ++writes;
                                    },
                                    state::ListenerThread::Main);
    store.set_gesture_callbacks([&](state::ParamID id) { if (id == 1) ++begins; },
                                [&](state::ParamID id) { if (id == 1) ++ends; });

    format::ViewBridge bridge(proc, store);
    REQUIRE(bridge.open());
    REQUIRE(proc.last_frame != nullptr);

    // One user gesture on the knob: press at its center, drag up, release.
    proc.last_frame->simulate_drag({20, 20}, {20, 5});

    CHECK(begins == 1);                  // exactly one undo group opened
    CHECK(ends == 1);                    // ...and closed
    CHECK(writes >= 1);                  // the drag reached the processor's param
    CHECK(store.get_normalized(1) > 0.0f);

    // Exactly-once, stated as the thing that actually breaks: a second writer on
    // the same gesture would show up as a doubled gesture bracket.
    CHECK(begins == ends);
    CHECK(begins != 2);
}

TEST_CASE("a routed control degrades to local state when the key is unknown",
          "[view-bridge][host-param]") {
    // A design control whose param_key the processor does not declare must not
    // fabricate a parameter or crash — it just drives its own visual.
    RoutedFrameProcessor proc;
    state::StateStore store;                 // deliberately EMPTY: no "gain" param
    format::ViewBridge bridge(proc, store);
    REQUIRE(bridge.open());
    REQUIRE(bridge.host_params() != nullptr);
    CHECK_FALSE(bridge.host_params()->has_param("gain"));

    proc.last_frame->simulate_drag({20, 20}, {20, 5});
    CHECK(proc.last_frame->element_value(0) > 0.0f);   // the control still moved
}


// ── Host -> UI: pulling host values into an imported design ──────────────────
//
// The mirror image of the routed-gesture tests above, and the half that was
// missing: `route_changes_to_host_params(true)` drove the store, but nothing
// ever pulled the store back into the view, so an imported design's controls
// did not follow automation playback or a host-side edit. A `bind_parameter`
// widget follows the host by registering a store listener that
// `pump_listeners()` drains; a DesignFrameView binds through the abstract
// HostParamSurface and registers no listener, so the editor pump must pull it.

namespace {

// A design whose bound frame is NESTED under a container root — the shape the
// importer actually emits (a frame inside a layout), not a bare frame root.
class NestedFrameProcessor : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {"NestedFrame", "Acme", "com.acme.nested", "1.0.0",
                format::PluginCategory::Effect};
    }
    void define_parameters(state::StateStore& s) override {
        s.add_parameter({1, "gain", "", {0.0f, 1.0f, 0.0f}});
    }
    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>&, const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext&) override {}

    std::unique_ptr<view::View> create_view() override {
        const std::string svg =
            R"(<svg width="100" height="100"><rect x="0" y="0" width="100" height="100"/></svg>)";
        view::DesignFrameElement knob;
        knob.kind = view::DesignFrameElement::Kind::knob;
        knob.x = 10; knob.y = 10; knob.w = 20; knob.h = 20;
        knob.cx = 20; knob.cy = 20; knob.hit_radius = 20.0f;
        knob.value = 0.0f;
        knob.param_key = "gain";
        auto frame = std::make_unique<view::DesignFrameView>(svg, std::vector{knob},
                                                             0, 0, 100, 100);
        frame->route_changes_to_host_params(true);
        frame->set_bounds({0, 0, 100, 100});
        last_frame = frame.get();

        auto root = std::make_unique<view::View>();
        root->set_bounds({0, 0, 100, 100});
        auto mid = std::make_unique<view::View>();   // an extra level of nesting
        mid->add_child(std::move(frame));
        root->add_child(std::move(mid));
        return root;
    }

    view::DesignFrameView* last_frame = nullptr;
};

}  // namespace

// Friend accessor: reaches ViewBridge's private host-pull seam. The pull is not
// public API — the editor idle pump is its only production caller — so a test
// that wants the synced COUNT (rather than the observable effect the pump
// tests assert) goes through here. Declared a friend in view_bridge.hpp;
// mirrors the StandaloneRenderTestAccess precedent.
namespace pulp::format {
struct ViewBridgeTestAccess {
    static std::size_t sync_design_frames_from_host(ViewBridge& bridge) {
        return bridge.sync_design_frames_from_host();
    }
};
}  // namespace pulp::format

TEST_CASE("host automation moves an imported design's control",
          "[view-bridge][host-param]") {
    RoutedFrameProcessor proc;
    state::StateStore store;
    proc.define_parameters(store);
    format::ViewBridge bridge(proc, store);
    REQUIRE(bridge.open());
    REQUIRE(proc.last_frame != nullptr);
    REQUIRE(proc.last_frame->element_value(0) == Catch::Approx(0.0f));

    // The host automates "gain" — the adapter writes the store from the audio
    // thread, exactly as an automation-playback block would.
    store.set_normalized_rt(1, 0.75f);

    // The production editor idle pump runs on the UI thread.
    auto pump = format::make_scripted_idle_pump(bridge);
    pump();

    CHECK(proc.last_frame->element_value(0) == Catch::Approx(0.75f));
}

TEST_CASE("the host pull reaches a design frame nested below the root",
          "[view-bridge][host-param]") {
    // The importer emits the frame inside a container, so a root-only pull
    // would reach nothing in a real imported design.
    NestedFrameProcessor proc;
    state::StateStore store;
    proc.define_parameters(store);
    format::ViewBridge bridge(proc, store);
    REQUIRE(bridge.open());
    REQUIRE(proc.last_frame != nullptr);

    store.set_normalized_rt(1, 0.5f);
    format::make_scripted_idle_pump(bridge)();

    CHECK(proc.last_frame->element_value(0) == Catch::Approx(0.5f));
    // ...and reached exactly that one frame, not the containers above it.
    CHECK(format::ViewBridgeTestAccess::sync_design_frames_from_host(bridge) == 1);
}

TEST_CASE("the host pull is silent: it never echoes back as a gesture",
          "[view-bridge][host-param]") {
    // The echo hazard: routing is auto-enabled for every bound imported design,
    // so a pull that re-emitted would drive the surface from the host's own
    // value and fight automation. The push must write the element directly.
    RoutedFrameProcessor proc;
    state::StateStore store;
    proc.define_parameters(store);
    format::ViewBridge bridge(proc, store);
    REQUIRE(bridge.open());

    int changed = 0, begins = 0, ends = 0;
    proc.last_frame->on_element_changed = [&](int, float) { ++changed; };
    proc.last_frame->on_gesture_begin = [&](int) { ++begins; };
    proc.last_frame->on_gesture_end = [&](int) { ++ends; };

    store.set_normalized_rt(1, 0.4f);
    auto pump = format::make_scripted_idle_pump(bridge);
    pump();

    CHECK(proc.last_frame->element_value(0) == Catch::Approx(0.4f));
    CHECK(changed == 0);   // a pull is not a user edit
    CHECK(begins == 0);
    CHECK(ends == 0);
}

TEST_CASE("repeated pulls converge and do not drift the host value",
          "[view-bridge][host-param]") {
    // Convergence on the continuous lane, demonstrated rather than argued: a
    // pull that fed its own value back would drift the parameter every frame.
    // The discrete (lossy) lane is swept separately below — a continuous knob
    // cannot fail this by construction, so it is not the interesting case.
    RoutedFrameProcessor proc;
    state::StateStore store;
    proc.define_parameters(store);
    format::ViewBridge bridge(proc, store);
    REQUIRE(bridge.open());

    // A value that does NOT sit on a quantization boundary.
    store.set_normalized_rt(1, 0.3f);
    auto pump = format::make_scripted_idle_pump(bridge);
    pump();

    const float settled_view = proc.last_frame->element_value(0);
    const float settled_host = store.get_normalized(1);

    for (int i = 0; i < 200; ++i) pump();   // a few seconds of UI ticks

    CHECK(proc.last_frame->element_value(0) == Catch::Approx(settled_view));
    CHECK(store.get_normalized(1) == Catch::Approx(settled_host));
    CHECK(store.get_normalized(1) == Catch::Approx(0.3f));   // host never moved
}

TEST_CASE("a user gesture still wins while the pull is live",
          "[view-bridge][host-param]") {
    // The pull must not fight the mouse: a gesture writes the store, so the
    // next pull reads back the value the user just set, not a stale one.
    RoutedFrameProcessor proc;
    state::StateStore store;
    proc.define_parameters(store);
    format::ViewBridge bridge(proc, store);
    REQUIRE(bridge.open());
    auto pump = format::make_scripted_idle_pump(bridge);
    pump();

    proc.last_frame->simulate_drag({20, 20}, {20, 5});   // user turns the knob up
    const float after_gesture = proc.last_frame->element_value(0);
    REQUIRE(after_gesture > 0.0f);

    pump();   // the pull must agree with the gesture, not undo it
    CHECK(proc.last_frame->element_value(0) == Catch::Approx(after_gesture));
}

namespace {

// A design whose bound control is a DISCRETE dropdown — the lossy lane. Three
// quantizing transforms sit between a user's pick and the value pulled back:
// choice_to_norm (index -> norm), the store's constrain_stored_value (which
// applies an implicit step of 1 to any non-Continuous ParamKind, whether or not
// the range declares a step), and norm_to_choice (norm -> index). A continuous
// knob passes a convergence test by construction; only this lane can fail one.
class DiscreteFrameProcessor : public format::Processor {
public:
    explicit DiscreteFrameProcessor(int options) : options_(options) {}
    format::PluginDescriptor descriptor() const override {
        return {"Discrete", "Acme", "com.acme.discrete", "1.0.0",
                format::PluginCategory::Effect};
    }
    void define_parameters(state::StateStore& s) override {
        state::ParamInfo p;
        p.id = 1;
        p.name = "mode";
        p.range = {0.0f, static_cast<float>(options_ - 1), 0.0f};
        p.kind = state::ParamKind::Enum;   // opts into integer quantization
        s.add_parameter(p);
    }
    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>&, const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext&) override {}

    std::unique_ptr<view::View> create_view() override {
        view::DesignFrameElement e;
        e.kind = view::DesignFrameElement::Kind::dropdown;
        e.x = 0; e.y = 0; e.w = 40; e.h = 10;
        e.param_key = "mode";
        for (int i = 0; i < options_; ++i) e.options.push_back("opt" + std::to_string(i));
        auto frame = std::make_unique<view::DesignFrameView>(
            R"(<svg width="40" height="10"><rect width="40" height="10"/></svg>)",
            std::vector{e}, 0, 0, 40, 10);
        frame->route_changes_to_host_params(true);
        frame->set_bounds({0, 0, 40, 10});
        last_frame = frame.get();
        return frame;
    }

    int options_;
    view::DesignFrameView* last_frame = nullptr;
};

}  // namespace

TEST_CASE("a discrete pull settles on the host's own value, across the range",
          "[view-bridge][host-param]") {
    // The oracle, swept the way the parameter-mirror lane learned to sweep:
    // option counts x 201 host start points, including values that land
    // BETWEEN options. A continuous knob cannot fail a convergence check by
    // construction; this lane can, because three quantizing hops sit between
    // the host's value and the index the design shows.
    //
    // Two questions, because neither implies the other:
    //   1. does the element settle (stop moving)?
    //   2. does the host stay where it was (the pull writes nothing back)?
    // A pull that echoed a quantized value would satisfy (1) while ratcheting
    // the parameter across the range in (2).
    for (int n : {2, 3, 4, 5, 8, 16}) {
        DiscreteFrameProcessor proc(n);
        state::StateStore store;
        proc.define_parameters(store);
        format::ViewBridge bridge(proc, store);
        REQUIRE(bridge.open());
        auto pump = format::make_scripted_idle_pump(bridge);

        for (int step = 0; step <= 200; ++step) {
            const float v = step / 200.0f;
            INFO("options=" << n << " host_norm=" << v);
            store.set_normalized_rt(1, v);
            pump();

            const float host_settled = store.get_normalized(1);
            const float elem_settled = proc.last_frame->element_value(0);

            // The element must show the option the HOST actually holds, not a
            // neighbor: re-deriving the index from the host's own value is the
            // only agreement that matters.
            CHECK(elem_settled == Catch::Approx(host_settled).margin(
                      0.5f / static_cast<float>(n - 1)));

            for (int t = 0; t < 50; ++t) pump();   // hold it for many UI ticks
            CHECK(store.get_normalized(1) == Catch::Approx(host_settled));  // host never moved
            CHECK(proc.last_frame->element_value(0) == Catch::Approx(elem_settled));  // settled
        }
    }
}

// ── Editor-initiated host resize (Processor::request_editor_resize) ──────────

// Without a handler installed (no open editor / a host with no resize path),
// request_editor_resize must be a safe no-op that reports refusal.
TEST_CASE("Processor::request_editor_resize is false with no handler installed",
          "[view_bridge][editor-resize]") {
    StubProcessor p;
    // The handler lives in a side table keyed by `this`; a prior test's
    // Processor could have reused this stack address, so start from a clean slot.
    p.set_editor_resize_handler(nullptr);
    REQUIRE_FALSE(p.request_editor_resize(640, 480));
}

// The adapter installs a handler; the plugin's editor calls
// request_editor_resize and the request forwards (w, h) verbatim and returns
// the host's accept/refuse result. Clearing the handler (editor close) restores
// the no-op refusal.
TEST_CASE("Processor::request_editor_resize forwards to the installed handler",
          "[view_bridge][editor-resize]") {
    StubProcessor p;

    uint32_t seen_w = 0, seen_h = 0;
    int calls = 0;
    p.set_editor_resize_handler([&](uint32_t w, uint32_t h) {
        ++calls;
        seen_w = w;
        seen_h = h;
        return true;  // host accepts
    });

    REQUIRE(p.request_editor_resize(900, 800));
    REQUIRE(calls == 1);
    REQUIRE(seen_w == 900);
    REQUIRE(seen_h == 800);

    // A host that refuses is reported honestly (the editor keeps its size).
    p.set_editor_resize_handler([](uint32_t, uint32_t) { return false; });
    REQUIRE_FALSE(p.request_editor_resize(1280, 800));

    // Cleared on editor close → back to a safe no-op refusal.
    p.set_editor_resize_handler(nullptr);
    REQUIRE_FALSE(p.request_editor_resize(1280, 800));
}

// If abnormal teardown skips the adapter's normal handler clear, an allocator
// may later place a new Processor at the same address. Construction must clear
// that stale slot before the replacement can accidentally invoke the old host.
TEST_CASE("Processor construction clears a stale editor-resize handler at a reused address",
          "[view_bridge][editor-resize]") {
    alignas(StubProcessor) std::byte storage[sizeof(StubProcessor)];
    auto* first = new (storage) StubProcessor;
    const void* key = first;
    first->set_editor_resize_handler([](uint32_t, uint32_t) { return true; });
    REQUIRE(pulp::format::detail::editor_resize_handlers().count(key) == 1);

    // Simulate abnormal teardown: no set_editor_resize_handler(nullptr).
    first->~StubProcessor();
    REQUIRE(pulp::format::detail::editor_resize_handlers().count(key) == 1);

    auto* replacement = new (storage) StubProcessor;
    REQUIRE(replacement == key);
    REQUIRE(pulp::format::detail::editor_resize_handlers().count(key) == 0);
    REQUIRE_FALSE(replacement->request_editor_resize(640, 480));
    replacement->~StubProcessor();
}

// A mode switch updates the reported preferred size + aspect while preserving
// the processor's declared drag envelope.
TEST_CASE("ViewBridge::set_preferred_size updates an in-bounds mode size",
          "[view_bridge][editor-resize]") {
    StubProcessor p;  // view_size() = {480,320, 320,240, 1024,768}
    state::StateStore store;
    format::ViewBridge bridge(p, store);
    REQUIRE(bridge.open());

    const auto& before = bridge.size_hints();
    REQUIRE(before.preferred_width == 480);
    REQUIRE(before.preferred_height == 320);

    REQUIRE(bridge.set_preferred_size(900, 700));
    const auto& after = bridge.size_hints();
    REQUIRE(after.preferred_width == 900);
    REQUIRE(after.preferred_height == 700);
    // Aspect follows the new natural shape.
    REQUIRE(after.aspect_ratio == Catch::Approx(900.0 / 700.0));
    REQUIRE(after.min_width == before.min_width);
    REQUIRE(after.min_height == before.min_height);
    REQUIRE(after.max_width == before.max_width);
    REQUIRE(after.max_height == before.max_height);
    REQUIRE(after.preferred_width >= after.min_width);
    REQUIRE(after.preferred_height >= after.min_height);
    REQUIRE(after.preferred_width <= after.max_width);
    REQUIRE(after.preferred_height <= after.max_height);

    // Out-of-envelope modes are rejected without partial mutation.
    REQUIRE_FALSE(bridge.set_preferred_size(900, 800));
    REQUIRE(bridge.size_hints().preferred_width == 900);
    REQUIRE(bridge.size_hints().preferred_height == 700);
    REQUIRE(bridge.size_hints().aspect_ratio ==
            Catch::Approx(900.0 / 700.0));

    // Zero dimensions are ignored (no accidental collapse).
    REQUIRE_FALSE(bridge.set_preferred_size(0, 500));
    REQUIRE(bridge.size_hints().preferred_width == 900);
    REQUIRE(bridge.size_hints().preferred_height == 700);
}

TEST_CASE("design-derived Forge bounds contain the 900x800 mode exactly",
          "[view_bridge][editor-resize][forge]") {
    const auto base = format::view_size_from_design(1280, 800, 640, 400);
    REQUIRE(base.min_width == 640);
    REQUIRE(base.min_height == 400);
    REQUIRE(base.max_width == 2560);
    REQUIRE(base.max_height == 1600);

    class ForgeSizeProcessor final : public format::Processor {
    public:
        format::PluginDescriptor descriptor() const override {
            return {"Forge", "Acme", "com.acme.forge", "1.0.0",
                    format::PluginCategory::Effect};
        }
        void define_parameters(state::StateStore&) override {}
        void prepare(const format::PrepareContext&) override {}
        void process(audio::BufferView<float>&,
                     const audio::BufferView<const float>&,
                     midi::MidiBuffer&, midi::MidiBuffer&,
                     const format::ProcessContext&) override {}
        format::ViewSize view_size() const override {
            return format::view_size_from_design(1280, 800, 640, 400);
        }
    } processor;
    state::StateStore store;
    format::ViewBridge bridge(processor, store);
    REQUIRE(bridge.set_preferred_size(900, 800));
    const auto& mode = bridge.size_hints();
    REQUIRE(mode.preferred_width == 900);
    REQUIRE(mode.preferred_height == 800);
    REQUIRE(mode.min_width == base.min_width);
    REQUIRE(mode.min_height == base.min_height);
    REQUIRE(mode.max_width == base.max_width);
    REQUIRE(mode.max_height == base.max_height);
    REQUIRE(mode.aspect_ratio == Catch::Approx(900.0 / 800.0));
}

TEST_CASE("preferred-size host transaction publishes, accepts, and rolls back",
          "[view_bridge][editor-resize][transaction]") {
    StubProcessor p;
    state::StateStore store;
    format::ViewBridge bridge(p, store);

    bool callback_saw_proposed_hints = false;
    REQUIRE(format::detail::negotiate_preferred_size(
        bridge, 900, 700,
        [&](uint32_t w, uint32_t h) {
            callback_saw_proposed_hints =
                w == 900 && h == 700 &&
                bridge.size_hints().preferred_width == 900 &&
                bridge.size_hints().preferred_height == 700 &&
                bridge.size_hints().aspect_ratio ==
                    Catch::Approx(900.0 / 700.0);
            return true;
        }));
    REQUIRE(callback_saw_proposed_hints);
    REQUIRE(bridge.size_hints().preferred_width == 900);
    REQUIRE(bridge.size_hints().preferred_height == 700);

    callback_saw_proposed_hints = false;
    REQUIRE_FALSE(format::detail::negotiate_preferred_size(
        bridge, 800, 600,
        [&](uint32_t, uint32_t) {
            callback_saw_proposed_hints =
                bridge.size_hints().preferred_width == 800 &&
                bridge.size_hints().preferred_height == 600;
            return false;
        }));
    REQUIRE(callback_saw_proposed_hints);
    REQUIRE(bridge.size_hints().preferred_width == 900);
    REQUIRE(bridge.size_hints().preferred_height == 700);
    REQUIRE(bridge.size_hints().aspect_ratio ==
            Catch::Approx(900.0 / 700.0));

    int invalid_request_calls = 0;
    REQUIRE_FALSE(format::detail::negotiate_preferred_size(
        bridge, 900, 800,
        [&](uint32_t, uint32_t) {
            ++invalid_request_calls;
            return true;
        }));
    REQUIRE(invalid_request_calls == 0);
    REQUIRE(bridge.size_hints().preferred_width == 900);
    REQUIRE(bridge.size_hints().preferred_height == 700);
}
