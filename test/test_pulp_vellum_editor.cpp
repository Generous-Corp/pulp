#include <catch2/catch_test_macros.hpp>

#include <pulp/format/editor_host.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>
#include <pulp/vellum/editor.hpp>

#include <vellum/value/param_model.hpp>
#include <vellum/view/plugin_view_host.hpp>
#include <vellum/view/view.hpp>

#include <memory>

namespace {

class Context final : public pulp::format::EditorContext {
  public:
    explicit Context(pulp::format::ParamPort& params) : params_(params) {}
    pulp::format::ParamPort& params() override {
        return params_;
    }
    pulp::format::VisualPort* visuals() override {
        return nullptr;
    }
    bool request_resize(const pulp::format::EditorSize&) override {
        return false;
    }
    bool host_action(const pulp::format::HostActionRequest&) override {
        return false;
    }
    pulp::format::EditorHostKind host_kind() const override {
        return pulp::format::EditorHostKind::plugin;
    }

  private:
    pulp::format::ParamPort& params_;
};

class Processor final : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {};
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}
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
    ::vellum::view::NativeViewHandle native_handle() override {
        return this;
    }
    void attach_to_parent(::vellum::view::NativeViewHandle) override {}
    void detach() override {}
    void repaint() override {}
    void set_size(uint32_t width, uint32_t height) override {
        size_ = {width, height};
    }
    Size get_size() const override {
        return size_;
    }

  private:
    Size size_;
};

// Clears any factory a test registers, even when an assertion stops it early.
struct FactoryReset {
    ~FactoryReset() {
        ::vellum::view::PluginViewHost::clear_factory();
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
    if (!pulp::vellum::detail::platform_plugin_view_host_available())
        SKIP("no platform plug-in view host on this OS");
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
