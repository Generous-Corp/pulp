#include <catch2/catch_test_macros.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/view.hpp>

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
