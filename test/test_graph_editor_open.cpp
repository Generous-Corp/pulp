// GraphEditorView opening a node's plugin editor — the first real consumer of
// EditorAttachment. Headless: a FakeSlot injected into a SignalGraph node and a
// FakeWindowHost supplied via the editor host provider. The reverse-order case
// is where EditorAttachment's shared-ownership fix (the slot outliving the graph
// node that opened it) is validated through a real widget.

#include <catch2/catch_test_macros.hpp>

#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/view/hosted_editor_attachment.hpp>
#include <pulp/view/widgets/graph_editor_view.hpp>
#include <pulp/view/window_host.hpp>

#include <functional>
#include <memory>
#include <vector>

using pulp::host::NodeId;
using pulp::host::SignalGraph;
using pulp::view::widgets::GraphEditorView;

namespace {

// Minimal slot fake that hands out a HostedEditor with a non-null handle.
class FakeSlot : public pulp::host::PluginSlot {
public:
    bool created = false;
    bool destroyed = false;
    int fake_view = 0;

    const pulp::host::PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    void process(pulp::audio::BufferView<float>&,
                 const pulp::audio::BufferView<const float>&,
                 const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::host::ParameterEventQueue&,
                 int) override {}
    std::vector<pulp::host::HostParamInfo> parameters() const override { return {}; }
    float get_parameter(uint32_t) const override { return 0.0f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool b) override { bypassed_ = b; }
    bool is_bypassed() const override { return bypassed_; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return true; }
    bool has_editor() const override { return true; }

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    void* create_editor_view() override { return &fake_view; }
    void destroy_editor_view() override {}
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

    std::unique_ptr<HostedEditor> create_hosted_editor(void* /*parent*/) override {
        auto ed = std::make_unique<HostedEditor>();
        ed->native_handle = &fake_view;
        ed->width = 800;
        ed->height = 600;
        ed->resizable = true;
        created = true;
        return ed;
    }
    void destroy_hosted_editor(std::unique_ptr<HostedEditor> ed) override {
        if (ed) destroyed = true;
    }

    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }

private:
    pulp::host::PluginInfo info_;
    bool bypassed_ = false;
};

// Records the attach/detach traffic EditorAttachment drives.
class FakeWindowHost : public pulp::view::WindowHost {
public:
    void show() override {}
    void hide() override {}
    bool is_visible() const override { return true; }
    void repaint() override {}
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}

    bool attach_native_child_view(void* v, float, float, float, float) override {
        attached_handle = v;
        attach_calls++;
        return attach_should_succeed;
    }
    bool set_native_child_view_bounds(void*, float, float, float, float) override {
        return true;
    }
    void detach_native_child_view(void* v) override {
        detached_handle = v;
        detach_calls++;
    }
    void* native_window_handle() const override {
        return reinterpret_cast<void*>(0xCAFEBABE);
    }

    int attach_calls = 0;
    int detach_calls = 0;
    bool attach_should_succeed = true;
    void* attached_handle = nullptr;
    void* detached_handle = nullptr;
};

NodeId add_fake_plugin_node(SignalGraph& graph, FakeSlot** out_raw = nullptr) {
    auto slot = std::make_unique<FakeSlot>();
    if (out_raw) *out_raw = slot.get();
    return graph.add_plugin_node(std::move(slot), 2, 2, "Fake");
}

}  // namespace

TEST_CASE("GraphEditorView opens a node's editor through the host provider",
          "[view][graph-editor][editor][issue-11]") {
    SignalGraph graph;
    FakeSlot* raw = nullptr;
    const NodeId id = add_fake_plugin_node(graph, &raw);

    FakeWindowHost host;
    GraphEditorView gev(graph);
    gev.set_editor_host_provider([&](NodeId) { return &host; });

    REQUIRE(gev.open_node_editor(id));
    REQUIRE(gev.is_editor_open(id));
    REQUIRE(raw->created);
    REQUIRE(host.attach_calls == 1);

    // Idempotent: a second open does not re-attach.
    REQUIRE(gev.open_node_editor(id));
    REQUIRE(host.attach_calls == 1);

    gev.close_node_editor(id);
    REQUIRE_FALSE(gev.is_editor_open(id));
    REQUIRE(host.detach_calls == 1);
}

TEST_CASE("GraphEditorView open_node_editor is a no-op without a usable host",
          "[view][graph-editor][editor][issue-11]") {
    SignalGraph graph;
    const NodeId id = add_fake_plugin_node(graph);
    GraphEditorView gev(graph);

    SECTION("no provider set") {
        REQUIRE_FALSE(gev.open_node_editor(id));
    }
    SECTION("provider returns no host") {
        gev.set_editor_host_provider([](NodeId) -> pulp::view::WindowHost* { return nullptr; });
        REQUIRE_FALSE(gev.open_node_editor(id));
    }
    REQUIRE_FALSE(gev.is_editor_open(id));
}

TEST_CASE("GraphEditorView open_node_editor rejects an unknown node",
          "[view][graph-editor][editor][issue-11]") {
    SignalGraph graph;
    FakeWindowHost host;
    GraphEditorView gev(graph);
    gev.set_editor_host_provider([&](NodeId) { return &host; });

    REQUIRE_FALSE(gev.open_node_editor(999999));
    REQUIRE(host.attach_calls == 0);
}

TEST_CASE("GraphEditorView keeps a node's slot alive while its editor is open",
          "[view][graph-editor][editor][issue-11]") {
    // The reverse-order teardown that EditorAttachment's shared_ptr fix exists
    // for: remove the node (dropping the graph's slot reference) while the
    // editor is still open. With a raw slot pointer this dangled; the
    // attachment's co-ownership keeps the slot alive until the editor closes.
    SignalGraph graph;
    const NodeId id = add_fake_plugin_node(graph);
    std::weak_ptr<pulp::host::PluginSlot> weak = graph.node(id)->plugin;

    FakeWindowHost host;
    GraphEditorView gev(graph);
    gev.set_editor_host_provider([&](NodeId) { return &host; });
    REQUIRE(gev.open_node_editor(id));

    graph.remove_node(id);
    REQUIRE(weak.lock());  // the open editor co-owns the slot — not freed

    gev.close_node_editor(id);  // detaches + destroys through the still-live slot
    REQUIRE(host.detach_calls == 1);
    REQUIRE_FALSE(gev.is_editor_open(id));
    REQUIRE_FALSE(weak.lock());  // attachment was the last owner → freed cleanly
}
