#pragma once

// VST3 host-side editor (IPlugView) negotiation.
//
// A hosted VST3 editor is embedded like the CLAP one: the plug-in's view
// CONSUMES a parent (IPlugView::attached), so the host creates a container view
// (hosted_editor_container.hpp), hands it to the plug-in as the parent, and
// returns the container as the HostedEditor::native_handle.
//
// The AppKit part of that dance is the container (create_editor_container); the
// VST3 negotiation — createView, platform-type check, size query, attach,
// resize, teardown — is pure interface calls, so it lives here as a testable
// seam that Vst3Slot orchestrates and a headless test drives with a fake
// IEditController / IPlugView. Vst3Slot is anonymous-namespace and not
// test-reachable, so this seam is the test entry point.
//
// Include only from translation units compiled with the VST3 SDK on the
// include path (PULP_HAS_VST3).

#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>

#include <cstdint>

namespace pulp::host::detail {

// The platform view type the container provides. The container is only backed
// by a real view on macOS today; the other constants keep the negotiation
// honest on the platforms whose container is still a stub.
#if defined(__APPLE__)
inline const Steinberg::FIDString kVst3EditorPlatformType =
    Steinberg::kPlatformTypeNSView;
#elif defined(_WIN32)
inline const Steinberg::FIDString kVst3EditorPlatformType =
    Steinberg::kPlatformTypeHWND;
#else
inline const Steinberg::FIDString kVst3EditorPlatformType =
    Steinberg::kPlatformTypeX11EmbedWindowID;
#endif

// Ask the controller for its editor view and confirm it can embed into this
// platform's container. Returns the created (but NOT yet attached) IPlugView
// with its initial size, or nullptr if the plug-in has no editor, the platform
// type is unsupported, or the size is unusable. On any failure the view is
// released, so the caller owns a returned view and nothing on nullptr.
inline Steinberg::IPlugView* vst3_create_editor_view(
    Steinberg::Vst::IEditController* controller,
    uint32_t* out_width,
    uint32_t* out_height,
    bool* out_resizable) {
    if (!controller) return nullptr;
    Steinberg::IPlugView* view = controller->createView(Steinberg::Vst::ViewType::kEditor);
    if (!view) return nullptr;

    if (view->isPlatformTypeSupported(kVst3EditorPlatformType) != Steinberg::kResultTrue) {
        view->release();
        return nullptr;
    }

    Steinberg::ViewRect rect{};
    if (view->getSize(&rect) != Steinberg::kResultOk) {
        view->release();
        return nullptr;
    }
    const Steinberg::int32 w = rect.right - rect.left;
    const Steinberg::int32 h = rect.bottom - rect.top;
    if (w <= 0 || h <= 0) {
        view->release();
        return nullptr;
    }

    if (out_width) *out_width = (uint32_t)w;
    if (out_height) *out_height = (uint32_t)h;
    if (out_resizable) *out_resizable = (view->canResize() == Steinberg::kResultTrue);
    return view;
}

// Attach an already-created view to the host container. Returns false (without
// side effects the caller can't unwind) if the plug-in rejects the parent.
inline bool vst3_attach_editor_view(Steinberg::IPlugView* view, void* container) {
    if (!view || !container) return false;
    return view->attached(container, kVst3EditorPlatformType) == Steinberg::kResultOk;
}

// Detach a view previously passed to vst3_attach_editor_view. Pairs with
// attached(); call before releasing.
inline void vst3_detach_editor_view(Steinberg::IPlugView* view) {
    if (view) view->removed();
}

// Release the controller's reference-counted view.
inline void vst3_release_editor_view(Steinberg::IPlugView* view) {
    if (view) view->release();
}

// Negotiate a resize: let the plug-in snap (width,height) to a valid size, then
// commit it. On success (width,height) hold the accepted size. Returns false
// when the plug-in rejects the size or the view is null.
inline bool vst3_resize_editor_view(Steinberg::IPlugView* view,
                                    uint32_t* width, uint32_t* height) {
    if (!view || !width || !height) return false;
    Steinberg::ViewRect rect{0, 0, (Steinberg::int32)*width, (Steinberg::int32)*height};
    // checkSizeConstraint is optional for the plug-in; it may leave rect as-is.
    view->checkSizeConstraint(&rect);
    if (view->onSize(&rect) != Steinberg::kResultOk) return false;
    const Steinberg::int32 w = rect.right - rect.left;
    const Steinberg::int32 h = rect.bottom - rect.top;
    if (w <= 0 || h <= 0) return false;
    *width = (uint32_t)w;
    *height = (uint32_t)h;
    return true;
}

}  // namespace pulp::host::detail
