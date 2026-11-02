// test_editor_host.cpp — the editor-host contract and its default ParamPort.
//
// The ParamPort tests drive StateStoreParamPort with HostParameterEditBridge
// installed exactly as a format adapter installs it, and assert the stream the
// host would receive: one begin, the values in order, the final value, one
// end. Ordering is what hosts latch on for touch automation and undo grouping,
// so the stream is compared whole rather than counted.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/editor_host.hpp>
#include <pulp/format/host_parameter_edit.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#include <memory>
#include <string>
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
