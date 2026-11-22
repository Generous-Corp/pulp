// First, so this file proves the header compiles on its own, as a generated
// editor package includes it.
#include <pulp/vellum/editor.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/format/editor_host.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#include <vellum/value/param_model.hpp>
#include <vellum/view/plugin_view_host.hpp>
#include <vellum/view/view.hpp>
#if defined(PULP_TEST_VELLUM_SCRIPTED_UI)
#include <vellum/view/scripted_ui.hpp>
#include <vellum/view/widgets.hpp>
#endif

#if defined(PULP_TEST_VELLUM_SCRIPTED_EDITOR)
#include <pulp/format/contract_editor_session.hpp>
#include <pulp/format/editor_assets.hpp>
#include <pulp/format/plugin_state_io.hpp>
#include <pulp/vellum/scripted_editor.hpp>
#endif

#if defined(PULP_TEST_VELLUM_SCRIPTED_AUDIO)
#include <pulp/vellum_audio/audio_widgets.hpp>
#include <algorithm>
#include <array>
#endif

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace {

class Context final : public pulp::format::EditorContext {
  public:
    explicit Context(pulp::format::ParamPort& params, pulp::format::VisualPort* visuals = nullptr)
        : params_(params), visuals_(visuals) {}
    pulp::format::ParamPort& params() override {
        return params_;
    }
    pulp::format::VisualPort* visuals() override {
        return visuals_;
    }
    bool request_resize(const pulp::format::EditorSize& size) override {
        ++resize_requests;
        requested_size = size;
        return accept_resize;
    }
    bool host_action(const pulp::format::HostActionRequest&) override {
        return false;
    }
    pulp::format::EditorHostKind host_kind() const override {
        return pulp::format::EditorHostKind::plugin;
    }

    int resize_requests = 0;
    pulp::format::EditorSize requested_size;
    bool accept_resize = false;

  private:
    pulp::format::ParamPort& params_;
    pulp::format::VisualPort* visuals_;
};

class Processor final : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {};
    }
    void define_parameters(pulp::state::StateStore&) override {}
    bool supports_editor_reload() const override {
        return reloadable;
    }
    std::uint64_t editor_reload_generation() const override {
        return generation;
    }

    bool reloadable = false;
    std::uint64_t generation = 0;
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}
    std::optional<std::uint32_t> editor_background() const override {
        return background;
    }

    std::optional<std::uint32_t> background;
};

class TestView final : public ::vellum::view::View {
  public:
    explicit TestView(pulp::vellum::EditorEnv& env) {
        auto gain = env.params.resolve("Gain");
        subscription_ =
            env.params.subscribe([this, gain](::vellum::value::ParamHandle handle, double value) {
                if (handle == gain)
                    observed = value;
            });
    }

    double observed = -1.0;

  private:
    ::vellum::value::Subscription subscription_;
};

class Fixture {
  public:
    pulp::state::StateStore store;
    std::unique_ptr<pulp::format::StateStoreParamPort> port;
    std::unique_ptr<pulp::vellum::ParamPortModel> model;

    Fixture() {
        pulp::state::ParamInfo info;
        info.id = 1;
        info.name = "Gain";
        info.range = {0.0f, 1.0f, 0.5f};
        store.add_parameter(info);
        port = std::make_unique<pulp::format::StateStoreParamPort>(store);
        model = std::make_unique<pulp::vellum::ParamPortModel>(*port);
    }
};

class StubHost final : public ::vellum::view::PluginViewHost {
  public:
    explicit StubHost(Size size = {}) : size_(size) {}

    ::vellum::view::NativeViewHandle native_handle() override {
        return this;
    }
    void attach_to_parent(::vellum::view::NativeViewHandle parent) override {
        attached_ = parent == this;
    }
    bool is_attached() const noexcept override {
        return attached_;
    }
    void detach() override {
        attached_ = false;
    }
    void repaint() override {}
    bool present_first_frame() override {
        ++present_first_frame_calls;
        return present_first_frame_result;
    }
    std::vector<std::uint8_t> capture_back_buffer_png() override {
        ++capture_back_buffer_calls;
        return capture_bytes;
    }
    void set_size(uint32_t width, uint32_t height) override {
        size_ = {width, height};
    }
    Size get_size() const override {
        return size_;
    }
    void set_design_viewport(float width, float height) override {
        ++design_viewport_calls;
        design_width = width;
        design_height = height;
    }
    void set_fixed_aspect_ratio(float ratio) override {
        ++fixed_aspect_calls;
        aspect_ratio = ratio;
    }

    int design_viewport_calls = 0;
    int fixed_aspect_calls = 0;
    int present_first_frame_calls = 0;
    int capture_back_buffer_calls = 0;
    bool present_first_frame_result = true;
    std::vector<std::uint8_t> capture_bytes = {0x89, 'P', 'N', 'G'};
    float design_width = 0;
    float design_height = 0;
    float aspect_ratio = 0;

  private:
    Size size_;
    bool attached_ = false;
};

// Records which host the root points at when its host starts being destroyed.
class RecordingHost final : public ::vellum::view::PluginViewHost {
  public:
    RecordingHost(::vellum::view::View& root, ::vellum::view::PluginViewHost** seen)
        : root_(root), seen_(seen) {}
    ~RecordingHost() override {
        *seen_ = root_.plugin_view_host();
    }
    ::vellum::view::NativeViewHandle native_handle() override {
        return this;
    }
    void attach_to_parent(::vellum::view::NativeViewHandle) override {}
    void detach() override {}
    void repaint() override {}
    void set_size(uint32_t, uint32_t) override {}
    Size get_size() const override {
        return {};
    }

  private:
    ::vellum::view::View& root_;
    ::vellum::view::PluginViewHost** seen_;
};

class EmptyView final : public ::vellum::view::View {
  public:
    explicit EmptyView(pulp::vellum::EditorEnv&) {}
};

// Counts the editor's idle ticks and records the bounds it was laid out to.
int g_idle_ticks = 0;
float g_idle_width = 0.0f;
float g_idle_height = 0.0f;

class IdleProbeView final : public ::vellum::view::View {
  public:
    explicit IdleProbeView(pulp::vellum::EditorEnv&) {}
    void editor_idle() {
        ++g_idle_ticks;
        g_idle_width = bounds().width;
        g_idle_height = bounds().height;
    }
};

// Clears any factory a test registers, even when an assertion stops it early.
struct FactoryReset {
    ~FactoryReset() {
        ::vellum::view::PluginViewHost::clear_factory();
    }
};

struct LifecycleProbe {
    int opens = 0;
    int closes = 0;
    int ticks = 0;
    bool host_attached_at_open = false;
    bool host_attached_at_close = false;
};

class HookView : public ::vellum::view::View {
  public:
    explicit HookView(pulp::vellum::EditorEnv& env) : env(env) {}
    HookView(pulp::vellum::EditorEnv& env, pulp::format::Processor& processor)
        : env(env), processor(&processor) {}

    static pulp::format::EditorSize preferred_editor_size() {
        return {532, 316, 1.0};
    }

    void editor_opened() {
        ++lifecycle->opens;
        lifecycle->host_attached_at_open = plugin_view_host()->is_attached();
    }
    void editor_closing() {
        ++lifecycle->closes;
        lifecycle->host_attached_at_close = plugin_view_host()->is_attached();
    }
    void editor_idle() {
        ++lifecycle->ticks;
    }

    pulp::vellum::EditorEnv& env;
    pulp::format::Processor* processor = nullptr;
    LifecycleProbe* lifecycle = nullptr;
};

template <bool HasDesign = true> class SizedHookView final : public HookView {
  public:
    using HookView::HookView;

    pulp::format::ViewSize editor_view_size() const {
        return {.preferred_width = 990,
                .preferred_height = 645,
                .min_width = 792,
                .min_height = 516,
                .max_width = 2640,
                .max_height = 1720,
                .aspect_ratio = 1320.0 / 860,
                .design_width = HasDesign ? 1320u : 0u,
                .design_height = HasDesign ? 860u : 0u};
    }
};

template <typename Root = HookView> struct HookFixture {
    using PluginViewHost = ::vellum::view::PluginViewHost;

    FactoryReset reset;
    Fixture fixture;
    Context context{*fixture.port};
    Processor processor;
    LifecycleProbe lifecycle;
    Root* root = nullptr;
    StubHost* host = nullptr;
    std::unique_ptr<pulp::format::Editor> editor;

    HookFixture() {
        PluginViewHost::set_factory(
            [this](::vellum::view::View& view, const PluginViewHost::Options& options) {
                root = &static_cast<Root&>(view);
                root->lifecycle = &lifecycle;
                auto stub = std::make_unique<StubHost>(options.size);
                host = stub.get();
                return stub;
            });
        pulp::vellum::EditorFactory<Root> factory;
        editor = factory.create(processor, context, pulp::format::EditorRole::editor);
        REQUIRE(host != nullptr);
    }

    pulp::format::NativeParent parent() const {
        return {.kind = pulp::format::NativeParentKind::cocoa_view, .handle = host};
    }
};

} // namespace

TEST_CASE("Vellum editor factory supports editor and inspector roles", "[pulp-vellum][editor]") {
    Fixture fixture;
    Context context(*fixture.port);
    Processor processor;
    pulp::vellum::EditorFactory<TestView> factory;

    REQUIRE(factory.supports(pulp::format::EditorRole::editor));
    REQUIRE(factory.supports(pulp::format::EditorRole::inspector));
    REQUIRE_FALSE(factory.supports(pulp::format::EditorRole::settings));
    REQUIRE(factory.create(processor, context, pulp::format::EditorRole::settings) == nullptr);

    auto editor = factory.create(processor, context, pulp::format::EditorRole::editor);
    REQUIRE(editor);
    CHECK(editor->size().width == 400);
    CHECK(editor->size().height == 300);
    REQUIRE(editor->attach({.kind = pulp::format::NativeParentKind::none}));
    // An X11 parent carries window_id, not handle; the editor must not claim a
    // parent kind its attach path cannot use.
    CHECK_FALSE(editor->supports_parent(pulp::format::NativeParentKind::x11_window));
    CHECK_FALSE(
        editor->attach({.kind = pulp::format::NativeParentKind::x11_window, .window_id = 42}));
    REQUIRE(editor->size_hints().preferred_width == editor->size().width);
    REQUIRE(editor->size_hints().preferred_height == editor->size().height);
    REQUIRE(editor->set_size({640, 480, 2.0}));
    CHECK(editor->size().width == 640);
    CHECK(editor->size().height == 480);
    CHECK(editor->size_hints().preferred_width == 640);
    CHECK(editor->size_hints().preferred_height == 480);

    editor->invalidate();
    editor.reset();
}

TEST_CASE("Vellum view observes a bridged parameter after the UI pump", "[pulp-vellum][editor]") {
    Fixture fixture;
    Context context(*fixture.port);
    pulp::vellum::EditorEnv env{context, *fixture.model};
    TestView view(env);

    fixture.store.set_value_rt(1, 0.75f);
    fixture.store.pump_listeners();
    fixture.store.reconcile_restore_listeners();
    REQUIRE(view.observed == 0.75);
}

TEST_CASE("PluginViewHost::create() gives a Vellum view a native editor host",
          "[pulp-vellum][editor][host]") {
    using ::vellum::view::PluginViewHost;
    ::vellum::view::View root;
    PluginViewHost::Options options;
    options.size = {320, 200};
    options.use_gpu = true;
    REQUIRE(pulp::vellum::register_editor_dawn_bootstrap("com.pulp.test-vellum"));
    auto host = PluginViewHost::create(root, options);

    REQUIRE(host != nullptr);
    CHECK(root.plugin_view_host() == host.get());
    CHECK(host->native_handle() != nullptr);
    CHECK(host->is_gpu_backed());
    CHECK(host->get_size().width == 320);
    CHECK(host->get_size().height == 200);
    host->set_size(640, 480);
    CHECK(host->get_size().width == 640);
    CHECK(host->get_size().height == 480);
    // The host paints the tree into its layer before it is ever shown.
    CHECK(host->present_first_frame());
    CHECK_FALSE(host->capture_back_buffer_png().empty());

    // A second host is a native parent the first can attach to.
    ::vellum::view::View parent_root;
    auto parent = PluginViewHost::create(parent_root, PluginViewHost::Size{800, 600});
    REQUIRE(parent != nullptr);
    CHECK_FALSE(host->is_attached());
    CHECK(host->try_attach_to_parent(parent->native_handle()));
    host->detach();
    CHECK_FALSE(host->is_attached());

    host.reset();
    CHECK(root.plugin_view_host() == nullptr);
}

TEST_CASE("A registered PluginViewHost factory wins over the platform host",
          "[pulp-vellum][editor][host]") {
    using ::vellum::view::PluginViewHost;
    FactoryReset reset;
    StubHost* made = nullptr;
    PluginViewHost::set_factory([&made](::vellum::view::View&, const PluginViewHost::Options&) {
        auto stub = std::make_unique<StubHost>();
        made = stub.get();
        return std::unique_ptr<PluginViewHost>(std::move(stub));
    });
    REQUIRE(PluginViewHost::has_factory());

    ::vellum::view::View root;
    auto host = PluginViewHost::create(root, PluginViewHost::Size{100, 100});
    REQUIRE(host != nullptr);
    CHECK(host.get() == made);
    CHECK(root.plugin_view_host() == made);

    PluginViewHost::clear_factory();
    CHECK_FALSE(PluginViewHost::has_factory());
}

TEST_CASE("A Vellum editor attaches to a native parent view", "[pulp-vellum][editor][host]") {
#if !defined(__APPLE__)
    SKIP("Vellum's SDK ships its GPU plug-in view host on macOS only");
#endif
    Fixture fixture;
    Context context(*fixture.port);
    Processor processor;
    pulp::vellum::EditorFactory<TestView> factory;
    auto editor = factory.create(processor, context, pulp::format::EditorRole::editor);
    REQUIRE(editor);

    ::vellum::view::View parent_root;
    auto parent = ::vellum::view::PluginViewHost::create(
        parent_root, ::vellum::view::PluginViewHost::Size{800, 600});
    REQUIRE(parent != nullptr);
    CHECK(editor->attach(
        {.kind = pulp::format::NativeParentKind::cocoa_view, .handle = parent->native_handle()}));
    editor->detach();
    editor->invalidate();
}

TEST_CASE("A Vellum editor clears the root's host pointer before freeing the host",
          "[pulp-vellum][editor][host]") {
    using ::vellum::view::PluginViewHost;
    FactoryReset reset;
    // The root is destroyed after its host, and its teardown can still request
    // a repaint, so the root must stop pointing at the host first. A platform
    // host is not required to clear that pointer, so the editor does.
    PluginViewHost* made = nullptr;
    PluginViewHost* seen = nullptr;
    PluginViewHost::set_factory(
        [&made, &seen](::vellum::view::View& root, const PluginViewHost::Options&) {
            auto host = std::make_unique<RecordingHost>(root, &seen);
            made = host.get();
            return std::unique_ptr<PluginViewHost>(std::move(host));
        });

    Fixture fixture;
    Context context(*fixture.port);
    Processor processor;
    pulp::vellum::EditorFactory<EmptyView> factory;
    auto editor = factory.create(processor, context, pulp::format::EditorRole::editor);
    REQUIRE(editor);
    REQUIRE(made != nullptr);
    seen = made;
    editor.reset();
    CHECK(seen == nullptr);
}

TEST_CASE("A Vellum editor lays out its root and ticks it on idle", "[pulp-vellum][editor][host]") {
    using ::vellum::view::PluginViewHost;
    FactoryReset reset;
    PluginViewHost::set_factory([](::vellum::view::View&, const PluginViewHost::Options&) {
        return std::unique_ptr<PluginViewHost>(std::make_unique<StubHost>());
    });
    g_idle_ticks = 0;

    Fixture fixture;
    Context context(*fixture.port);
    Processor processor;
    pulp::vellum::EditorFactory<IdleProbeView> factory;
    auto editor = factory.create(processor, context, pulp::format::EditorRole::editor);
    REQUIRE(editor);
    CHECK(g_idle_ticks == 0);

    editor->idle();
    CHECK(g_idle_ticks == 1);
    CHECK(g_idle_width == editor->size().width);
    CHECK(g_idle_height == editor->size().height);

    REQUIRE(editor->set_size({640, 480, 1.0}));
    editor->idle();
    CHECK(g_idle_ticks == 2);
    CHECK(g_idle_width == 640.0f);
    CHECK(g_idle_height == 480.0f);
}

TEST_CASE("A Vellum editor captures through its PluginViewHost", "[pulp-vellum][editor][capture]") {
    HookFixture f;
    auto* automation = f.editor->automation();
    REQUIRE(automation != nullptr);

    std::vector<std::uint8_t> png;
    REQUIRE(automation->capture_png(png));
    CHECK(png == f.host->capture_bytes);
    CHECK(f.host->present_first_frame_calls == 1);
    CHECK(f.host->capture_back_buffer_calls == 1);
}

TEST_CASE("A Vellum root can opt into processor access", "[pulp-vellum][editor][processor-hook]") {
    HookFixture f;
    CHECK(f.root->processor == &f.processor);
    CHECK(&f.root->env.context == &f.context);
}

TEST_CASE("A Vellum root supplies the initial size and requests host resizing",
          "[pulp-vellum][editor][resize-hook]") {
    HookFixture f;
    CHECK(f.editor->size().width == 532);
    CHECK(f.editor->size().height == 316);
    CHECK(f.editor->size_hints().preferred_width == 532);
    CHECK(f.editor->size_hints().preferred_height == 316);
    CHECK(f.host->get_size().width == 532);
    CHECK(f.host->get_size().height == 316);
    CHECK(f.root->bounds().width == 532.0f);
    CHECK(f.root->bounds().height == 316.0f);

    REQUIRE(f.editor->attach(f.parent()));
    CHECK_FALSE(f.root->env.request_resize({700, 530, 1.5}));
    CHECK(f.context.resize_requests == 1);
    f.context.accept_resize = true;
    REQUIRE(f.root->env.request_resize({700, 530, 1.5}));
    CHECK(f.context.resize_requests == 2);
    CHECK(f.context.requested_size.width == 700);
    CHECK(f.context.requested_size.height == 530);
    CHECK(f.context.requested_size.scale == 1.5);
    CHECK(f.editor->size().width == 532);
    REQUIRE(f.editor->set_size(f.context.requested_size));
    CHECK(f.host->get_size().width == 700);
    CHECK(f.root->bounds().height == 530.0f);

    f.editor->detach();
    CHECK_FALSE(f.root->env.request_resize({700, 530, 1.5}));
    CHECK(f.context.resize_requests == 2);
}

TEST_CASE("Vellum reports the root's declared size hints", "[pulp-vellum][editor][size-hints]") {
    HookFixture<SizedHookView<>> f;
    REQUIRE(f.editor->set_size({1320, 860, 1.0}));
    const auto hints = f.editor->size_hints();
    CHECK(hints.preferred_width == 990);
    CHECK(hints.preferred_height == 645);
    CHECK(hints.min_width == 792);
    CHECK(hints.min_height == 516);
    CHECK(hints.max_width == 2640);
    CHECK(hints.max_height == 1720);
    CHECK(hints.aspect_ratio == Catch::Approx(1320.0 / 860));
    CHECK(hints.design_width == 1320);
    CHECK(hints.design_height == 860);
}

TEST_CASE("Vellum opens at the root's declared preferred size",
          "[pulp-vellum][editor][initial-size]") {
    HookFixture<SizedHookView<>> f;
    CHECK(f.editor->size().width == 990);
    CHECK(f.editor->size().height == 645);
    CHECK(f.host->get_size().width == 990);
    CHECK(f.host->get_size().height == 645);
}

TEST_CASE("Vellum passes the declared design viewport and aspect to the host",
          "[pulp-vellum][editor][host-viewport]") {
    HookFixture<SizedHookView<>> f;
    CHECK(f.host->design_viewport_calls == 1);
    CHECK(f.host->design_width == 1320.0f);
    CHECK(f.host->design_height == 860.0f);
    CHECK(f.host->fixed_aspect_calls == 1);
    CHECK(f.host->aspect_ratio == Catch::Approx(1320.0 / 860));
}

TEST_CASE("Vellum lays out the root at its design size through host resizes",
          "[pulp-vellum][editor][design-layout]") {
    HookFixture<SizedHookView<>> f;
    CHECK(f.root->bounds().width == 1320.0f);
    CHECK(f.root->bounds().height == 860.0f);
    REQUIRE(f.editor->set_size({1584, 1032, 1.0}));
    CHECK(f.host->get_size().width == 1584);
    CHECK(f.host->get_size().height == 1032);
    CHECK(f.root->bounds().width == 1320.0f);
    CHECK(f.root->bounds().height == 860.0f);
}

TEST_CASE("Vellum constrains host resizes to the root's size hints",
          "[pulp-vellum][editor][host-constraints]") {
    HookFixture<SizedHookView<>> f;
    auto check_size = [&](pulp::format::EditorSize request, uint32_t width, uint32_t height) {
        REQUIRE(f.editor->set_size(request));
        CHECK(f.editor->size().width == width);
        CHECK(f.editor->size().height == height);
        CHECK(f.editor->size().scale == request.scale);
        CHECK(f.host->get_size().width == width);
        CHECK(f.host->get_size().height == height);
    };
    check_size({400, 300, 1.0}, 792, 516);
    check_size({3000, 2000, 1.0}, 2640, 1720);
    check_size({1320, 1000, 1.5}, 1320, 860);
}

TEST_CASE("Vellum constrains root resize requests before they reach the host",
          "[pulp-vellum][editor][request-constraints]") {
    HookFixture<SizedHookView<>> f;
    REQUIRE(f.editor->attach(f.parent()));
    CHECK_FALSE(f.root->env.request_resize({3000, 2000, 1.5}));
    CHECK(f.context.resize_requests == 1);
    CHECK(f.context.requested_size.width == 2640);
    CHECK(f.context.requested_size.height == 1720);

    f.editor->detach();
    REQUIRE(f.editor->attach(f.parent()));
    f.context.accept_resize = true;
    REQUIRE(f.root->env.request_resize({400, 300, 1.5}));
    CHECK(f.context.resize_requests == 2);
    CHECK(f.context.requested_size.width == 792);
    CHECK(f.context.requested_size.height == 516);
    CHECK(f.context.requested_size.scale == 1.5);
    CHECK(f.editor->size().width == 990);
}

TEST_CASE("A Vellum root with size hints but no design size still follows host bounds",
          "[pulp-vellum][editor][responsive-layout]") {
    HookFixture<SizedHookView<false>> f;
    CHECK(f.host->design_viewport_calls == 0);
    CHECK(f.host->fixed_aspect_calls == 0);
    REQUIRE(f.editor->set_size({1320, 950, 1.0}));
    CHECK(f.root->bounds().width == 1320.0f);
    CHECK(f.root->bounds().height == 860.0f);
}

TEST_CASE("Vellum preserves legacy sizing without the size contract hook",
          "[pulp-vellum][editor][legacy-sizing]") {
    HookFixture f;
    CHECK(f.editor->size().width == 532);
    CHECK(f.editor->size().height == 316);
    CHECK(f.host->design_viewport_calls == 0);
    CHECK(f.host->fixed_aspect_calls == 0);
    REQUIRE(f.editor->set_size({701, 319, 1.5}));
    const auto hints = f.editor->size_hints();
    CHECK(hints.preferred_width == 701);
    CHECK(hints.preferred_height == 319);
    CHECK(hints.min_width == 1);
    CHECK(hints.min_height == 1);
    CHECK(hints.max_width == 0);
    CHECK(hints.max_height == 0);
    CHECK(hints.aspect_ratio == 0);
    CHECK(f.root->bounds().width == 701.0f);
    CHECK(f.root->bounds().height == 319.0f);
}

TEST_CASE("A Vellum root receives lifecycle callbacks at native attachment edges",
          "[pulp-vellum][editor][lifecycle-hook]") {
    HookFixture f;
    f.editor->detach();
    CHECK(f.editor->attach({.kind = pulp::format::NativeParentKind::none}));
    CHECK_FALSE(
        f.editor->attach({.kind = pulp::format::NativeParentKind::cocoa_view, .handle = &f}));
    CHECK(f.lifecycle.opens == 0);
    CHECK(f.lifecycle.closes == 0);
    REQUIRE(f.editor->attach(f.parent()));
    CHECK_FALSE(f.editor->attach(f.parent()));
    CHECK(f.lifecycle.opens == 1);
    CHECK(f.lifecycle.host_attached_at_open);
    f.editor->idle();
    CHECK(f.lifecycle.ticks == 1);

    f.editor->detach();
    f.editor->detach();
    CHECK(f.lifecycle.closes == 1);
    CHECK(f.lifecycle.host_attached_at_close);
    CHECK_FALSE(f.host->is_attached());
    f.editor->idle();
    CHECK(f.lifecycle.ticks == 1);

    REQUIRE(f.editor->attach(f.parent()));
    CHECK(f.lifecycle.opens == 2);
    f.editor->invalidate();
    CHECK(f.lifecycle.closes == 2);
    CHECK(f.root->plugin_view_host() == nullptr);
    f.editor->idle();
    CHECK(f.lifecycle.ticks == 1);
    CHECK_FALSE(f.editor->set_size({640, 480, 1.0}));
    f.editor.reset();
    CHECK(f.lifecycle.closes == 2);
}

#if defined(PULP_TEST_VELLUM_SCRIPTED_UI)
TEST_CASE("Vellum native messages are scoped to the editor attachment that registered them",
          "[pulp-vellum][editor][message-hook]") {
    HookFixture f;
    auto retained_env = f.root->env;
    // Keep an independent scripted root alive so a delivery can be attempted
    // even after the editor that supplied its handler is destroyed.
    ::vellum::view::View script_root;
    ::vellum::view::ScriptedUiSession session(
        script_root, *f.fixture.model,
        {.script_source = R"JS(createLabel('response', editorMessage('{"type":"ping"}'), '');)JS"});
    int calls = 0;
    auto register_handler = [&](const pulp::vellum::EditorEnv& env) {
        session.attach_native_message_handler(
            "editorMessage", env.native_message_handler([&calls](std::string_view json) {
                ++calls;
                CHECK(json == R"({"type":"ping"})");
                return R"({"ok":true,"type":"pong"})";
            }));
    };
    auto response = [&] {
        std::string error;
        const bool loaded = session.load(&error);
        INFO(error);
        REQUIRE(loaded);
        REQUIRE(session.bridge() != nullptr);
        auto* label = dynamic_cast<::vellum::view::Label*>(session.bridge()->widget("response"));
        REQUIRE(label != nullptr);
        return std::string(label->text());
    };

    register_handler(f.root->env);
    CHECK(response() == R"({"ok":true,"type":"pong"})");
    CHECK(calls == 1);
    REQUIRE(f.editor->attach(f.parent()));
    f.editor->detach();
    REQUIRE(f.editor->attach(f.parent()));
    CHECK(response() == R"({"ok":false,"error":"editor message handler unavailable"})");
    CHECK(calls == 1);
    register_handler(f.root->env);
    CHECK(response() == R"({"ok":true,"type":"pong"})");
    CHECK(calls == 2);
    f.editor.reset();
    CHECK(response() == R"({"ok":false,"error":"editor message handler unavailable"})");
    CHECK(calls == 2);
    CHECK_FALSE(retained_env.request_resize({600, 400, 1.0}));
    CHECK(f.context.resize_requests == 0);
    session.detach_native_message_handler("editorMessage");
}
#endif

#if defined(PULP_TEST_VELLUM_SCRIPTED_EDITOR)
namespace {

template <typename Widget>
Widget* find_widget(::vellum::view::View& view, std::string_view id = {}) {
    if (auto* widget = dynamic_cast<Widget*>(&view); widget && (id.empty() || view.id() == id))
        return widget;
    for (std::size_t i = 0; i < view.child_count(); ++i) {
        if (auto* widget = find_widget<Widget>(*view.child_at(i), id))
            return widget;
    }
    return nullptr;
}

} // namespace

TEST_CASE("Vellum scripted editor mounts and exchanges parameters through the host session",
          "[pulp-vellum][editor][scripted-editor]") {
    using pulp::format::ContractEditorSession;
    using ::vellum::view::PluginViewHost;
    FactoryReset reset;
    Fixture fixture;
    Processor processor;
    processor.set_state_store(&fixture.store);
    processor.reloadable = true;
    int gesture_begins = 0;
    int gesture_ends = 0;
    fixture.store.set_gesture_callbacks(
        [&](pulp::state::ParamID id) {
            if (id == 1)
                ++gesture_begins;
        },
        [&](pulp::state::ParamID id) {
            if (id == 1)
                ++gesture_ends;
        });

    ::vellum::view::View* root = nullptr;
    StubHost* host = nullptr;
    PluginViewHost::set_factory(
        [&](::vellum::view::View& view, const PluginViewHost::Options& options) {
            root = &view;
            auto stub = std::make_unique<StubHost>(options.size);
            host = stub.get();
            return stub;
        });
    REQUIRE(pulp::format::embeds_editor_script(pulp::format::registered_editor_assets()));
    auto session = ContractEditorSession::create(processor, fixture.store,
                                                 *pulp::vellum::scripted_editor_factory(), {},
                                                 ContractEditorSession::IdleSource::host_driven);
    REQUIRE(session);
    REQUIRE(root);
    REQUIRE(host);
    REQUIRE(session->attach({.kind = pulp::format::NativeParentKind::cocoa_view, .handle = host}));
    auto* knob = find_widget<::vellum::view::Knob>(*root);
    REQUIRE(knob);
    REQUIRE(knob->value() == Catch::Approx(0.5f));

    fixture.store.set_value_rt(1, 0.25f);
    CHECK(knob->value() == Catch::Approx(0.5f));
    session->tick();
    REQUIRE(knob->value() == Catch::Approx(0.25f));
    const auto saved = pulp::format::plugin_state_io::serialize(fixture.store, processor);

    knob->on_mouse_down({24, 32});
    knob->on_mouse_drag({24, 8});
    knob->on_mouse_up({24, 8});
    REQUIRE(fixture.store.get_normalized(1) > 0.25f);
    CHECK(fixture.store.get_normalized(1) == Catch::Approx(knob->value()));
    CHECK(gesture_begins == 1);
    CHECK(gesture_ends == 1);

    REQUIRE(pulp::format::plugin_state_io::deserialize(saved, fixture.store, processor));
    CHECK(knob->value() > 0.25f);
    session->tick();
    CHECK(knob->value() == Catch::Approx(0.25f));

    auto initial_value = [&] {
        const auto* label = find_widget<::vellum::view::Label>(*root);
        REQUIRE(label);
        return std::string(label->text());
    };
    CHECK(initial_value() == "0.5");
    ++processor.generation;
    session->tick();
    CHECK(initial_value() == "0.25");
}
#endif

#if defined(PULP_TEST_VELLUM_SCRIPTED_AUDIO)
namespace {

class ScriptVisualPort final : public pulp::format::VisualPort {
  public:
    std::size_t channel_count() const noexcept override {
        return 5;
    }

    pulp::format::VisualChannelInfo channel(std::size_t index) const noexcept override {
        using Shape = pulp::format::VisualChannelShape;
        constexpr std::array channels{
            pulp::format::VisualChannelInfo{"input-spectrum", {}, Shape::vector, 0.0f},
            pulp::format::VisualChannelInfo{"aux", {}, Shape::scalar, 0.0f},
            pulp::format::VisualChannelInfo{"input-meter", {}, Shape::meter, 0.0f},
            pulp::format::VisualChannelInfo{"meter", {}, Shape::meter, 0.0f},
            pulp::format::VisualChannelInfo{"scope", {}, Shape::vector, 0.0f},
        };
        return channels[index];
    }

    bool read_scalar(std::size_t, float&) noexcept override {
        return false;
    }

    std::size_t read_meter(std::size_t index, std::span<float> rms,
                           std::span<float> peak) noexcept override {
        if ((index != 2 && index != 3) || rms.empty() || peak.empty())
            return 0;
        rms.front() = index == 3 ? level : 0.125f;
        peak.front() = rms.front() + 0.2f;
        return 1;
    }

    std::size_t read_vector(std::size_t index, std::span<float> values) noexcept override {
        if ((index != 0 && index != 4) || values.size() < bins.size())
            return 0;
        std::copy(bins.begin(), bins.end(), values.begin());
        return index == 4 ? bins.size() : 1;
    }

    std::size_t read_events(std::size_t, std::span<pulp::format::VisualEvent>) noexcept override {
        return 0;
    }

    std::uint32_t publish_count(std::size_t index) const noexcept override {
        return index < channel_count() && index != 1 ? frame : 0;
    }

    float level = 0.0f;
    std::uint32_t frame = 0;
    std::array<float, 3> bins{-60.0f, -24.0f, -6.0f};
};

} // namespace

TEST_CASE("Vellum scripted editor samples its own visual feed on idle",
          "[pulp-vellum][editor][scripted-editor]") {
    using pulp::vellum_audio::AnalyzerScope;
    using pulp::vellum_audio::LevelMeter;
    using ::vellum::view::PluginViewHost;
    FactoryReset reset;
    ::vellum::view::View* root = nullptr;
    PluginViewHost::set_factory(
        [&](::vellum::view::View& view, const PluginViewHost::Options& options) {
            root = &view;
            return std::make_unique<StubHost>(options.size);
        });

    Fixture first, second;
    ScriptVisualPort first_port, second_port;
    Context first_context{*first.port, &first_port};
    Context second_context{*second.port, &second_port};
    Processor first_processor, second_processor;
    first_processor.reloadable = true;
    auto& factory = *pulp::vellum::scripted_editor_factory();
    auto first_editor =
        factory.create(first_processor, first_context, pulp::format::EditorRole::editor);
    REQUIRE(first_editor);
    REQUIRE(root);
    auto& first_root = *root;
    auto second_editor =
        factory.create(second_processor, second_context, pulp::format::EditorRole::editor);
    REQUIRE(second_editor);
    REQUIRE(root != &first_root);
    REQUIRE(first_editor->attach({.kind = pulp::format::NativeParentKind::none}));
    REQUIRE(second_editor->attach({.kind = pulp::format::NativeParentKind::none}));

    first_port.level = 0.55f;
    ++first_port.frame;
    second_port.level = 0.25f;
    ++second_port.frame;
    first_editor->idle();
    second_editor->idle();

    const auto require_widgets = [](::vellum::view::View& view, float level) {
        const auto* meter = find_widget<LevelMeter>(view, "meter");
        const auto* default_meter = find_widget<LevelMeter>(view, "default-meter");
        const auto* scope = find_widget<AnalyzerScope>(view);
        REQUIRE(meter);
        REQUIRE(default_meter);
        REQUIRE(scope);
        CHECK(meter->rms() == Catch::Approx(level));
        CHECK(meter->peak() == Catch::Approx(level + 0.2f));
        CHECK(default_meter->rms() == Catch::Approx(0.125f));
        CHECK(scope->column_count() == 3);
        CHECK_FALSE(meter->stale());
        CHECK_FALSE(scope->stale());
    };
    require_widgets(first_root, first_port.level);
    require_widgets(*root, second_port.level);

    first_port.level = 0.65f;
    ++first_port.frame;
    ++first_processor.generation;
    second_editor.reset();
    first_editor->idle();
    require_widgets(first_root, first_port.level);
}
#endif

TEST_CASE("Vellum editor host opens on the plug-in's declared editor colour",
          "[pulp-vellum][editor][host]") {
    using ::vellum::view::PluginViewHost;
    FactoryReset reset;
    std::optional<std::uint32_t> seen;
    PluginViewHost::set_factory(
        [&seen](::vellum::view::View&, const PluginViewHost::Options& options) {
            seen = options.background_rgb;
            return std::unique_ptr<PluginViewHost>(std::make_unique<StubHost>(options.size));
        });
    Fixture fixture;
    Context context(*fixture.port);
    pulp::vellum::EditorFactory<TestView> factory;

    SECTION("a declared colour reaches the host") {
        Processor processor;
        processor.background = 0x05070A;
        auto editor = factory.create(processor, context, pulp::format::EditorRole::editor);
        REQUIRE(editor);
        REQUIRE(seen.has_value());
        CHECK(*seen == 0x05070Au);
    }
    SECTION("no declared colour keeps the host default") {
        Processor processor;
        auto editor = factory.create(processor, context, pulp::format::EditorRole::editor);
        REQUIRE(editor);
        REQUIRE(seen.has_value());
        CHECK(*seen == ::vellum::view::kEditorHostClearRgb);
    }
}
