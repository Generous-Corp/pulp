// test_native_surface_host.cpp — the dev.pulp.native_surface/1 editor-host
// extension: who offers it, what it reports, and how its observers behave.
//
// An editor that brings its own renderer reads this extension through the
// EditorContext it was created with. The contract session is the host every
// format adapter uses, so it is tested as the offering host; a context that
// overrides nothing is the null host.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/contract_editor_session.hpp>
#include <pulp/format/editor_host.hpp>
#include <pulp/format/native_surface_host.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#include <limits>
#include <memory>
#include <vector>

using namespace pulp::format;

namespace native_surface_host_test {

/// A host that offers nothing beyond the base contract.
class BareContext final : public EditorContext {
  public:
    explicit BareContext(ParamPort& port) : port_(port) {}
    ParamPort& params() override {
        return port_;
    }
    VisualPort* visuals() override {
        return nullptr;
    }
    bool request_resize(const EditorSize&) override {
        return false;
    }
    bool host_action(const HostActionRequest&) override {
        return false;
    }
    EditorHostKind host_kind() const override {
        return EditorHostKind::plugin;
    }

  private:
    ParamPort& port_;
};

/// Records the extension it sees at create and at attach, the way a
/// native-surface editor would look it up.
class ProbeEditor final : public Editor {
  public:
    explicit ProbeEditor(EditorContext& context) : context_(context) {
        at_create = native_surface_host(context_);
    }
    bool supports_parent(NativeParentKind kind) const override {
        return kind == NativeParentKind::cocoa_view;
    }
    bool attach(const NativeParent&) override {
        at_attach = native_surface_host(context_);
        if (at_attach)
            observation = at_attach->observe(
                [this](const NativeSurfaceHostInfo& info) { seen.push_back(info); });
        return true;
    }
    void detach() override {
        observation.reset();
    }
    void invalidate() override {
        observation.reset();
    }
    EditorSize size() const override {
        return size_;
    }
    bool set_size(const EditorSize& size) override {
        size_ = size;
        return true;
    }

    NativeSurfaceHost* at_create = nullptr;
    NativeSurfaceHost* at_attach = nullptr;
    NativeSurfaceObservation observation;
    std::vector<NativeSurfaceHostInfo> seen;

  private:
    EditorContext& context_;
    EditorSize size_{320, 200, 1.0};
};

class ProbeFactory final : public EditorFactory {
  public:
    bool supports(EditorRole role) const override {
        return role == EditorRole::editor;
    }
    std::unique_ptr<Editor> create(Processor&, EditorContext& context, EditorRole) override {
        auto editor = std::make_unique<ProbeEditor>(context);
        last = editor.get();
        return editor;
    }
    ProbeEditor* last = nullptr;
};

class SilentProcessor final : public Processor {
  public:
    PluginDescriptor descriptor() const override {
        return {};
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& out, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&, const ProcessContext&) override {
        out.clear();
    }
};

struct Session {
    pulp::state::StateStore store;
    SilentProcessor processor;
    ProbeFactory factory;
    std::unique_ptr<ContractEditorSession> session;
    int parent = 0;

    Session() {
        session = ContractEditorSession::create(processor, store, factory, {},
                                                ContractEditorSession::IdleSource::host_driven);
    }
    ProbeEditor& editor() {
        return *factory.last;
    }
    bool attach() {
        return session->attach({NativeParentKind::cocoa_view, &parent, 0});
    }
};

} // namespace native_surface_host_test

using namespace native_surface_host_test;

TEST_CASE("a host that does not offer the native-surface extension returns null",
          "[editor-host][native-surface]") {
    pulp::state::StateStore store;
    StateStoreParamPort port(store);
    BareContext context(port);
    REQUIRE(context.query_extension(native_surface_extension_id) == nullptr);
    REQUIRE(native_surface_host(context) == nullptr);
}

TEST_CASE("the extension id is versioned in the id itself", "[editor-host][native-surface]") {
    REQUIRE(native_surface_extension_id == "dev.pulp.native_surface/1");
}

TEST_CASE("a contract session offers the extension to its editor from create on",
          "[editor-host][native-surface]") {
    Session s;
    REQUIRE(s.session);
    auto& editor = s.editor();
    REQUIRE(editor.at_create != nullptr);
    // The editor and the adapter reach one object.
    REQUIRE(editor.at_create ==
            static_cast<NativeSurfaceHost*>(s.session->native_surface_host()));
    REQUIRE(s.attach());
    REQUIRE(editor.at_attach == editor.at_create);

    // Before the adapter has said anything, every field reads unknown.
    const auto initial = editor.at_create->info();
    REQUIRE(initial.generation == 0);
    REQUIRE(initial.display_id == 0);
    REQUIRE(initial.backing_scale == 0.0);
    REQUIRE(initial.refresh_hz == 0.0);
    REQUIRE(initial.visibility == NativeSurfaceVisibility::unknown);
    s.session->detach();
}

TEST_CASE("the contract session's other extension ids stay null",
          "[editor-host][native-surface]") {
    pulp::state::StateStore store;
    SilentProcessor processor;
    struct ContextGrab final : public EditorFactory {
        bool supports(EditorRole role) const override {
            return role == EditorRole::editor;
        }
        std::unique_ptr<Editor> create(Processor&, EditorContext& context, EditorRole) override {
            v1 = context.query_extension("dev.pulp.native_surface/1");
            v2 = context.query_extension("dev.pulp.native_surface/2");
            near_miss = context.query_extension("dev.pulp.native_surface");
            other = context.query_extension("dev.pulp.test.example/1");
            return std::make_unique<ProbeEditor>(context);
        }
        void* v1 = nullptr;
        void* v2 = reinterpret_cast<void*>(1);
        void* near_miss = reinterpret_cast<void*>(1);
        void* other = reinterpret_cast<void*>(1);
    } factory;
    auto session = ContractEditorSession::create(processor, store, factory, {},
                                                 ContractEditorSession::IdleSource::host_driven);
    REQUIRE(session);
    REQUIRE(factory.v1 != nullptr);
    REQUIRE(factory.v2 == nullptr);
    REQUIRE(factory.near_miss == nullptr);
    REQUIRE(factory.other == nullptr);
}

TEST_CASE("the host's scale and visibility reach the extension, once per change",
          "[editor-host][native-surface]") {
    Session s;
    REQUIRE(s.session);
    REQUIRE(s.attach());
    auto& editor = s.editor();
    auto* host = editor.at_attach;
    REQUIRE(host != nullptr);

    REQUIRE(s.session->set_scale(2.0));
    REQUIRE(host->info().backing_scale == 2.0);
    REQUIRE(editor.seen.size() == 1);
    REQUIRE(editor.seen.back().generation == 1);

    // The same scale again is not a change.
    REQUIRE(s.session->set_scale(2.0));
    REQUIRE(editor.seen.size() == 1);

    s.session->visibility_changed(false);
    REQUIRE(host->info().visibility == NativeSurfaceVisibility::hidden);
    s.session->visibility_changed(true);
    REQUIRE(host->info().visibility == NativeSurfaceVisibility::visible);
    REQUIRE(editor.seen.size() == 3);
    REQUIRE(editor.seen.back().generation == 3);
    REQUIRE(editor.seen.back().backing_scale == 2.0);

    // An adapter that knows the display publishes it through the session.
    s.session->native_surface_host()->set_display(69734662u, 120.0);
    REQUIRE(editor.seen.size() == 4);
    REQUIRE(editor.seen.back().display_id == 69734662u);
    REQUIRE(editor.seen.back().refresh_hz == 120.0);

    // After detach the editor's observation is gone and changes reach nobody.
    s.session->detach();
    REQUIRE(s.session->native_surface_host()->observer_count() == 0);
    REQUIRE(s.session->set_scale(1.0));
    REQUIRE(editor.seen.size() == 4);
}

TEST_CASE("a refused scale is still the host's scale", "[editor-host][native-surface]") {
    pulp::state::StateStore store;
    SilentProcessor processor;
    struct Refusing final : public EditorFactory {
        bool supports(EditorRole role) const override {
            return role == EditorRole::editor;
        }
        std::unique_ptr<Editor> create(Processor&, EditorContext& context, EditorRole) override {
            struct E final : public Editor {
                bool supports_parent(NativeParentKind) const override {
                    return true;
                }
                bool attach(const NativeParent&) override {
                    return true;
                }
                void detach() override {}
                void invalidate() override {}
                EditorSize size() const override {
                    return {100, 100, 1.0};
                }
                bool set_size(const EditorSize&) override {
                    return false;
                }
            };
            host = native_surface_host(context);
            return std::make_unique<E>();
        }
        NativeSurfaceHost* host = nullptr;
    } factory;
    auto session = ContractEditorSession::create(processor, store, factory, {},
                                                 ContractEditorSession::IdleSource::host_driven);
    REQUIRE(session);
    REQUIRE(factory.host != nullptr);
    REQUIRE_FALSE(session->set_scale(1.5));
    REQUIRE(factory.host->info().backing_scale == 1.5);
}

TEST_CASE("a closed or invalidated session no longer offers its state",
          "[editor-host][native-surface]") {
    {
        Session s;
        REQUIRE(s.attach());
        s.session->invalidate();
        REQUIRE(s.session->native_surface_host() == nullptr);
    }
    {
        Session s;
        REQUIRE(s.attach());
        s.session->close_for_owner();
        REQUIRE(s.session->native_surface_host() == nullptr);
    }
}

TEST_CASE("NativeSurfaceHostState reads bad values as unknown", "[editor-host][native-surface]") {
    NativeSurfaceHostState state;
    state.set_backing_scale(2.0);
    state.set_backing_scale(std::numeric_limits<double>::quiet_NaN());
    REQUIRE(state.info().backing_scale == 0.0);
    state.set_backing_scale(-1.0);
    REQUIRE(state.info().backing_scale == 0.0);
    state.set_backing_scale(std::numeric_limits<double>::infinity());
    REQUIRE(state.info().backing_scale == 0.0);
    state.set_display(7u, -60.0);
    REQUIRE(state.info().display_id == 7u);
    REQUIRE(state.info().refresh_hz == 0.0);
    // 2.0, then unknown, then display 7: three changes.
    REQUIRE(state.info().generation == 3);
}

TEST_CASE("NativeSurfaceHostState observers may change the observer set while called",
          "[editor-host][native-surface]") {
    NativeSurfaceHostState state;
    int first = 0;
    int second = 0;
    int late = 0;
    NativeSurfaceObservation a;
    NativeSurfaceObservation b;
    NativeSurfaceObservation c;
    a = state.observe([&](const NativeSurfaceHostInfo&) {
        ++first;
        // Cancel itself and the next observer, and add one, mid-publish.
        a.reset();
        b.reset();
        c = state.observe([&](const NativeSurfaceHostInfo&) { ++late; });
    });
    b = state.observe([&](const NativeSurfaceHostInfo&) { ++second; });
    REQUIRE(state.observer_count() == 2);

    state.set_visibility(NativeSurfaceVisibility::visible);
    REQUIRE(first == 1);
    REQUIRE(second == 0); // cancelled before its turn
    REQUIRE(late == 0);   // added during the publish, so not called for it
    REQUIRE(state.observer_count() == 1);

    state.set_visibility(NativeSurfaceVisibility::hidden);
    REQUIRE(first == 1);
    REQUIRE(late == 1);
    c.reset();
    REQUIRE(state.observer_count() == 0);
}

TEST_CASE("an empty observer registers nothing", "[editor-host][native-surface]") {
    NativeSurfaceHostState state;
    auto observation = state.observe({});
    REQUIRE_FALSE(observation);
    REQUIRE(state.observer_count() == 0);
}

TEST_CASE("an observation that outlives its host state is inert",
          "[editor-host][native-surface]") {
    NativeSurfaceObservation observation;
    {
        NativeSurfaceHostState state;
        observation = state.observe([](const NativeSurfaceHostInfo&) {});
        REQUIRE(observation);
    }
    // Cancelling must not reach the destroyed state (ASan holds this).
    observation.reset();
    REQUIRE_FALSE(observation);
}
