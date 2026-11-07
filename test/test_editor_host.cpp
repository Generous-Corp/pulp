// test_editor_host.cpp — the editor-host contract and its default ParamPort.
//
// The ParamPort tests drive StateStoreParamPort with HostParameterEditBridge
// installed exactly as a format adapter installs it, and assert the stream the
// host would receive: one begin, the values in order, the final value, one
// end. Ordering is what hosts latch on for touch automation and undo grouping,
// so the stream is compared whole rather than counted.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/contract_editor_session.hpp>
#include <pulp/format/editor_host.hpp>
#include <pulp/format/editor_package.hpp>
#include <pulp/format/host_parameter_edit.hpp>
#include <pulp/format/native_surface_host.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <thread>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace pulp::format;
using pulp::state::ParamID;
using pulp::state::StateStore;

namespace {

constexpr ParamID kGain = 1;
constexpr ParamID kMix = 2;

void add_unit_param(StateStore& store, ParamID id, const char* name) {
    pulp::state::ParamInfo info;
    info.id = id;
    info.name = name;
    info.range = {0.0f, 1.0f, 0.5f};
    store.add_parameter(info);
}

/// The host side of the gesture path, rendered as "B1 V1(0.75) E1".
struct HostLog {
    std::vector<std::string> entries;

    HostParameterEditBridge::Callbacks callbacks() {
        return {
            [this](ParamID id) { entries.push_back("B" + std::to_string(id)); },
            [this](ParamID id, float normalized, float) {
                entries.push_back("V" + std::to_string(id) + "(" + value(normalized) + ")");
            },
            [this](ParamID id) { entries.push_back("E" + std::to_string(id)); },
        };
    }

    static std::string value(float v) {
        // Two decimals are enough to tell every value these tests write apart.
        const int hundredths = static_cast<int>(v * 100.0f + 0.5f);
        std::string s = std::to_string(hundredths / 100) + ".";
        const int frac = hundredths % 100;
        if (frac < 10)
            s += '0';
        return s + std::to_string(frac);
    }

    std::string stream() const {
        std::string s;
        for (const auto& e : entries) {
            if (!s.empty())
                s += ' ';
            s += e;
        }
        return s;
    }
};

struct Fixture {
    StateStore store;
    HostLog host;
    std::unique_ptr<HostParameterEditBridge> bridge;

    Fixture() {
        add_unit_param(store, kGain, "Gain");
        add_unit_param(store, kMix, "Mix");
        bridge = std::make_unique<HostParameterEditBridge>(store, host.callbacks());
    }
};

} // namespace

TEST_CASE("a ParamPort gesture reaches the host as begin, values, end in order",
          "[editor-host][param-port]") {
    Fixture f;
    StateStoreParamPort port(f.store);

    port.begin(kGain);
    port.set(kGain, 0.5f);
    port.set(kGain, 0.75f);
    port.end(kGain);

    // The final value precedes the end, so a host that latches on end records
    // where the control landed.
    REQUIRE(f.host.stream() == "B1 V1(0.50) V1(0.75) V1(0.75) E1");
    REQUIRE(f.store.get_value(kGain) == 0.75f);
    REQUIRE(f.store.open_gesture_count() == 0);
    REQUIRE(port.open_gestures(kGain) == 0);
}

TEST_CASE("a ParamPort write outside a gesture is a bracketed one-shot edit",
          "[editor-host][param-port]") {
    Fixture f;
    StateStoreParamPort port(f.store);

    port.set(kMix, 0.25f);

    REQUIRE(f.host.stream() == "B2 V2(0.25) V2(0.25) E2");
    REQUIRE(f.store.open_gesture_count() == 0);
}

TEST_CASE("a ParamPort ignores unbalanced ends and unknown parameters",
          "[editor-host][param-port]") {
    Fixture f;
    StateStoreParamPort port(f.store);

    port.end(kGain);
    port.begin(999);
    port.set(999, 0.5f);
    port.end(999);

    REQUIRE(f.host.entries.empty());
    REQUIRE(port.info(999) == nullptr);
    REQUIRE(port.get_normalized(999) == 0.0f);
}

TEST_CASE("a ParamPort exposes parameter info and normalized access", "[editor-host][param-port]") {
    Fixture f;
    StateStoreParamPort port(f.store);

    REQUIRE(port.params().size() == 2);
    REQUIRE(port.params()[0].id == kGain);
    REQUIRE(port.info(kMix) != nullptr);
    REQUIRE(port.info(kMix)->name == "Mix");

    port.set_normalized(kGain, 0.4f);
    REQUIRE(port.get(kGain) == f.store.get_value(kGain));
    REQUIRE(port.get_normalized(kGain) == f.store.get_normalized(kGain));
    REQUIRE(f.host.stream() == "B1 V1(0.40) V1(0.40) E1");
}

TEST_CASE("ParamPort listeners see every writer and stop when unsubscribed",
          "[editor-host][param-port]") {
    Fixture f;
    StateStoreParamPort port(f.store);
    std::vector<std::pair<ParamID, float>> seen;
    auto subscription =
        port.add_listener([&seen](ParamID id, float plain) { seen.emplace_back(id, plain); });
    REQUIRE(subscription);

    port.set(kGain, 0.3f);
    {
        // Host automation is delivered to the editor, and is not reported back
        // to the host as an edit.
        const ScopedHostParameterWrite host_write;
        f.store.set_value(kMix, 0.6f);
    }

    REQUIRE(seen.size() == 2);
    REQUIRE(seen[0] == std::make_pair(kGain, 0.3f));
    REQUIRE(seen[1] == std::make_pair(kMix, 0.6f));
    REQUIRE(f.host.stream() == "B1 V1(0.30) V1(0.30) E1");

    subscription.reset();
    REQUIRE_FALSE(subscription);
    port.set(kGain, 0.9f);
    REQUIRE(seen.size() == 2);
}

TEST_CASE("two editors sharing one ParamPort open one host gesture",
          "[editor-host][param-port][multi-editor]") {
    Fixture f;
    StateStoreParamPort shared(f.store);
    std::vector<float> editor_a_saw;
    std::vector<float> editor_b_saw;
    auto a = shared.add_listener([&](ParamID, float v) { editor_a_saw.push_back(v); });
    auto b = shared.add_listener([&](ParamID, float v) { editor_b_saw.push_back(v); });

    shared.begin(kGain); // editor A grabs the knob
    shared.begin(kGain); // editor B grabs the same parameter's control
    shared.set(kGain, 0.2f);
    shared.set(kGain, 0.8f);
    shared.end(kGain); // A lets go: B still holds it, so no host end yet
    REQUIRE(f.host.stream() == "B1 V1(0.20) V1(0.80)");
    shared.end(kGain); // B lets go

    REQUIRE(f.host.stream() == "B1 V1(0.20) V1(0.80) V1(0.80) E1");
    // Each editor saw the other's writes: the round trip between windows.
    REQUIRE(editor_a_saw == std::vector<float>{0.2f, 0.8f});
    REQUIRE(editor_b_saw == editor_a_saw);
}

TEST_CASE("closing one editor's port mid-gesture keeps another editor's gesture open",
          "[editor-host][param-port][multi-editor]") {
    Fixture f;
    StateStoreParamPort editor_b(f.store);
    {
        StateStoreParamPort editor_a(f.store);
        editor_a.begin(kGain);
        editor_b.begin(kGain);
        editor_a.set(kGain, 0.6f);
        // editor_a's window closes without lifting the mouse.
    }
    REQUIRE(f.host.stream() == "B1 V1(0.60)");
    REQUIRE(f.store.open_gesture_count() == 1);

    editor_b.release_all();
    REQUIRE(f.host.stream() == "B1 V1(0.60) V1(0.60) E1");
    REQUIRE(f.store.open_gesture_count() == 0);
}

TEST_CASE("destroying the last port mid-gesture releases its gestures",
          "[editor-host][param-port]") {
    Fixture f;
    {
        StateStoreParamPort port(f.store);
        port.begin(kGain);
        port.begin(kMix);
        port.set(kMix, 0.1f);
    }
    REQUIRE(f.store.open_gesture_count() == 0);
    REQUIRE(f.host.entries.size() == 7);
    REQUIRE(f.host.entries.front().front() == 'B');
    REQUIRE(f.host.entries.back().front() == 'E');
}

TEST_CASE("a disconnected ParamPort releases its gestures and refuses every later call",
          "[editor-host][param-port]") {
    Fixture f;
    f.store.set_value(kGain, 0.25f);
    StateStoreParamPort port(f.store);
    int heard = 0;
    auto subscription = port.add_listener([&heard](ParamID, float) { ++heard; });
    port.begin(kGain);
    REQUIRE(f.store.open_gesture_count() == 1);
    REQUIRE(port.connected());

    port.disconnect();
    REQUIRE_FALSE(port.connected());
    // Every gesture it held is closed, and the host saw it end.
    REQUIRE(f.store.open_gesture_count() == 0);
    REQUIRE(f.host.entries.back() == "E1");
    const auto host_entries = f.host.entries.size();

    // Nothing reads or writes the store any more. The parameter list stays
    // readable: it is the port's own.
    REQUIRE(port.params().size() == 2);
    REQUIRE(port.info(kGain) != nullptr);
    REQUIRE(port.get(kGain) == 0.0f);
    port.begin(kGain);
    port.set(kGain, 0.9f);
    port.end(kGain);
    REQUIRE(f.store.open_gesture_count() == 0);
    REQUIRE(f.store.get_value(kGain) == 0.25f);
    REQUIRE(port.open_gestures(kGain) == 0);
    REQUIRE(f.host.entries.size() == host_entries);
    REQUIRE_FALSE(port.add_listener([](ParamID, float) {}));

    // Its listener was removed with it: a write by another writer is unheard.
    f.store.set_value(kMix, 0.6f);
    REQUIRE(heard == 0);

    port.disconnect(); // idempotent
    subscription.reset();
}

TEST_CASE("a ParamPort's parameter list is its own and outlives the store",
          "[editor-host][param-port]") {
    auto store = std::make_unique<StateStore>();
    add_unit_param(*store, kGain, "Gain");
    add_unit_param(*store, kMix, "Mix");
    StateStoreParamPort port(*store);

    // An editor builds its controls from these and keeps them.
    const auto* gain = port.info(kGain);
    const auto params = port.params();
    REQUIRE(gain != nullptr);
    REQUIRE(params.size() == 2);
    // They are the port's copy, never pointers into the store, so destroying
    // the store cannot leave them dangling.
    REQUIRE(gain != store->info(kGain));
    REQUIRE(params.data() != store->all_params().data());

    port.disconnect();
    store.reset();
    REQUIRE(gain->id == kGain);
    REQUIRE(gain->name == "Gain");
    REQUIRE(params[1].name == "Mix");
    REQUIRE(port.info(kMix) == &params[1]);
}

namespace pulp::format::detail {
// Defined in editor_host.cpp: listener registrations reset so far.
std::uint64_t param_port_listener_resets();
} // namespace pulp::format::detail

TEST_CASE("each ParamPort listener registration is reset exactly once",
          "[editor-host][param-port]") {
    using pulp::format::detail::param_port_listener_resets;
    Fixture f;
    SECTION("disconnect, then the subscription") {
        StateStoreParamPort port(f.store);
        auto subscription = port.add_listener([](ParamID, float) {});
        const auto before = param_port_listener_resets();
        port.disconnect();
        subscription.reset();
        REQUIRE(param_port_listener_resets() - before == 1);
    }
    SECTION("the subscription, then disconnect") {
        StateStoreParamPort port(f.store);
        auto subscription = port.add_listener([](ParamID, float) {});
        const auto before = param_port_listener_resets();
        subscription.reset();
        port.disconnect();
        REQUIRE(param_port_listener_resets() - before == 1);
    }
    SECTION("both at once, on two threads") {
        constexpr int kRounds = 500;
        const auto before = param_port_listener_resets();
        for (int round = 0; round < kRounds; ++round) {
            StateStoreParamPort port(f.store);
            auto subscription = port.add_listener([](ParamID, float) {});
            std::thread disconnector([&port] { port.disconnect(); });
            subscription.reset();
            disconnector.join();
        }
        REQUIRE(param_port_listener_resets() - before == kRounds);
    }
}

TEST_CASE("a disconnected ParamPort outlives its store", "[editor-host][param-port]") {
    auto store = std::make_unique<StateStore>();
    add_unit_param(*store, kGain, "Gain");
    StateStoreParamPort port(*store);
    auto subscription = port.add_listener([](ParamID, float) {});
    port.begin(kGain);
    port.disconnect();
    store.reset();

    // The store is gone; the port and its subscription touch nothing of it.
    REQUIRE(port.get(kGain) == 0.0f);
    port.begin(kGain);
    port.set(kGain, 0.5f);
    port.end(kGain);
    port.release_all();
    subscription.reset();
    REQUIRE(port.open_gestures(kGain) == 0);
}

TEST_CASE("ParamSubscription cancels exactly once and moves its ownership", "[editor-host]") {
    int cancelled = 0;
    {
        ParamSubscription first([&cancelled] { ++cancelled; });
        ParamSubscription second = std::move(first);
        REQUIRE_FALSE(first);
        REQUIRE(second);
        first.reset();
        REQUIRE(cancelled == 0);
        ParamSubscription third;
        third = std::move(second);
        REQUIRE(cancelled == 0);
    }
    REQUIRE(cancelled == 1);
}

namespace {

/// The smallest editor: records what the host did to it.
class RecordingEditor final : public Editor {
  public:
    RecordingEditor(EditorContext& context, EditorRole role) : context_(context), role_(role) {}

    bool supports_parent(NativeParentKind kind) const override {
        return kind == NativeParentKind::none;
    }
    bool attach(const NativeParent& parent) override {
        if (!supports_parent(parent.kind))
            return false;
        attached_ = true;
        return true;
    }
    void detach() override {
        attached_ = false;
    }
    void invalidate() override {
        attached_ = false;
        invalidated_ = true;
    }
    EditorSize size() const override {
        return size_;
    }
    bool set_size(const EditorSize& size) override {
        size_ = size;
        return true;
    }

    EditorContext& context() {
        return context_;
    }
    EditorRole role() const {
        return role_;
    }
    bool attached() const {
        return attached_;
    }
    bool invalidated() const {
        return invalidated_;
    }

  private:
    EditorContext& context_;
    EditorRole role_;
    EditorSize size_{400, 300, 1.0};
    bool attached_ = false;
    bool invalidated_ = false;
};

class RecordingFactory final : public EditorFactory {
  public:
    bool supports(EditorRole role) const override {
        return role != EditorRole::settings;
    }
    std::unique_ptr<Editor> create(Processor&, EditorContext& context, EditorRole role) override {
        if (!supports(role))
            return nullptr;
        return std::make_unique<RecordingEditor>(context, role);
    }
};

class TestContext final : public EditorContext {
  public:
    explicit TestContext(ParamPort& port) : port_(port) {}
    ParamPort& params() override {
        return port_;
    }
    VisualPort* visuals() override {
        return nullptr;
    }
    bool request_resize(const EditorSize& size) override {
        requested_ = size;
        return true;
    }
    bool host_action(const HostActionRequest& request) override {
        return request.action == HostAction::mark_state_dirty;
    }
    EditorHostKind host_kind() const override {
        return EditorHostKind::plugin;
    }

    EditorSize requested_{};

  private:
    ParamPort& port_;
};

/// A host that offers one extension beyond the base contract.
struct ExampleExtension {
    int answer = 42;
};

class ExtendedContext final : public EditorContext {
  public:
    explicit ExtendedContext(ParamPort& port) : port_(port) {}
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
        return EditorHostKind::standalone;
    }
    void* query_extension(std::string_view id) override {
        return id == "dev.pulp.test.example/1" ? &extension_ : nullptr;
    }

  private:
    ParamPort& port_;
    ExampleExtension extension_;
};

class GainProcessor : public Processor {
  public:
    PluginDescriptor descriptor() const override {
        return {};
    }
    void define_parameters(StateStore& store) override {
        add_unit_param(store, kGain, "Gain");
    }
    void prepare(const PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& out, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&, const ProcessContext&) override {
        out.clear();
    }
};

class GainProcessorWithEditor final : public GainProcessor {
  public:
    EditorFactory* editor_factory() override {
        return &factory_;
    }

  private:
    RecordingFactory factory_;
};

} // namespace

TEST_CASE("a processor has no editor factory unless it provides one", "[editor-host]") {
    GainProcessor headless;
    REQUIRE(headless.editor_factory() == nullptr);

    GainProcessorWithEditor with_editor;
    REQUIRE(with_editor.editor_factory() != nullptr);
}

namespace {

RecordingFactory& package_factory() {
    static RecordingFactory factory;
    return factory;
}

EditorFactory* package_hook() {
    return &package_factory();
}

} // namespace

TEST_CASE("a processor without its own factory returns its binary's UI package factory",
          "[editor-host][editor-package]") {
    struct ClearHook {
        ~ClearHook() {
            install_plugin_editor_factory_hook(nullptr);
        }
    } clear_hook;

    GainProcessor processor;
    REQUIRE(processor.editor_factory() == nullptr);

    // What the source pulp_add_plugin(... UI <package>) generates does at
    // static init.
    install_plugin_editor_factory_hook(&package_hook);
    REQUIRE(plugin_editor_factory() == &package_factory());
    REQUIRE(processor.editor_factory() == &package_factory());

    // A processor that supplies its own factory keeps it.
    GainProcessorWithEditor with_editor;
    REQUIRE(with_editor.editor_factory() != nullptr);
    REQUIRE(with_editor.editor_factory() != &package_factory());

    install_plugin_editor_factory_hook(nullptr);
    REQUIRE(processor.editor_factory() == nullptr);
}

TEST_CASE("an editor and an inspector share one port through the contract",
          "[editor-host][multi-editor]") {
    Fixture f;
    GainProcessorWithEditor processor;
    StateStoreParamPort port(f.store);
    TestContext context(port);

    auto* factory = processor.editor_factory();
    REQUIRE(factory->supports(EditorRole::editor));
    REQUIRE_FALSE(factory->supports(EditorRole::settings));
    REQUIRE(factory->create(processor, context, EditorRole::settings) == nullptr);

    factory->prewarm(); // the default has nothing to warm
    auto editor = factory->create(processor, context, EditorRole::editor);
    auto inspector = factory->create(processor, context, EditorRole::inspector);
    REQUIRE(editor);
    REQUIRE(inspector);
    // Hosts may negotiate scale and size before handing over a window.
    REQUIRE(editor->set_size({400, 300, 2.0}));
    REQUIRE(editor->attach(NativeParent{}));
    REQUIRE(inspector->attach(NativeParent{}));
    REQUIRE_FALSE(editor->attach(NativeParent{NativeParentKind::cocoa_view, nullptr, 0}));

    float inspector_saw = -1.0f;
    auto seen =
        context.params().add_listener([&inspector_saw](ParamID, float v) { inspector_saw = v; });

    // The editor drags gain; the inspector observes it through the same port.
    auto& editor_params = static_cast<RecordingEditor&>(*editor).context().params();
    editor_params.begin(kGain);
    editor_params.set(kGain, 0.75f);
    editor_params.end(kGain);
    REQUIRE(inspector_saw == 0.75f);
    REQUIRE(f.host.stream() == "B1 V1(0.75) V1(0.75) E1");

    // Defaults an editor need not override.
    REQUIRE(editor->offer_key(EditorKeyEvent{EditorKey::space, U' ', 0, true, false}) ==
            KeyDisposition::forward);
    REQUIRE(editor->automation() == nullptr);
    REQUIRE(editor->adjust_size({500, 200, 2.0}).width == 500);
    REQUIRE(editor->size_hints().preferred_width == 400);
    REQUIRE(editor->set_size({640, 480, 2.0}));
    REQUIRE(editor->size().scale == 2.0);

    REQUIRE(context.request_resize({320, 240, 1.0}));
    REQUIRE(context.requested_.width == 320);
    REQUIRE(context.host_action({HostAction::mark_state_dirty}));
    REQUIRE_FALSE(context.host_action({HostAction::close_editor}));

    editor->detach();
    REQUIRE_FALSE(static_cast<RecordingEditor&>(*editor).attached());
    inspector->invalidate();
    REQUIRE(static_cast<RecordingEditor&>(*inspector).invalidated());
}

TEST_CASE("the default automation provider reports every request as unsupported", "[editor-host]") {
    EditorAutomation automation;
    std::vector<std::uint8_t> png;
    std::string error;
    std::string reply;
    REQUIRE_FALSE(automation.capture_png(png));
    REQUIRE_FALSE(automation.inject_input(R"({"type":"click"})", error));
    REQUIRE_FALSE(error.empty());
    REQUIRE_FALSE(automation.observe("{}", reply));
}

namespace {

class TwoChannelVisuals final : public VisualPort {
  public:
    std::size_t channel_count() const noexcept override {
        return 2;
    }
    VisualChannelInfo channel(std::size_t index) const noexcept override {
        return index == 0 ? VisualChannelInfo{"gain", "dB", VisualChannelShape::scalar, 0.0f}
                          : VisualChannelInfo{"out", "", VisualChannelShape::meter, 0.0f};
    }
    bool read_scalar(std::size_t index, float& value) noexcept override {
        if (index != 0)
            return false;
        value = -6.0f;
        return true;
    }
    std::size_t read_meter(std::size_t, std::span<float>, std::span<float>) noexcept override {
        return 0;
    }
    std::size_t read_vector(std::size_t, std::span<float>) noexcept override {
        return 0;
    }
    std::size_t read_events(std::size_t, std::span<VisualEvent>) noexcept override {
        return 0;
    }
    std::uint32_t publish_count(std::size_t) const noexcept override {
        return 0;
    }
};

} // namespace

TEST_CASE("VisualPort::find matches a channel by name and shape", "[editor-host]") {
    TwoChannelVisuals visuals;
    REQUIRE(visuals.find("gain", VisualChannelShape::scalar) == std::size_t{0});
    REQUIRE(visuals.find("out", VisualChannelShape::meter) == std::size_t{1});
    REQUIRE_FALSE(visuals.find("gain", VisualChannelShape::meter));
    REQUIRE_FALSE(visuals.find("missing", VisualChannelShape::scalar));
}

TEST_CASE("query_extension is null by default and reaches a host's extension", "[editor-host]") {
    Fixture f;
    StateStoreParamPort port(f.store);

    TestContext plain(port);
    EditorContext& base = plain;
    REQUIRE(base.query_extension("dev.pulp.test.example/1") == nullptr);

    ExtendedContext extended(port);
    EditorContext& through_base = extended;
    auto* ext =
        static_cast<ExampleExtension*>(through_base.query_extension("dev.pulp.test.example/1"));
    REQUIRE(ext != nullptr);
    REQUIRE(ext->answer == 42);
    REQUIRE(through_base.query_extension("dev.pulp.test.example/2") == nullptr);
}

namespace {

/// Counts idle ticks; attaches to any non-empty Cocoa parent.
class TickingEditor final : public Editor {
  public:
    explicit TickingEditor(int& idles) : idles_(idles) {}
    bool supports_parent(NativeParentKind kind) const override {
        return kind == NativeParentKind::cocoa_view;
    }
    bool attach(const NativeParent&) override {
        return true;
    }
    void detach() override {}
    void invalidate() override {}
    EditorSize size() const override {
        return {400, 300, 1.0};
    }
    bool set_size(const EditorSize&) override {
        return true;
    }
    void idle() override {
        ++idles_;
    }

  private:
    int& idles_;
};

class TickingFactory final : public EditorFactory {
  public:
    explicit TickingFactory(int& idles) : idles_(idles) {}
    bool supports(EditorRole role) const override {
        return role == EditorRole::editor;
    }
    std::unique_ptr<Editor> create(Processor&, EditorContext&, EditorRole) override {
        return std::make_unique<TickingEditor>(idles_);
    }

  private:
    int& idles_;
};

} // namespace

TEST_CASE("a contract session whose owner is gone draws no more frames", "[editor-host]") {
    Fixture f;
    GainProcessor processor;
    int idles = 0;
    TickingFactory factory(idles);
    auto session = ContractEditorSession::create(processor, f.store, factory, {},
                                                 ContractEditorSession::IdleSource::host_driven);
    REQUIRE(session);
    int parent = 0;
    REQUIRE(session->attach({NativeParentKind::cocoa_view, &parent, 0}));
    session->tick();
    REQUIRE(idles == 1);

    session->disconnect_owner();
    session->tick();
    session->tick();
    REQUIRE(idles == 1);
    session->close_for_owner();
}

// ── dev.pulp.native_surface/1 ────────────────────────────────────────────────
//
// An editor that brings its own renderer reads this extension through the
// EditorContext it was created with. The contract session is the host every
// format adapter uses, so it is tested as the offering host; a context that
// overrides nothing is the null host.

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
    REQUIRE(editor.at_create == static_cast<NativeSurfaceHost*>(s.session->native_surface_host()));
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

TEST_CASE("the contract session's other extension ids stay null", "[editor-host][native-surface]") {
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

TEST_CASE("an observation that outlives its host state is inert", "[editor-host][native-surface]") {
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
