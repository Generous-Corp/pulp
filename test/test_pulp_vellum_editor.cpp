#include <catch2/catch_test_macros.hpp>

#include <pulp/format/editor_host.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>
#include <pulp/vellum/editor.hpp>

#include <vellum/value/param_model.hpp>
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
