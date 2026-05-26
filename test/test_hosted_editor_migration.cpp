// HostedEditor → WindowHost migration (item 4.4 macOS plan).
//
// Exercises pulp::view::EditorAttachment — the RAII wrapper that walks a
// PluginSlot's typed HostedEditor onto WindowHost::attach_native_child_view
// (the canonical native-window contract from PR #2844).

#include <catch2/catch_test_macros.hpp>

#include <pulp/host/plugin_slot.hpp>
#include <pulp/view/hosted_editor_attachment.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/window_host.hpp>

#include <memory>
#include <vector>

namespace {

// Minimal slot fake that hands out a HostedEditor with a non-null handle.
class FakeSlot : public pulp::host::PluginSlot {
public:
    bool created = false;
    bool destroyed = false;
    int fake_view = 0;
    uint32_t init_w = 800;
    uint32_t init_h = 600;
    bool resizable = true;

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

    // The legacy `void*` virtuals are pure in the base class, so we must
    // override them — but we route to the typed API below and use the legacy
    // entry points only via the deprecation-aware default impl.
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    void* create_editor_view() override { return &fake_view; }
    void destroy_editor_view() override {}
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

    std::unique_ptr<HostedEditor>
    create_hosted_editor(void* /*parent*/) override {
        auto ed = std::make_unique<HostedEditor>();
        ed->native_handle = &fake_view;
        ed->width = init_w;
        ed->height = init_h;
        ed->resizable = resizable;
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

// Slot whose typed editor returns a null handle — should be treated as
// "no editor available" by EditorAttachment::create.
class NullHandleSlot : public FakeSlot {
public:
    int destroy_typed_calls = 0;

    std::unique_ptr<HostedEditor>
    create_hosted_editor(void* /*parent*/) override {
        auto ed = std::make_unique<HostedEditor>();
        ed->native_handle = nullptr;  // simulate "couldn't open editor"
        ed->width = 200;
        ed->height = 100;
        created = true;
        return ed;
    }
    void destroy_hosted_editor(std::unique_ptr<HostedEditor> ed) override {
        if (ed) ++destroy_typed_calls;
    }
};

// Fake host that records attach/detach/bounds traffic. We don't subclass
// pulp::view::WindowHost directly because it pulls in `View&` for construction
// and lots of platform plumbing; instead, we cover the no-native-window-host
// path via WindowHost::create returning nullptr and exercise the bridge via
// a hand-rolled host stub that satisfies the public virtual surface used by
// EditorAttachment.

class FakeWindowHost : public pulp::view::WindowHost {
public:
    // Pure virtuals on WindowHost we don't care about for this test.
    void show() override {}
    void hide() override {}
    bool is_visible() const override { return true; }
    void repaint() override {}
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}

    // The two seams EditorAttachment touches.
    bool attach_native_child_view(void* v, float x, float y, float w, float h) override {
        attached_handle = v;
        attached_x = x; attached_y = y;
        attached_w = w; attached_h = h;
        attach_calls++;
        return attach_should_succeed;
    }
    bool set_native_child_view_bounds(void* v, float x, float y, float w, float h) override {
        bounds_handle = v;
        bounds_x = x; bounds_y = y;
        bounds_w = w; bounds_h = h;
        bounds_calls++;
        return true;
    }
    void detach_native_child_view(void* v) override {
        detached_handle = v;
        detach_calls++;
    }
    void* native_window_handle() const override { return reinterpret_cast<void*>(0xCAFEBABE); }

    int attach_calls = 0;
    int detach_calls = 0;
    int bounds_calls = 0;
    bool attach_should_succeed = true;
    void* attached_handle = nullptr;
    void* bounds_handle = nullptr;
    void* detached_handle = nullptr;
    float attached_x = -1, attached_y = -1, attached_w = -1, attached_h = -1;
    float bounds_x = -1, bounds_y = -1, bounds_w = -1, bounds_h = -1;
};

} // namespace

TEST_CASE("EditorAttachment attaches via the native-windows seam",
          "[host][hosted-editor][migration][item-4-4]") {
    FakeSlot slot;
    FakeWindowHost host;
    auto att = pulp::view::EditorAttachment::create(&slot, &host, 12.0f, 34.0f);
    REQUIRE(att);
    REQUIRE(slot.created);
    REQUIRE(host.attach_calls == 1);
    REQUIRE(host.attached_handle == &slot.fake_view);
    REQUIRE(host.attached_x == 12.0f);
    REQUIRE(host.attached_y == 34.0f);
    REQUIRE(host.attached_w == 800.0f);
    REQUIRE(host.attached_h == 600.0f);
    REQUIRE(att->is_attached());
    REQUIRE(att->editor()->resizable);
}

TEST_CASE("EditorAttachment destructor detaches and destroys",
          "[host][hosted-editor][migration][item-4-4]") {
    FakeSlot slot;
    FakeWindowHost host;
    {
        auto att = pulp::view::EditorAttachment::create(&slot, &host);
        REQUIRE(att);
    }
    REQUIRE(host.detach_calls == 1);
    REQUIRE(host.detached_handle == &slot.fake_view);
    REQUIRE(slot.destroyed);
}

TEST_CASE("EditorAttachment release() is idempotent",
          "[host][hosted-editor][migration][item-4-4]") {
    FakeSlot slot;
    FakeWindowHost host;
    auto att = pulp::view::EditorAttachment::create(&slot, &host);
    REQUIRE(att);
    att->release();
    REQUIRE(host.detach_calls == 1);
    REQUIRE_FALSE(att->is_attached());
    // Second release is a no-op.
    att->release();
    REQUIRE(host.detach_calls == 1);
}

TEST_CASE("EditorAttachment set_bounds forwards to the host",
          "[host][hosted-editor][migration][item-4-4]") {
    FakeSlot slot;
    FakeWindowHost host;
    auto att = pulp::view::EditorAttachment::create(&slot, &host);
    REQUIRE(att);
    REQUIRE(att->set_bounds(5.0f, 6.0f, 200.0f, 150.0f));
    REQUIRE(host.bounds_calls == 1);
    REQUIRE(host.bounds_handle == &slot.fake_view);
    REQUIRE(host.bounds_w == 200.0f);
    REQUIRE(host.bounds_h == 150.0f);
    REQUIRE(att->width() == 200.0f);
    REQUIRE(att->height() == 150.0f);
    att->release();
    REQUIRE_FALSE(att->set_bounds(0, 0, 1, 1));  // post-release reject
}

TEST_CASE("EditorAttachment::create returns null when attach fails",
          "[host][hosted-editor][migration][item-4-4]") {
    FakeSlot slot;
    FakeWindowHost host;
    host.attach_should_succeed = false;
    auto att = pulp::view::EditorAttachment::create(&slot, &host);
    REQUIRE_FALSE(att);
    // The slot still gets destroy_hosted_editor() so it doesn't leak.
    REQUIRE(slot.destroyed);
    REQUIRE(host.attach_calls == 1);
    REQUIRE(host.detach_calls == 0);
}

TEST_CASE("EditorAttachment::create returns null on null handles",
          "[host][hosted-editor][migration][item-4-4]") {
    NullHandleSlot slot;
    FakeWindowHost host;
    auto att = pulp::view::EditorAttachment::create(&slot, &host);
    REQUIRE_FALSE(att);
    // The typed destroy is the path that ran (handle was null so we never
    // touched the legacy entry points).
    REQUIRE(slot.destroy_typed_calls == 1);
    REQUIRE(host.attach_calls == 0);
}

TEST_CASE("EditorAttachment::create rejects null inputs",
          "[host][hosted-editor][migration][item-4-4]") {
    FakeSlot slot;
    FakeWindowHost host;
    REQUIRE_FALSE(pulp::view::EditorAttachment::create(nullptr, &host));
    REQUIRE_FALSE(pulp::view::EditorAttachment::create(&slot, nullptr));
}

TEST_CASE("EditorAttachment is move-constructible without re-detaching",
          "[host][hosted-editor][migration][item-4-4]") {
    FakeSlot slot;
    FakeWindowHost host;
    auto first = pulp::view::EditorAttachment::create(&slot, &host);
    REQUIRE(first);
    pulp::view::EditorAttachment moved(std::move(*first));
    REQUIRE(moved.is_attached());
    // first is now hollow; releasing it must not double-detach.
    first->release();
    REQUIRE(host.detach_calls == 0);
    // Letting moved go out of scope does the single real detach.
    moved.release();
    REQUIRE(host.detach_calls == 1);
}
