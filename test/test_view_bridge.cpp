#include <catch2/catch_test_macros.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/runtime/message_channel.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/view.hpp>
#include <functional>

using namespace pulp;

namespace {

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
