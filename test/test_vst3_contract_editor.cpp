// test_vst3_contract_editor.cpp — the VST3 adapter opening a processor's
// editor through the editor-host contract.
//
// A processor that returns an editor factory gets a ContractPlugView from
// createView(); a processor without one keeps the create_view() path
// (make_plug_view(), defined below as a marker). The editor here records every
// contract call, so each test asserts what the host's IPlugView calls became,
// and a recording IComponentHandler shows the gesture stream the host saw.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/editor_host.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/vst3_adapter.hpp>
#include <pulp/format/vst3_contract_view.hpp>
#include <pulp/state/store.hpp>

#include <pluginterfaces/base/keycodes.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <public.sdk/source/common/pluginview.h>

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using pulp::format::Editor;
using pulp::format::EditorContext;
using pulp::format::EditorFactory;
using pulp::format::EditorKey;
using pulp::format::EditorKeyEvent;
using pulp::format::EditorRole;
using pulp::format::EditorSize;
using pulp::format::KeyDisposition;
using pulp::format::NativeParent;
using pulp::format::NativeParentKind;
using pulp::format::ViewSize;
using pulp::format::vst3::ContractPlugView;
using pulp::format::vst3::PulpVst3Processor;

namespace {

constexpr pulp::state::ParamID kGain = 1;

/// Everything the contract editors did, in order.
struct EditorLog {
    std::vector<std::string> calls;
    void* parent_handle = nullptr;
    EditorContext* context = nullptr;
    int live = 0;

    bool saw(const std::string& call) const {
        for (const auto& c : calls)
            if (c == call)
                return true;
        return false;
    }
};
EditorLog g_log;

class RecordingEditor final : public Editor {
  public:
    explicit RecordingEditor(EditorContext& context) {
        g_log.context = &context;
        ++g_log.live;
    }
    ~RecordingEditor() override {
        --g_log.live;
        g_log.calls.push_back("destroyed");
    }

    bool supports_parent(NativeParentKind kind) const override {
        return kind != NativeParentKind::none;
    }
    bool attach(const NativeParent& parent) override {
        g_log.calls.push_back(parent.kind == NativeParentKind::cocoa_view ? "attach:cocoa_view"
                                                                         : "attach:other");
        g_log.parent_handle = parent.handle;
        return true;
    }
    void detach() override {
        g_log.calls.push_back("detach");
    }
    void invalidate() override {
        g_log.calls.push_back("invalidate");
    }
    EditorSize size() const override {
        return size_;
    }
    bool set_size(const EditorSize& size) override {
        // An undersized window is refused.
        if (size.width < 200 || size.height < 150)
            return false;
        size_ = size;
        g_log.calls.push_back("set_size:" + std::to_string(size.width) + "x" +
                              std::to_string(size.height));
        return true;
    }
    ViewSize size_hints() const override {
        ViewSize hints;
        hints.preferred_width = 400;
        hints.preferred_height = 300;
        hints.min_width = 200;
        hints.min_height = 150;
        hints.max_width = 800;
        hints.max_height = 600;
        return hints;
    }
    EditorSize adjust_size(const EditorSize& proposed) const override {
        // Holds 4:3.
        EditorSize adjusted = proposed;
        adjusted.height = proposed.width * 3 / 4;
        return adjusted;
    }
    KeyDisposition offer_key(const EditorKeyEvent& event) override {
        if (event.key == EditorKey::space) {
            g_log.calls.push_back("offer:space");
            return KeyDisposition::forward;
        }
        g_log.calls.push_back("offer:char");
        return KeyDisposition::consumed;
    }

  private:
    EditorSize size_{400, 300, 1.0};
};

class RecordingFactory final : public EditorFactory {
  public:
    bool supports(EditorRole role) const override {
        return role == EditorRole::editor;
    }
    std::unique_ptr<Editor> create(pulp::format::Processor&, EditorContext& context,
                                   EditorRole role) override {
        if (role != EditorRole::editor)
            return nullptr;
        return std::make_unique<RecordingEditor>(context);
    }
};

class GainProcessor : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor d;
        d.name = "Contract Gain";
        d.manufacturer = "Pulp";
        d.bundle_id = "dev.pulp.test.vst3-contract-editor";
        d.version = "1.0.0";
        d.input_buses = {{"Audio In", 2}};
        d.output_buses = {{"Audio Out", 2}};
        return d;
    }
    void define_parameters(pulp::state::StateStore& store) override {
        pulp::state::ParamInfo info;
        info.id = kGain;
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
};

class ContractProcessor final : public GainProcessor {
  public:
    EditorFactory* editor_factory() override {
        return &factory_;
    }

  private:
    RecordingFactory factory_;
};

std::unique_ptr<pulp::format::Processor> create_contract_processor() {
    return std::make_unique<ContractProcessor>();
}
std::unique_ptr<pulp::format::Processor> create_plain_processor() {
    return std::make_unique<GainProcessor>();
}

// The create_view() path's view, as a marker this test can recognise.
class LegacyMarkerView final : public Steinberg::CPluginView {
  public:
    LegacyMarkerView() : CPluginView(nullptr) {}
};
int g_legacy_views = 0;

class ScopedEnvUnset {
  public:
    explicit ScopedEnvUnset(const char* name) : name_(name) {
        if (const char* prev = std::getenv(name))
            prev_ = std::string(prev);
#if defined(_WIN32)
        _putenv_s(name_.c_str(), "");
#else
        ::unsetenv(name_.c_str());
#endif
    }
    ~ScopedEnvUnset() {
        if (!prev_)
            return;
#if defined(_WIN32)
        _putenv_s(name_.c_str(), prev_->c_str());
#else
        ::setenv(name_.c_str(), prev_->c_str(), 1);
#endif
    }

  private:
    std::string name_;
    std::optional<std::string> prev_;
};

// Editors are refused in CI and headless environments; these tests open one.
struct EditorEnvironment {
    ScopedEnvUnset disable_editor{"PULP_DISABLE_PLUGIN_EDITOR"};
    ScopedEnvUnset headless{"PULP_HEADLESS"};
    ScopedEnvUnset test_mode{"PULP_TEST_MODE"};
    ScopedEnvUnset ci{"CI"};
};

class HostApp final : public Steinberg::Vst::IHostApplication {
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

// The host's side of the gesture path, rendered as "B1 V1 E1".
class HostEdits final : public Steinberg::Vst::IComponentHandler {
  public:
    Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID id) override {
        stream += "B" + std::to_string(id) + " ";
        ++open;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID id,
                                              Steinberg::Vst::ParamValue) override {
        stream += "V" + std::to_string(id) + " ";
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID id) override {
        stream += "E" + std::to_string(id) + " ";
        --open;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32) override {
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IComponentHandler::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid)) {
            *obj = static_cast<Steinberg::Vst::IComponentHandler*>(this);
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

    std::string stream;
    int open = 0;
};

// Any non-null pointer stands in for the host's NSView: the adapter forwards
// it to the editor untouched.
int g_fake_parent = 0;
void* fake_parent() {
    return &g_fake_parent;
}

ContractPlugView* open_contract_view(PulpVst3Processor& processor) {
    auto* raw = processor.createView(Steinberg::Vst::ViewType::kEditor);
    REQUIRE(raw != nullptr);
    auto* view = dynamic_cast<ContractPlugView*>(raw);
    REQUIRE(view != nullptr);
    return view;
}

} // namespace

namespace pulp::format::vst3 {

Steinberg::IPlugView* make_plug_view(Processor&, state::StateStore&,
                                     runtime::AliveToken::Handle) {
    ++g_legacy_views;
    return new LegacyMarkerView();
}

} // namespace pulp::format::vst3

TEST_CASE("VST3 opens, resizes and closes a contract editor, twice",
          "[vst3][editor-host][vst3-contract]") {
    EditorEnvironment env;
    g_log = {};
    g_legacy_views = 0;
    HostApp host_app;
    HostEdits host;
    PulpVst3Processor processor(create_contract_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
    REQUIRE(processor.setComponentHandler(&host) == Steinberg::kResultOk);

    auto* view = open_contract_view(processor);
    REQUIRE(g_legacy_views == 0);

    // Size before attach: the editor's own size.
    Steinberg::ViewRect rect;
    REQUIRE(view->getSize(&rect) == Steinberg::kResultTrue);
    REQUIRE(rect.getWidth() == 400);
    REQUIRE(rect.getHeight() == 300);

    REQUIRE(view->isPlatformTypeSupported(Steinberg::kPlatformTypeNSView) == Steinberg::kResultTrue);
    REQUIRE(view->isPlatformTypeSupported("not-a-platform") == Steinberg::kResultFalse);
    REQUIRE(view->attached(fake_parent(), Steinberg::kPlatformTypeNSView) == Steinberg::kResultTrue);
    REQUIRE(g_log.saw("attach:cocoa_view"));
    REQUIRE(g_log.parent_handle == fake_parent());

    // Size negotiation: 4:3, clamped to the editor's bounds.
    REQUIRE(view->canResize() == Steinberg::kResultTrue);
    Steinberg::ViewRect proposed(0, 0, 1000, 1000);
    REQUIRE(view->checkSizeConstraint(&proposed) == Steinberg::kResultTrue);
    REQUIRE(proposed.getWidth() == 800);
    REQUIRE(proposed.getHeight() == 600);
    Steinberg::ViewRect resized(0, 0, 640, 480);
    REQUIRE(view->onSize(&resized) == Steinberg::kResultTrue);
    REQUIRE(g_log.saw("set_size:640x480"));
    Steinberg::ViewRect tiny(0, 0, 100, 100);
    REQUIRE(view->onSize(&tiny) == Steinberg::kResultFalse);
    REQUIRE(view->getSize(&rect) == Steinberg::kResultTrue);
    REQUIRE(rect.getWidth() == 640);
    REQUIRE(rect.getHeight() == 480);

    // Space is offered and forwarded, so the host keeps its transport key;
    // a key the editor consumes is reported as handled.
    REQUIRE(view->onKeyDown(u' ', Steinberg::KEY_SPACE, 0) == Steinberg::kResultFalse);
    REQUIRE(g_log.saw("offer:space"));
    REQUIRE(view->onKeyDown(u'a', 0, 0) == Steinberg::kResultTrue);

    // A drag through the contract's port reaches the host as one gesture, and
    // closing the window mid-drag still ends it.
    auto& params = g_log.context->params();
    params.begin(kGain);
    params.set(kGain, 0.75f);
    REQUIRE(host.open == 1);
    REQUIRE(view->removed() == Steinberg::kResultOk);
    REQUIRE(g_log.saw("detach"));
    REQUIRE(host.open == 0);
    REQUIRE(host.stream.rfind("B1 ", 0) == 0);
    REQUIRE(host.stream.find("E1 ") != std::string::npos);
    view->release();
    REQUIRE(g_log.live == 0);

    // Reopen: a fresh editor sees the value the first one set.
    g_log.calls.clear();
    view = open_contract_view(processor);
    REQUIRE(view->attached(fake_parent(), Steinberg::kPlatformTypeNSView) == Steinberg::kResultTrue);
    REQUIRE(g_log.context->params().get(kGain) == 0.75f);
    REQUIRE(view->removed() == Steinberg::kResultOk);
    view->release();
    REQUIRE(g_log.live == 0);
    REQUIRE(host.open == 0);
    REQUIRE_FALSE(g_log.saw("invalidate"));

    REQUIRE(processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 terminate invalidates a contract editor the host still holds",
          "[vst3][editor-host][vst3-contract]") {
    EditorEnvironment env;
    g_log = {};
    HostApp host_app;
    HostEdits host;
    PulpVst3Processor processor(create_contract_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
    REQUIRE(processor.setComponentHandler(&host) == Steinberg::kResultOk);

    auto* view = open_contract_view(processor);
    REQUIRE(view->attached(fake_parent(), Steinberg::kPlatformTypeNSView) == Steinberg::kResultTrue);
    g_log.context->params().begin(kGain);
    REQUIRE(host.open == 1);

    // The host tears the component down before the view: the editor is
    // invalidated (never detached into a dead window) and destroyed while the
    // processor that owns its factory still exists, and the gesture is ended.
    REQUIRE(processor.terminate() == Steinberg::kResultOk);
    REQUIRE(g_log.saw("invalidate"));
    REQUIRE_FALSE(g_log.saw("detach"));
    REQUIRE(g_log.live == 0);
    REQUIRE(host.open == 0);
    REQUIRE(view->editor() == nullptr);

    // Every later call on the orphaned view is a harmless refusal.
    Steinberg::ViewRect rect;
    REQUIRE(view->getSize(&rect) == Steinberg::kResultFalse);
    REQUIRE(view->onKeyDown(u' ', Steinberg::KEY_SPACE, 0) == Steinberg::kResultFalse);
    view->removed();
    view->release();
}

TEST_CASE("VST3 keeps create_view() for a processor without an editor factory",
          "[vst3][editor-host][vst3-contract]") {
    EditorEnvironment env;
    g_log = {};
    g_legacy_views = 0;
    HostApp host_app;
    PulpVst3Processor processor(create_plain_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);

    auto* raw = processor.createView(Steinberg::Vst::ViewType::kEditor);
    REQUIRE(raw != nullptr);
    REQUIRE(dynamic_cast<ContractPlugView*>(raw) == nullptr);
    REQUIRE(g_legacy_views == 1);
    REQUIRE(g_log.live == 0);
    raw->release();
    REQUIRE(processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 key callbacks map to the contract's key events",
          "[vst3][editor-host][vst3-contract]") {
    auto space = ContractPlugView::key_event(u' ', Steinberg::KEY_SPACE, Steinberg::kShiftKey, true);
    REQUIRE(space.key == EditorKey::space);
    REQUIRE(space.character == U' ');
    REQUIRE(space.modifiers == pulp::format::editor_modifier_shift);
    REQUIRE(space.is_down);

    auto escape = ContractPlugView::key_event(0, Steinberg::KEY_ESCAPE, 0, false);
    REQUIRE(escape.key == EditorKey::escape);
    REQUIRE(escape.character == 0);
    REQUIRE_FALSE(escape.is_down);

    REQUIRE(ContractPlugView::parent_kind(Steinberg::kPlatformTypeNSView) ==
            NativeParentKind::cocoa_view);
    REQUIRE(ContractPlugView::parent_kind(Steinberg::kPlatformTypeHWND) ==
            NativeParentKind::win32_window);
    REQUIRE(ContractPlugView::parent_kind("unknown") == NativeParentKind::none);
}
