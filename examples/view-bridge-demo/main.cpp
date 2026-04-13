// view-bridge-demo — exercises Processor::create_view() and multi-view attach.
//
// Runs headlessly:
//   1. Construct DemoProcessor with a custom create_view() returning a
//      hand-built view tree.
//   2. Open a ViewBridge; the custom view is used instead of AutoUi.
//   3. Attach an inspector view as a secondary view (role = Inspector).
//   4. Resize the editor and detach the inspector.
//   5. Close the bridge; lifecycle counters verify every callback fired.
//
// Exit code is 0 on success, non-zero if any invariant fails.

#include <pulp/format/processor.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/view.hpp>

#include <cstdio>
#include <cstdlib>

namespace {

class DemoProcessor : public pulp::format::Processor {
public:
    int opened = 0, closed = 0, resized = 0;
    uint32_t last_w = 0, last_h = 0;

    pulp::format::PluginDescriptor descriptor() const override {
        return {"view-bridge-demo", "Pulp", "com.pulp.view-bridge-demo", "0.1.0",
                pulp::format::PluginCategory::Effect};
    }

    void define_parameters(pulp::state::StateStore& s) override {
        s.add_parameter({1, "Gain", "dB", {-60.0f, 12.0f, 0.0f}});
        s.add_parameter({2, "Mix",  "%",  {  0.0f, 100.0f, 100.0f}});
    }

    void prepare(const pulp::format::PrepareContext&) override {}

    void process(pulp::audio::BufferView<float>&,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}

    pulp::format::ViewSize view_size() const override {
        return {640, 480, 400, 300, 1280, 960};
    }

    std::unique_ptr<pulp::view::View> create_view() override {
        auto root = std::make_unique<pulp::view::View>();
        root->set_theme(pulp::view::Theme::dark());
        return root;
    }

    void on_view_opened(pulp::view::View&) override { ++opened; }
    void on_view_closed(pulp::view::View&) override { ++closed; }
    void on_view_resized(pulp::view::View&, uint32_t w, uint32_t h) override {
        ++resized;
        last_w = w;
        last_h = h;
    }
};

#define REQUIRE(cond)                                                        \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "REQUIRE failed: %s (%s:%d)\n",             \
                         #cond, __FILE__, __LINE__);                         \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)

} // namespace

int main() {
    DemoProcessor proc;
    pulp::state::StateStore store;
    proc.set_state_store(&store);
    proc.define_parameters(store);

    pulp::format::ViewBridge bridge(proc, store);
    REQUIRE(!bridge.is_open());
    REQUIRE(bridge.size_hints().preferred_width == 640);
    REQUIRE(bridge.size_hints().min_width == 400);
    REQUIRE(bridge.size_hints().max_height == 960);

    std::string err;
    REQUIRE(bridge.open(&err));
    REQUIRE(bridge.is_open());
    REQUIRE(bridge.view() != nullptr);
    REQUIRE(proc.opened == 0);  // deferred until notify_attached()
    bridge.notify_attached();
    REQUIRE(proc.opened == 1);
    REQUIRE(bridge.view_count() == 1);

    // Attach an inspector as a secondary view.
    auto inspector = std::make_unique<pulp::view::View>();
    auto* insp = bridge.attach_secondary_view(
        std::move(inspector), pulp::format::ViewRole::Inspector);
    REQUIRE(insp != nullptr);
    REQUIRE(bridge.view_count() == 2);
    REQUIRE(bridge.role_at(1) == pulp::format::ViewRole::Inspector);

    bridge.resize(800, 600);
    REQUIRE(proc.resized == 1);
    REQUIRE(proc.last_w == 800);
    REQUIRE(proc.last_h == 600);

    REQUIRE(bridge.detach_secondary_view(insp));
    REQUIRE(bridge.view_count() == 1);

    bridge.close();
    REQUIRE(!bridge.is_open());
    REQUIRE(proc.closed == 1);

    std::printf("view-bridge-demo: ok (opened=%d resized=%d closed=%d)\n",
                proc.opened, proc.resized, proc.closed);
    return 0;
}
