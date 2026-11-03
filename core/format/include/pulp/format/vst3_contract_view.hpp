#pragma once

// The VST3 editor of a processor that supplies an editor factory
// (`Processor::editor_factory()`, see pulp/format/editor_host.hpp).
//
// `PulpVst3Processor::createView()` returns one of these whenever the factory
// is non-null, and falls back to `make_plug_view()` (the `create_view()` path)
// when it is null. The view translates IPlugView calls into the contract:
//
//   attached(parent, type)   -> Editor::attach(NativeParent)
//   removed()                -> every gesture released, Editor::detach()
//   getSize / onSize         -> Editor::size() / Editor::set_size()
//   canResize                -> Editor::size_hints() (equal min and max: fixed)
//   checkSizeConstraint      -> Editor::adjust_size(), clamped to the hints
//   onKeyDown / onKeyUp      -> Editor::offer_key() (forward = kResultFalse,
//                               so Space stays the host's transport key)
//   onFocus                  -> Editor::focus_changed()
//   setContentScaleFactor    -> Editor::set_size() with the new scale
//                               (IPlugViewContentScaleSupport)
//   while attached           -> Editor::idle() from a main-thread timer
//                               (detail::MainThreadTimer), started by attached()
//                               and stopped by removed(), owner termination and
//                               destruction; once stopped it runs no more
//                               plug-in code
//   adapter terminate        -> every gesture released and Editor::detach()
//                               while the host's window is still there, then the
//                               editor is destroyed before the processor that
//                               owns its factory
//
// Each view owns one ContractEditorSession (contract_editor_session.hpp): the
// editor, its StateStoreParamPort over the adapter's store (so edits reach the
// host through HostParameterEditBridge -> beginEdit/performEdit/endEdit, and
// closing one view never ends a gesture another view holds), and the idle
// tick. This class only maps IPlugView calls onto it.
//
// View-free: this names no view, canvas or render type, so it compiles into
// pulp-format-core with the audio adapter.

#include <pulp/format/contract_editor_session.hpp>
#include <pulp/format/editor_host.hpp>

#include <pluginterfaces/gui/iplugviewcontentscalesupport.h>
#include <public.sdk/source/common/pluginview.h>

#include <functional>
#include <memory>

namespace pulp::state {
class StateStore;
}

namespace pulp::format::vst3 {

class PulpVst3Processor;

class ContractPlugView final : public Steinberg::CPluginView,
                               public Steinberg::IPlugViewContentScaleSupport {
  public:
    /// Interval of the main-thread idle tick while attached (about 60 Hz).
    static constexpr int kIdleIntervalMs = ContractEditorSession::kIdleIntervalMs;

    using HostActionHandler = std::function<bool(const HostActionRequest&)>;

    /// A view over a new editor from `factory` in the editor role, or null
    /// when the factory builds none. `owner` is told when the view is
    /// destroyed; it calls `owner_terminated()` first if it goes away sooner.
    static ContractPlugView* create(PulpVst3Processor& owner, Processor& processor,
                                    state::StateStore& store, EditorFactory& factory,
                                    HostActionHandler host_action);

    ~ContractPlugView() override;

    // IPlugView
    Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API attached(void* parent, Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API removed() override;
    Steinberg::tresult PLUGIN_API onKeyDown(Steinberg::char16 key, Steinberg::int16 keyCode,
                                            Steinberg::int16 modifiers) override;
    Steinberg::tresult PLUGIN_API onKeyUp(Steinberg::char16 key, Steinberg::int16 keyCode,
                                          Steinberg::int16 modifiers) override;
    Steinberg::tresult PLUGIN_API getSize(Steinberg::ViewRect* size) override;
    Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect* newSize) override;
    Steinberg::tresult PLUGIN_API onFocus(Steinberg::TBool state) override;
    Steinberg::tresult PLUGIN_API canResize() override;
    Steinberg::tresult PLUGIN_API checkSizeConstraint(Steinberg::ViewRect* rect) override;

    // IPlugViewContentScaleSupport
    Steinberg::tresult PLUGIN_API setContentScaleFactor(ScaleFactor factor) override;

    OBJ_METHODS(ContractPlugView, CPluginView)
    DEFINE_INTERFACES
    DEF_INTERFACE(IPlugViewContentScaleSupport)
    END_DEFINE_INTERFACES(CPluginView)
    REFCOUNT_METHODS(CPluginView)

    /// The adapter is terminating or being destroyed while the host still
    /// holds this view. Releases every open gesture, detaches and destroys the
    /// editor (its factory belongs to the processor about to go), and
    /// answers every later call with kResultFalse.
    void owner_terminated();

    /// The editor, or null after `owner_terminated()`.
    Editor* editor() const {
        return session_ ? session_->editor() : nullptr;
    }
    /// This view's parameter port, or null after `owner_terminated()`.
    StateStoreParamPort* param_port() const {
        return session_ ? session_->param_port() : nullptr;
    }

    /// The contract's parent kind for a VST3 platform type, or `none` for a
    /// type it has no kind for.
    static NativeParentKind parent_kind(Steinberg::FIDString type);
    /// The contract's key event for a VST3 key callback.
    static EditorKeyEvent key_event(Steinberg::char16 key, Steinberg::int16 key_code,
                                    Steinberg::int16 modifiers, bool is_down);

  private:
    explicit ContractPlugView(PulpVst3Processor& owner);
    Steinberg::tresult offer(const EditorKeyEvent& event);

    PulpVst3Processor* owner_;
    std::unique_ptr<ContractEditorSession> session_;
};

} // namespace pulp::format::vst3
