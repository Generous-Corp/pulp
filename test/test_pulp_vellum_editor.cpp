// First, so this file proves the header compiles on its own, as a generated
// editor package includes it.
#include <pulp/runtime/interaction_event.hpp>
#include <pulp/runtime/interaction_router.hpp>
#include <pulp/vellum/auto_ui.hpp>
#include <pulp/vellum/editor.hpp>
#include <pulp/vellum/interaction_adapter.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/format/editor_host.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/runtime/log_sink.hpp>
#include <pulp/state/store.hpp>

#include <vellum/graphics/dawn_bootstrap.hpp>
#include <vellum/value/param_model.hpp>
#include <vellum/view/plugin_view_host.hpp>
#include <vellum/view/ui_components.hpp>
#include <vellum/view/view.hpp>
#include <vellum/view/widgets.hpp>
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
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

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
    void repaint() override {
        ++repaint_calls;
    }
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
    void resume_presentation() {
        ++resume_presentation_calls;
        if constexpr (requires(PluginViewHost& host) { host.resume_presentation(); })
            PluginViewHost::resume_presentation();
    }
    bool presentation_allowed() const noexcept {
        if constexpr (requires(const StubHost& host) { host.root_presentation_allowed(); })
            return root_presentation_allowed();
        return true;
    }

    int resume_presentation_calls = 0;
    int design_viewport_calls = 0;
    int fixed_aspect_calls = 0;
    int repaint_calls = 0;
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

    explicit HookFixture(pulp::format::VisualPort* visuals = nullptr)
        : context(*fixture.port, visuals) {
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

TEST_CASE("Vellum editor reports a Dawn bootstrap failure on attach",
          "[pulp-vellum][.bootstrap-failure]") {
    using namespace ::vellum::graphics;
    const DawnBootstrap incompatible{
        .abi_version = kDawnBootstrapAbiVersion,
        .callback = +[](const DawnBootstrapRequest&, void*,
                        std::string*) { return DawnBootstrapResult::ready; },
    };
    // This registration owns the process, so CTest runs this case on its own.
    REQUIRE(register_dawn_bootstrap(incompatible, "test-incompatible-revision", nullptr));
    std::string reason;
    REQUIRE_FALSE(::vellum::app_host::register_native_dawn_bootstrap(&reason));
    REQUIRE_FALSE(reason.empty());

    pulp::runtime::CaptureLogSink capture;
    pulp::runtime::ScopedLogSink log(capture.sink());
    FactoryReset reset;
    int hosts_created = 0;
    ::vellum::view::PluginViewHost::set_factory(
        [&](::vellum::view::View&, const ::vellum::view::PluginViewHost::Options&) {
            ++hosts_created;
            return std::make_unique<StubHost>();
        });
    Fixture fixture;
    Context context(*fixture.port);
    Processor processor;
    pulp::vellum::EditorFactory<EmptyView> factory;
    for (int i = 0; i < 2; ++i) {
        auto editor = factory.create(processor, context, pulp::format::EditorRole::editor);
        REQUIRE(editor);
        CHECK_FALSE(editor->attach({.kind = pulp::format::NativeParentKind::none}));
        CHECK_FALSE(editor->attach(
            {.kind = pulp::format::NativeParentKind::cocoa_view, .handle = &context}));
        const auto attach_error = []<typename Editor>(const Editor& failed) -> std::string_view {
            if constexpr (requires { failed.attach_error(); })
                return failed.attach_error();
            return {};
        };
        CHECK(attach_error(*editor) == reason);
    }
    CHECK(hosts_created == 0);
    const auto errors = capture.records_at_least(pulp::runtime::LogLevel::Error);
    REQUIRE(errors.size() == 1);
    CHECK(errors.front().message.find(reason) != std::string::npos);
}

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

namespace {

class TestVisualPort final : public pulp::format::VisualPort {
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

TEST_CASE("Vellum editor idle repaints for visual publishes and preserves view damage",
          "[pulp-vellum][editor][idle-damage]") {
    TestVisualPort visual_port;
    HookFixture fixture(&visual_port);
    REQUIRE(fixture.editor->attach(fixture.parent()));
    fixture.host->clear_pending_dirty();
    fixture.host->repaint_calls = 0;

    for (int i = 0; i < 60; ++i)
        fixture.editor->idle();
    CHECK(fixture.lifecycle.ticks == 60);
    CHECK(fixture.host->repaint_calls == 0);

    fixture.host->repaint_calls = 0;
    visual_port.level = 0.5f;
    ++visual_port.frame;
    fixture.editor->idle();
    CHECK(fixture.host->repaint_calls == 1);
    fixture.editor->idle();
    CHECK(fixture.host->repaint_calls == 1);

    fixture.host->repaint_calls = 0;
    fixture.host->mark_dirty_region({1, 2, 3, 4});
    REQUIRE(fixture.host->has_pending_dirty_bounds());
    CHECK(fixture.host->repaint_calls == 1);
    fixture.editor->idle();
    CHECK(fixture.host->repaint_calls == 1);
    CHECK(fixture.host->pending_dirty_bounds().width == 3.0f);
}

#if defined(PULP_TEST_VELLUM_SCRIPTED_AUDIO)
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
    TestVisualPort first_port, second_port;
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

class InteractionView final : public ::vellum::view::View {
  public:
    InteractionView() = default;

    void on_mouse_event(const ::vellum::view::MouseEvent& event) override {
        events.push_back(event);
    }

    std::vector<::vellum::view::MouseEvent> events;
};

TEST_CASE("runtime interactions route in sequence order and reject stale epochs",
          "[pulp-vellum][interaction]") {
    InteractionView view;
    pulp::vellum::InteractionAdapter adapter;
    pulp::runtime::InteractionBatch<> batch;
    batch.append({.epoch = 4,
                  .sequence = 2,
                  .object_id = 7,
                  .phase = pulp::runtime::InteractionPhase::move,
                  .x = 2.0f});
    batch.append({.epoch = 4,
                  .sequence = 1,
                  .object_id = 7,
                  .phase = pulp::runtime::InteractionPhase::press,
                  .x = 1.0f});
    REQUIRE(adapter.deliver(view, batch));
    REQUIRE(view.events.size() == 2);
    REQUIRE(view.events[0].phase == ::vellum::view::MousePhase::press);
    REQUIRE(view.events[1].phase == ::vellum::view::MousePhase::drag);

    pulp::runtime::InteractionBatch<> stale;
    stale.append({.epoch = 3,
                  .sequence = 3,
                  .object_id = 7,
                  .phase = pulp::runtime::InteractionPhase::release,
                  .x = 3.0f});
    REQUIRE_FALSE(adapter.deliver(view, stale));
    REQUIRE(view.events.size() == 2);
}

TEST_CASE("runtime interaction batch routes only the first event's source",
          "[pulp-vellum][interaction]") {
    InteractionView view;
    pulp::vellum::InteractionAdapter adapter;
    pulp::runtime::InteractionBatch<> batch;
    batch.append({.source_id = 1,
                  .epoch = 1,
                  .sequence = 1,
                  .object_id = 5,
                  .phase = pulp::runtime::InteractionPhase::press,
                  .x = 1.0f});
    batch.append({.source_id = 2,
                  .epoch = 1,
                  .sequence = 2,
                  .object_id = 6,
                  .phase = pulp::runtime::InteractionPhase::press,
                  .x = 2.0f});
    batch.append({.source_id = 1,
                  .epoch = 1,
                  .sequence = 3,
                  .object_id = 5,
                  .phase = pulp::runtime::InteractionPhase::move,
                  .x = 3.0f});
    REQUIRE(adapter.deliver(view, batch));
    REQUIRE(view.events.size() == 2);
    REQUIRE(view.events[0].phase == ::vellum::view::MousePhase::press);
    REQUIRE(view.events[1].phase == ::vellum::view::MousePhase::drag);

    pulp::runtime::InteractionBatch<> only_other;
    only_other.append({.source_id = 2,
                       .epoch = 1,
                       .sequence = 4,
                       .object_id = 6,
                       .phase = pulp::runtime::InteractionPhase::move,
                       .x = 4.0f});
    REQUIRE_FALSE(adapter.deliver(view, only_other));
    REQUIRE(view.events.size() == 2);
}

TEST_CASE("runtime interactions keep object pointers independent and cancel them",
          "[pulp-vellum][interaction]") {
    InteractionView view;
    pulp::vellum::InteractionAdapter adapter;
    pulp::runtime::InteractionBatch<> batch;
    batch.append({.epoch = 1,
                  .sequence = 1,
                  .object_id = 10,
                  .phase = pulp::runtime::InteractionPhase::press,
                  .x = 1.0f});
    batch.append({.epoch = 1,
                  .sequence = 2,
                  .object_id = 11,
                  .phase = pulp::runtime::InteractionPhase::press,
                  .x = 2.0f});
    batch.append({.epoch = 1,
                  .sequence = 3,
                  .object_id = 10,
                  .phase = pulp::runtime::InteractionPhase::cancel,
                  .x = 3.0f});
    REQUIRE(adapter.deliver(view, batch));
    REQUIRE(view.events.size() == 3);
    REQUIRE(view.events[0].pointer_id != view.events[1].pointer_id);
    REQUIRE(view.events[2].is_cancelled);

    pulp::runtime::InteractionBatch<> after_cancel;
    after_cancel.append({.epoch = 1,
                         .sequence = 4,
                         .object_id = 10,
                         .phase = pulp::runtime::InteractionPhase::move,
                         .x = 4.0f});
    REQUIRE_FALSE(adapter.deliver(view, after_cancel));
    REQUIRE(view.events.size() == 3);
}

TEST_CASE("tracker router adapter scales events and cancels each consumer once",
          "[pulp-vellum][interaction]") {
    InteractionView view;
    pulp::vellum::InteractionAdapter adapter;
    adapter.set_view_metrics(100.0f, 80.0f, 2.0f);
    pulp::runtime::InteractionRouter<8, 8> router;
    REQUIRE(router.publish({.source_id = 5,
                            .epoch = 1,
                            .sequence = 1,
                            .object_id = 3,
                            .phase = pulp::runtime::InteractionPhase::press,
                            .x = .25f,
                            .y = .5f}));
    REQUIRE(router.publish({.source_id = 5,
                            .epoch = 2,
                            .sequence = 1,
                            .object_id = 4,
                            .phase = pulp::runtime::InteractionPhase::press,
                            .x = .75f,
                            .y = .25f}));
    pulp::runtime::InteractionBatch<8> batch;
    pulp::runtime::InteractionEvent event{};
    while (router.try_pop_ui(event))
        REQUIRE(batch.append(event));
    REQUIRE(adapter.deliver(view, batch));
    REQUIRE(view.events.size() == 3);
    CHECK(view.events[0].phase == ::vellum::view::MousePhase::press);
    CHECK(view.events[1].is_cancelled);
    CHECK(view.events[2].position.x == Catch::Approx(150.0f));
    CHECK(view.events[2].position.y == Catch::Approx(40.0f));

    std::size_t audio_cancels = 0;
    while (router.try_pop_audio(event))
        audio_cancels += event.phase == pulp::runtime::InteractionPhase::cancel;
    CHECK(audio_cancels == 1);
}

TEST_CASE("runtime interaction batch remains bounded", "[pulp-vellum][interaction]") {
    pulp::runtime::InteractionBatch<2> batch;
    REQUIRE(batch.append({}));
    REQUIRE(batch.append({}));
    REQUIRE_FALSE(batch.append({}));
    REQUIRE(batch.count == 2);
}

TEST_CASE("runtime interaction coordinates scale to Vellum backing pixels",
          "[pulp-vellum][interaction]") {
    InteractionView view;
    pulp::vellum::InteractionAdapter adapter;
    adapter.set_view_metrics(200.0f, 100.0f, 2.0f);
    pulp::runtime::InteractionBatch<> batch;
    batch.append({.source_id = 9,
                  .epoch = 1,
                  .sequence = 1,
                  .object_id = 21,
                  .phase = pulp::runtime::InteractionPhase::press,
                  .x = 0.25f,
                  .y = 0.5f});
    REQUIRE(adapter.deliver(view, batch));
    REQUIRE(view.events.size() == 1);
    CHECK(view.events[0].position.x == Catch::Approx(100.0f));
    CHECK(view.events[0].position.y == Catch::Approx(100.0f));

    pulp::runtime::InteractionBatch<> release;
    release.append({.source_id = 9,
                    .epoch = 1,
                    .sequence = 2,
                    .object_id = 21,
                    .phase = pulp::runtime::InteractionPhase::release,
                    .x = 0.25f,
                    .y = 0.5f});
    REQUIRE(adapter.deliver(view, release));

    pulp::runtime::InteractionBatch<> recycled;
    recycled.append({.source_id = 9,
                     .epoch = 1,
                     .sequence = 3,
                     .object_id = 22,
                     .phase = pulp::runtime::InteractionPhase::press,
                     .x = 0.1f,
                     .y = 0.1f});
    REQUIRE(adapter.deliver(view, recycled));
    REQUIRE(view.events.size() == 3);
    CHECK(view.events[2].pointer_id == view.events[0].pointer_id);
}

namespace {

// A root that opts into content-first presentation and reports when its
// content is ready, with a test-controlled timeout.
bool g_content_ready = false;
std::chrono::milliseconds g_presentation_timeout{60'000};

class DeferredView final : public ::vellum::view::View {
  public:
    explicit DeferredView(pulp::vellum::EditorEnv&) {}
    static bool editor_defers_presentation() {
        return true;
    }
    bool editor_content_ready() const {
        return g_content_ready;
    }
    std::chrono::milliseconds editor_presentation_timeout() const {
        return g_presentation_timeout;
    }
};

// A deferring root with no readiness hook: its content is there at attach.
class DeferredNativeView final : public ::vellum::view::View {
  public:
    explicit DeferredNativeView(pulp::vellum::EditorEnv&) {}
    static bool editor_defers_presentation() {
        return true;
    }
};

template <typename Root> struct PresentationFixture {
    using PluginViewHost = ::vellum::view::PluginViewHost;

    FactoryReset reset;
    Fixture fixture;
    Context context{*fixture.port};
    Processor processor;
    StubHost* host = nullptr;
    std::optional<bool> deferred;
    std::unique_ptr<pulp::format::Editor> editor;

    PresentationFixture() {
        PluginViewHost::set_factory(
            [this](::vellum::view::View&, const PluginViewHost::Options& options) {
                if constexpr (requires(const PluginViewHost::Options& o) { o.defer_presentation; })
                    deferred = options.defer_presentation;
                else
                    deferred = false;
                auto stub = std::make_unique<StubHost>(options.size);
                host = stub.get();
                return stub;
            });
        pulp::vellum::EditorFactory<Root> factory;
        editor = factory.create(processor, context, pulp::format::EditorRole::editor);
        REQUIRE(host != nullptr);
    }

    bool attach() {
        return editor->attach({.kind = pulp::format::NativeParentKind::cocoa_view, .handle = host});
    }
};

struct PresentationGlobals {
    PresentationGlobals() {
        g_content_ready = false;
        g_presentation_timeout = std::chrono::milliseconds{60'000};
    }
    ~PresentationGlobals() {
        g_content_ready = false;
        g_presentation_timeout = std::chrono::milliseconds{60'000};
    }
};

constexpr bool has_presentation_api() {
    return requires(::vellum::view::PluginViewHost& host) { host.resume_presentation(); }&&
        requires(::vellum::view::PluginViewHost::Options & options)
    {
        options.defer_presentation = true;
    };
}

} // namespace

TEST_CASE("A Vellum root that defers presentation presents nothing until its content is ready",
          "[pulp-vellum][editor][presentation]") {
    if constexpr (!has_presentation_api())
        SKIP("installed Vellum SDK has no deferred-presentation host API");
    PresentationGlobals globals;
    PresentationFixture<DeferredView> f;
    REQUIRE(f.deferred == std::optional<bool>{true});
    REQUIRE(f.attach());

    // Attached and ticking, but not ready: the host keeps its native layer.
    for (int i = 0; i < 5; ++i)
        f.editor->idle();
    CHECK(f.host->resume_presentation_calls == 0);
    CHECK(f.host->present_first_frame_calls == 0);
    CHECK_FALSE(f.host->presentation_allowed());

    // Ready: the host presents, exactly once.
    g_content_ready = true;
    f.editor->idle();
    CHECK(f.host->resume_presentation_calls == 1);
    CHECK(f.host->presentation_allowed());
    f.editor->idle();
    CHECK(f.host->resume_presentation_calls == 1);

    // A later attach does not defer again.
    f.editor->detach();
    REQUIRE(f.attach());
    f.editor->idle();
    CHECK(f.host->resume_presentation_calls == 1);
}

TEST_CASE("A deferred Vellum root that never reports ready still presents after its timeout",
          "[pulp-vellum][editor][presentation]") {
    if constexpr (!has_presentation_api())
        SKIP("installed Vellum SDK has no deferred-presentation host API");
    PresentationGlobals globals;
    g_presentation_timeout = std::chrono::milliseconds{0};
    PresentationFixture<DeferredView> f;
    REQUIRE(f.attach());
    // The timeout runs from idle, never from attach itself.
    CHECK(f.host->resume_presentation_calls == 0);
    f.editor->idle();
    CHECK(f.host->resume_presentation_calls == 1);
}

TEST_CASE("A detached deferred Vellum root does not present from a stale timeout",
          "[pulp-vellum][editor][presentation]") {
    if constexpr (!has_presentation_api())
        SKIP("installed Vellum SDK has no deferred-presentation host API");
    PresentationGlobals globals;
    g_presentation_timeout = std::chrono::milliseconds{0};
    PresentationFixture<DeferredView> f;
    REQUIRE(f.attach());
    f.editor->detach();
    f.editor->idle();
    CHECK(f.host->resume_presentation_calls == 0);
}

TEST_CASE("A deferring Vellum root without a readiness hook presents at attach",
          "[pulp-vellum][editor][presentation]") {
    if constexpr (!has_presentation_api())
        SKIP("installed Vellum SDK has no deferred-presentation host API");
    PresentationFixture<DeferredNativeView> f;
    REQUIRE(f.deferred == std::optional<bool>{true});
    CHECK(f.host->resume_presentation_calls == 0);
    REQUIRE(f.attach());
    CHECK(f.host->resume_presentation_calls == 1);
}

TEST_CASE("A Vellum root that does not opt in keeps immediate presentation",
          "[pulp-vellum][editor][presentation]") {
    if constexpr (!has_presentation_api())
        SKIP("installed Vellum SDK has no deferred-presentation host API");
    PresentationFixture<TestView> f;
    REQUIRE(f.deferred == std::optional<bool>{false});
    REQUIRE(f.attach());
    f.editor->idle();
    CHECK(f.host->resume_presentation_calls == 0);
    CHECK(f.host->presentation_allowed());
}

// ── A VST3 host opening a Vellum editor ─────────────────────────────────────
//
// PulpVst3Processor::createView() returns the view-free ContractPlugView,
// which drives a pulp::vellum::Editor and the Vellum PluginViewHost it owns.
// Each case stands in for the DAW and calls the IPlugView methods a host
// calls, then checks what the Vellum root and its host saw. Rendering stays
// offscreen: the parent is another Vellum host's native view, never shown.

#if defined(PULP_TEST_VELLUM_VST3)

#include <pulp/format/vst3_adapter.hpp>
#include <pulp/format/vst3_contract_view.hpp>

#include <pluginterfaces/base/keycodes.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <public.sdk/source/common/pluginview.h>

#include <vellum/view/text_editor.hpp>

#include <cstdlib>
#include <string>

using pulp::format::vst3::ContractPlugView;
using pulp::format::vst3::PulpVst3Processor;

namespace vst3_cases {

using PluginViewHost = ::vellum::view::PluginViewHost;

/// What the root saw, reset by each case.
struct Vst3RootLog {
    int opens = 0;
    int closes = 0;
    ::vellum::view::View* root = nullptr;
    ::vellum::view::TextEditor* field = nullptr;
};
Vst3RootLog g_vst3_root;

/// An aspect-locked root with one text field.
class Vst3FieldView final : public ::vellum::view::View {
  public:
    explicit Vst3FieldView(pulp::vellum::EditorEnv&) {
        auto field = std::make_unique<::vellum::view::TextEditor>();
        g_vst3_root.field = field.get();
        add_child(std::move(field));
        g_vst3_root.root = this;
    }
    ~Vst3FieldView() override {
        g_vst3_root.root = nullptr;
        g_vst3_root.field = nullptr;
    }

    pulp::format::ViewSize editor_view_size() const {
        return {.preferred_width = 400,
                .preferred_height = 300,
                .min_width = 200,
                .min_height = 150,
                .max_width = 800,
                .max_height = 600,
                .aspect_ratio = 4.0 / 3.0};
    }
    void editor_opened() {
        ++g_vst3_root.opens;
    }
    void editor_closing() {
        ++g_vst3_root.closes;
    }
};

class Vst3FieldProcessor final : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor d;
        d.name = "Vellum VST3 Editor";
        d.manufacturer = "Pulp";
        d.bundle_id = "dev.pulp.test.vst3-vellum-editor";
        d.version = "1.0.0";
        d.input_buses = {{"Audio In", 2}};
        d.output_buses = {{"Audio Out", 2}};
        return d;
    }
    void define_parameters(pulp::state::StateStore& store) override {
        pulp::state::ParamInfo info;
        info.id = 1;
        info.name = "Gain";
        info.range = {0.0f, 1.0f, 0.5f};
        store.add_parameter(info);
    }
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& out, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        out.clear();
    }
    pulp::format::EditorFactory* editor_factory() override {
        return pulp::vellum::editor_factory<Vst3FieldView>();
    }
};

std::unique_ptr<pulp::format::Processor> create_vst3_field_processor() {
    return std::make_unique<Vst3FieldProcessor>();
}

class Vst3HostApp final : public Steinberg::Vst::IHostApplication {
  public:
    Steinberg::tresult PLUGIN_API getName(Steinberg::Vst::String128 name) override {
        name[0] = 0;
        return Steinberg::kResultTrue;
    }
    Steinberg::tresult PLUGIN_API createInstance(Steinberg::TUID, Steinberg::TUID,
                                                 void** obj) override {
        if (obj)
            *obj = nullptr;
        return Steinberg::kNotImplemented;
    }
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IHostApplication::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid)) {
            *obj = static_cast<Steinberg::Vst::IHostApplication*>(this);
            return Steinberg::kResultTrue;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override {
        return 1;
    }
    Steinberg::uint32 PLUGIN_API release() override {
        return 1;
    }
};

// Unset an environment variable for one scope, then put back what was there.
class Vst3ScopedEnvUnset {
  public:
    explicit Vst3ScopedEnvUnset(const char* name) : name_(name) {
        if (const char* prev = std::getenv(name))
            prev_ = std::string(prev);
        ::unsetenv(name_.c_str());
    }
    ~Vst3ScopedEnvUnset() {
        if (prev_)
            ::setenv(name_.c_str(), prev_->c_str(), 1);
    }

  private:
    std::string name_;
    std::optional<std::string> prev_;
};

// createView() refuses editors in CI and headless environments; these cases
// open one offscreen.
struct Vst3EditorEnvironment {
    Vst3ScopedEnvUnset disable_editor{"PULP_DISABLE_PLUGIN_EDITOR"};
    Vst3ScopedEnvUnset headless{"PULP_HEADLESS"};
    Vst3ScopedEnvUnset test_mode{"PULP_TEST_MODE"};
    Vst3ScopedEnvUnset ci{"CI"};
};

// Clears any host factory a case registers, even when an assertion stops it.
struct Vst3FactoryReset {
    ~Vst3FactoryReset() {
        PluginViewHost::clear_factory();
    }
};

// Both focus slots are process-global; leave them clear between cases.
struct Vst3FocusReset {
    Vst3FocusReset() {
        ::vellum::view::View::focused_input_ = nullptr;
        ::vellum::view::View::active_overlay_ = nullptr;
    }
    ~Vst3FocusReset() {
        ::vellum::view::View::focused_input_ = nullptr;
        ::vellum::view::View::active_overlay_ = nullptr;
    }
};

// A host whose native view never accepts a parent.
class Vst3RefusingHost final : public PluginViewHost {
  public:
    explicit Vst3RefusingHost(Size size) : size_(size) {}
    ::vellum::view::NativeViewHandle native_handle() override {
        return this;
    }
    void attach_to_parent(::vellum::view::NativeViewHandle) override {}
    bool is_attached() const noexcept override {
        return false;
    }
    void detach() override {
        ++detaches;
    }
    void repaint() override {}
    void set_size(uint32_t width, uint32_t height) override {
        size_ = {width, height};
    }
    Size get_size() const override {
        return size_;
    }

    int detaches = 0;

  private:
    Size size_;
};

/// The adapter, its host-side objects and the offscreen parent view.
struct Vst3Fixture {
    Vst3EditorEnvironment env;
    Vst3FocusReset focus;
    Vst3HostApp host_app;
    PulpVst3Processor processor{create_vst3_field_processor};
    ::vellum::view::View parent_root;
    std::unique_ptr<PluginViewHost> parent;

    Vst3Fixture() {
        g_vst3_root = {};
        REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
    }
    ~Vst3Fixture() {
        processor.terminate();
    }

    // A second Vellum host stands in for the DAW's NSView.
    void* native_parent() {
        if (!parent) {
            parent = PluginViewHost::create(parent_root, PluginViewHost::Size{800, 600});
            REQUIRE(parent != nullptr);
        }
        return parent->native_handle();
    }

    ContractPlugView* create_view() {
        auto* raw = processor.createView(Steinberg::Vst::ViewType::kEditor);
        REQUIRE(raw != nullptr);
        auto* view = dynamic_cast<ContractPlugView*>(raw);
        REQUIRE(view != nullptr);
        return view;
    }
};

Steinberg::tresult press(ContractPlugView& view, Steinberg::char16 key, Steinberg::int16 code,
                         Steinberg::int16 modifiers = 0) {
    return view.onKeyDown(key, code, modifiers);
}

} // namespace vst3_cases

using namespace vst3_cases;

namespace pulp::format::vst3 {

// The create_view() path is not part of this test; a processor here always
// supplies an editor factory, so this is never reached.
Steinberg::IPlugView* make_plug_view(Processor&, state::StateStore&, runtime::AliveToken::Handle) {
    FAIL("a Vellum editor never takes the create_view() path");
    return nullptr;
}

} // namespace pulp::format::vst3

TEST_CASE("A VST3 host opens, sizes and closes a Vellum editor",
          "[pulp-vellum][vst3][editor-host]") {
#if !defined(__APPLE__)
    SKIP("Vellum's SDK ships its GPU plug-in view host on macOS only");
#endif
    Vst3Fixture fx;
    auto* view = fx.create_view();
    REQUIRE(g_vst3_root.root != nullptr);
    auto* host = g_vst3_root.root->plugin_view_host();
    REQUIRE(host != nullptr);

    // Before attach: the root's declared preferred size.
    Steinberg::ViewRect rect;
    REQUIRE(view->getSize(&rect) == Steinberg::kResultTrue);
    CHECK(rect.getWidth() == 400);
    CHECK(rect.getHeight() == 300);

    REQUIRE(view->attached(fx.native_parent(), Steinberg::kPlatformTypeNSView) ==
            Steinberg::kResultTrue);
    CHECK(g_vst3_root.opens == 1);
    CHECK(host->is_attached());

    // The declared hints drive resize negotiation: 4:3 inside [min, max].
    REQUIRE(view->canResize() == Steinberg::kResultTrue);
    Steinberg::ViewRect proposed(0, 0, 1000, 1000);
    REQUIRE(view->checkSizeConstraint(&proposed) == Steinberg::kResultTrue);
    CHECK(proposed.getWidth() == 800);
    CHECK(proposed.getHeight() == 600);
    Steinberg::ViewRect resized(0, 0, 640, 480);
    REQUIRE(view->onSize(&resized) == Steinberg::kResultTrue);
    REQUIRE(view->getSize(&rect) == Steinberg::kResultTrue);
    CHECK(rect.getWidth() == 640);
    CHECK(rect.getHeight() == 480);
    CHECK(host->get_size().width == 640);
    CHECK(host->get_size().height == 480);

    REQUIRE(view->removed() == Steinberg::kResultTrue);
    CHECK(g_vst3_root.closes == 1);
    CHECK_FALSE(host->is_attached());
    view->release();
}

TEST_CASE("A VST3 host's Space reaches a focused Vellum text field and nothing else",
          "[pulp-vellum][vst3][editor-host][keys]") {
#if !defined(__APPLE__)
    SKIP("Vellum's SDK ships its GPU plug-in view host on macOS only");
#endif
    Vst3Fixture fx;
    auto* view = fx.create_view();
    REQUIRE(view->attached(fx.native_parent(), Steinberg::kPlatformTypeNSView) ==
            Steinberg::kResultTrue);
    REQUIRE(g_vst3_root.field != nullptr);

    // Nothing focused: Space stays the host's transport key.
    CHECK(press(*view, u' ', Steinberg::KEY_SPACE) == Steinberg::kResultFalse);
    CHECK(g_vst3_root.field->get_text().empty());

    g_vst3_root.field->claim_input_focus();
    // A focused field takes Space as text, and the host does not also act on it.
    CHECK(press(*view, u' ', Steinberg::KEY_SPACE) == Steinberg::kResultTrue);
    CHECK(g_vst3_root.field->get_text() == " ");
    CHECK(press(*view, u' ', Steinberg::KEY_SPACE, Steinberg::kShiftKey) == Steinberg::kResultTrue);
    CHECK(g_vst3_root.field->get_text() == "  ");
    // The release, a Command chord and any other key stay with the host or
    // the native view's own key path.
    CHECK(view->onKeyUp(u' ', Steinberg::KEY_SPACE, 0) == Steinberg::kResultFalse);
    CHECK(press(*view, u' ', Steinberg::KEY_SPACE, Steinberg::kCommandKey) ==
          Steinberg::kResultFalse);
    CHECK(press(*view, u'a', 0) == Steinberg::kResultFalse);
    CHECK(g_vst3_root.field->get_text() == "  ");

    REQUIRE(view->removed() == Steinberg::kResultTrue);
    view->release();
}

TEST_CASE("A VST3 host gets no Vellum editor when the view host cannot be created",
          "[pulp-vellum][vst3][editor-host][negative]") {
    Vst3FactoryReset reset;
    PluginViewHost::set_factory(
        [](::vellum::view::View&,
           const PluginViewHost::Options&) -> std::unique_ptr<PluginViewHost> { return nullptr; });
    Vst3Fixture fx;
    auto* view = fx.create_view();
    int parent_stand_in = 0;
    CHECK(view->attached(&parent_stand_in, Steinberg::kPlatformTypeNSView) ==
          Steinberg::kResultFalse);
    CHECK(g_vst3_root.opens == 0);
    view->removed();
    CHECK(g_vst3_root.closes == 0);
    view->release();
}

TEST_CASE("A VST3 host whose parent refuses the Vellum view never opens the editor",
          "[pulp-vellum][vst3][editor-host][negative]") {
    Vst3FactoryReset reset;
    Vst3RefusingHost* made = nullptr;
    PluginViewHost::set_factory(
        [&made](::vellum::view::View&, const PluginViewHost::Options& options) {
            auto host = std::make_unique<Vst3RefusingHost>(options.size);
            made = host.get();
            return std::unique_ptr<PluginViewHost>(std::move(host));
        });
    Vst3Fixture fx;
    auto* view = fx.create_view();
    REQUIRE(made != nullptr);
    int parent_stand_in = 0;
    CHECK(view->attached(&parent_stand_in, Steinberg::kPlatformTypeNSView) ==
          Steinberg::kResultFalse);
    CHECK(g_vst3_root.opens == 0);
    // Nothing opened, so nothing closes; a later removed() is not a second close.
    view->removed();
    CHECK(g_vst3_root.closes == 0);
    view->release();
}

#endif // PULP_TEST_VELLUM_VST3

// AutoUi on Vellum views: the generated editor over a processor's parameters.
namespace {

struct AutoUiFixture {
    pulp::state::StateStore store;
    std::unique_ptr<pulp::format::StateStoreParamPort> port;
    std::unique_ptr<pulp::vellum::ParamPortModel> model;

    void add(pulp::state::ParamID id, std::string name, std::string unit,
             pulp::state::ParamRange range, int group = 0) {
        pulp::state::ParamInfo info;
        info.id = id;
        info.name = std::move(name);
        info.unit = std::move(unit);
        info.range = range;
        info.group_id = group;
        store.add_parameter(info);
    }
    // The port snapshots the parameter list, so it is made after they are.
    std::unique_ptr<::vellum::view::View> build() {
        port = std::make_unique<pulp::format::StateStoreParamPort>(store);
        model = std::make_unique<pulp::vellum::ParamPortModel>(*port);
        return pulp::vellum::AutoUi::build(*port, *model);
    }
};

template <typename Widget> Widget* auto_ui_widget(::vellum::view::View& view, std::string_view id) {
    if (view.id() == id)
        if (auto* widget = dynamic_cast<Widget*>(&view))
            return widget;
    for (std::size_t i = 0; i < view.child_count(); ++i)
        if (auto* widget = auto_ui_widget<Widget>(*view.child_at(i), id))
            return widget;
    return nullptr;
}

} // namespace

TEST_CASE("Vellum AutoUi builds a knob tile per parameter with its rows below the dial",
          "[pulp-vellum][auto-ui]") {
    using namespace ::vellum::view;
    AutoUiFixture fixture;
    fixture.add(1, "Gain", "dB", {-60.0f, 12.0f, 0.0f});
    fixture.add(2, "Mix", "%", {0.0f, 100.0f, 100.0f});
    fixture.add(3, "Bypass", "", {0.0f, 1.0f, 0.0f, 1.0f});
    auto root = fixture.build();
    REQUIRE(root);

    // root: [title, body]; body: [grid]; grid: one tile per parameter.
    REQUIRE(root->child_count() == 2);
    auto* body = root->child_at(1);
    REQUIRE(body->child_count() == 1);
    auto* grid = body->child_at(0);
    REQUIRE(grid->child_count() == 3);

    auto* gain_tile = grid->child_at(0);
    REQUIRE(gain_tile->child_count() == 3);
    auto* gain = dynamic_cast<Knob*>(gain_tile->child_at(0));
    REQUIRE(gain);
    CHECK_FALSE(gain->show_label());
    CHECK_FALSE(gain->show_value());
    auto* name = dynamic_cast<Label*>(gain_tile->child_at(1));
    auto* value = dynamic_cast<Label*>(gain_tile->child_at(2));
    REQUIRE(name);
    REQUIRE(value);
    CHECK(name->text() == "Gain");
    CHECK(value->text() == "0.00 dB");
    CHECK(auto_ui_widget<Toggle>(*root, "Bypass") != nullptr);
}

TEST_CASE("Vellum AutoUi opens large enough to fit every parameter",
          "[pulp-vellum][auto-ui][fit]") {
    using namespace ::vellum::view;
    for (std::size_t n :
         {std::size_t{1}, std::size_t{4}, std::size_t{7}, std::size_t{16}, std::size_t{32}}) {
        AutoUiFixture fixture;
        for (std::size_t i = 1; i <= n; ++i)
            fixture.add(static_cast<pulp::state::ParamID>(i), "P" + std::to_string(i), "",
                        {0.0f, 1.0f, 0.5f});
        auto root = fixture.build();
        const auto fit = pulp::vellum::AutoUi::preferred_size(*fixture.port);
        INFO("n=" << n << " fit=" << fit.width << "x" << fit.height);
        CHECK(fit.width >= 320);
        CHECK(fit.height >= 240);
        root->set_bounds(
            {0.0f, 0.0f, static_cast<float>(fit.width), static_cast<float>(fit.height)});
        root->layout_children();
        auto* body = dynamic_cast<ScrollView*>(root->child_at(1));
        REQUIRE(body);
        CHECK(body->content_size().height <= body->local_bounds().height + 1.0f);
        CHECK_FALSE(body->wants_wheel_scroll());
        CHECK(auto_ui_widget<Knob>(*root, "P" + std::to_string(n)) != nullptr);
    }
}

TEST_CASE("Vellum AutoUi shows registered groups as sections and keeps the rest in Other",
          "[pulp-vellum][auto-ui][groups]") {
    using namespace ::vellum::view;
    AutoUiFixture fixture;
    fixture.store.add_group({1, "Registered", 0});
    fixture.add(1, "Registered param", "", {0.0f, 1.0f, 0.5f}, 1);
    fixture.add(2, "Unknown group param", "", {0.0f, 1.0f, 0.5f}, 999);
    fixture.add(3, "Ungrouped param", "", {0.0f, 1.0f, 0.5f});
    auto root = fixture.build();
    root->set_bounds({0, 0, 400, 300});
    root->layout_children();

    auto* body = dynamic_cast<ScrollView*>(root->child_at(1));
    REQUIRE(body);
    REQUIRE(body->child_count() == 1);
    auto* content = body->child_at(0);
    REQUIRE(content->child_count() == 2);
    auto* registered = dynamic_cast<GroupBox*>(content->child_at(0));
    auto* other = dynamic_cast<GroupBox*>(content->child_at(1));
    REQUIRE(registered);
    REQUIRE(other);
    CHECK(registered->title() == "Registered");
    CHECK(other->title() == "Other");
    CHECK(auto_ui_widget<Knob>(*root, "Unknown group param") != nullptr);
    CHECK(auto_ui_widget<Knob>(*root, "Ungrouped param") != nullptr);
}

TEST_CASE("Vellum AutoUi controls write back through one host gesture each",
          "[pulp-vellum][auto-ui][gesture]") {
    using namespace ::vellum::view;
    AutoUiFixture fixture;
    fixture.add(1, "Frequency", "Hz", {55.0f, 1760.0f, 440.0f});
    fixture.add(2, "Bypass", "", {0.0f, 1.0f, 0.0f, 1.0f});
    std::vector<pulp::state::ParamID> begins;
    std::vector<pulp::state::ParamID> ends;
    fixture.store.set_gesture_callbacks([&](pulp::state::ParamID id) { begins.push_back(id); },
                                        [&](pulp::state::ParamID id) { ends.push_back(id); });
    auto root = fixture.build();
    auto* frequency = auto_ui_widget<Knob>(*root, "Frequency");
    auto* bypass = auto_ui_widget<Toggle>(*root, "Bypass");
    REQUIRE(frequency);
    REQUIRE(bypass);
    REQUIRE(frequency->on_gesture_begin);
    REQUIRE(frequency->on_gesture_end);

    frequency->on_gesture_begin();
    frequency->on_change(0.75f);
    frequency->on_change(0.80f);
    frequency->on_gesture_end();
    bypass->on_toggle(true);

    CHECK(fixture.store.get_normalized(1) == Catch::Approx(0.80f).margin(0.001f));
    CHECK(fixture.store.get_normalized(2) == Catch::Approx(1.0f).margin(0.001f));
    CHECK(begins == std::vector<pulp::state::ParamID>{1, 2});
    CHECK(ends == std::vector<pulp::state::ParamID>{1, 2});
    CHECK(fixture.store.open_gesture_count() == 0);
}

TEST_CASE("Vellum AutoUi knobs and value rows follow automation", "[pulp-vellum][auto-ui]") {
    using namespace ::vellum::view;
    AutoUiFixture fixture;
    fixture.add(7, "Cutoff", "Hz", {0.0f, 100.0f, 50.0f});
    auto root = fixture.build();
    auto* knob = auto_ui_widget<Knob>(*root, "Cutoff");
    REQUIRE(knob);
    std::vector<pulp::state::ParamID> begins;
    fixture.store.set_gesture_callbacks([&](pulp::state::ParamID id) { begins.push_back(id); },
                                        [](pulp::state::ParamID) {});

    fixture.store.set_value(7, 80.0f);
    fixture.store.pump_listeners();
    CHECK(knob->value() == Catch::Approx(0.8f).margin(1e-5f));
    auto* tile = knob->parent();
    REQUIRE(tile);
    auto* row = dynamic_cast<Label*>(tile->child_at(2));
    REQUIRE(row);
    CHECK(row->text() == "80.0 Hz");
    // Automation is not an edit: nothing opened a gesture.
    CHECK(begins.empty());
}

TEST_CASE("Vellum AutoUi editor opens at the fit size unless the processor declares one",
          "[pulp-vellum][auto-ui][editor]") {
    struct SizedProcessor final : public pulp::format::Processor {
        pulp::format::PluginDescriptor descriptor() const override {
            return {};
        }
        void define_parameters(pulp::state::StateStore&) override {}
        void prepare(const pulp::format::PrepareContext&) override {}
        void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                     pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                     const pulp::format::ProcessContext&) override {}
        std::pair<uint32_t, uint32_t> editor_size() const override {
            return declared;
        }
        std::pair<uint32_t, uint32_t> declared{400, 300};
    };
    FactoryReset reset;
    ::vellum::view::PluginViewHost::set_factory(
        [](::vellum::view::View&, const ::vellum::view::PluginViewHost::Options& options) {
            return std::make_unique<StubHost>(options.size);
        });
    AutoUiFixture fixture;
    for (pulp::state::ParamID id = 1; id <= 16; ++id)
        fixture.add(id, "P" + std::to_string(id), "", {0.0f, 1.0f, 0.5f});
    fixture.port = std::make_unique<pulp::format::StateStoreParamPort>(fixture.store);
    Context context(*fixture.port);
    const auto fit = pulp::vellum::AutoUi::preferred_size(*fixture.port);
    REQUIRE((fit.width != 400 || fit.height != 300));
    auto* factory = pulp::vellum::auto_ui_editor_factory();
    REQUIRE(factory);

    SizedProcessor by_default;
    auto fitted = factory->create(by_default, context, pulp::format::EditorRole::editor);
    REQUIRE(fitted);
    CHECK(fitted->size().width == fit.width);
    CHECK(fitted->size().height == fit.height);
    CHECK(fitted->size_hints().aspect_ratio > 0.0);

    SizedProcessor declared;
    declared.declared = {500, 260};
    auto kept = factory->create(declared, context, pulp::format::EditorRole::editor);
    REQUIRE(kept);
    CHECK(kept->size().width == 500);
    CHECK(kept->size().height == 260);
}
