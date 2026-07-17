#include <catch2/catch_test_macros.hpp>
#include <pulp/format/gpu_host_select.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/runtime/message_channel.hpp>
#include <pulp/state/store.hpp>
#include <pulp/state/listener_token.hpp>
#include <pulp/view/design_frame_view.hpp>
#include <pulp/view/host_param_surface.hpp>
#include <pulp/view/scripted_ui.hpp>
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

} // namespace

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
