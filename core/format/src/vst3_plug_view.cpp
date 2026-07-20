// VST3 IPlugView implementation
// Creates AutoUi editor view and embeds it in the DAW's window via PluginViewHost.
// Platform-specific rendering is handled by PluginViewHost (CoreGraphics on macOS).

#ifdef PULP_VST3_GUI

#include <pulp/format/vst3_plug_view.hpp>
#include <pulp/format/gpu_host_select.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/text_editor.hpp>
#include <pluginterfaces/base/keycodes.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace pulp::format::vst3 {

using namespace Steinberg;

PulpPlugView::PulpPlugView(Processor& processor, state::StateStore& store,
                           runtime::AliveToken::Handle owner_alive)
    : CPluginView(nullptr)
    , processor_(processor)
    , store_(store)
    , bridge_(processor, store, std::move(owner_alive))
{
    const auto& hints = bridge_.size_hints();
    ViewRect r(0, 0, static_cast<int32>(hints.preferred_width),
               static_cast<int32>(hints.preferred_height));
    setRect(r);
}

tresult PLUGIN_API PulpPlugView::isPlatformTypeSupported(FIDString type) {
#ifdef __APPLE__
    if (strcmp(type, kPlatformTypeNSView) == 0) return kResultTrue;
#elif defined(_WIN32)
    if (strcmp(type, kPlatformTypeHWND) == 0) return kResultTrue;
#elif defined(__linux__)
    if (strcmp(type, kPlatformTypeX11EmbedWindowID) == 0) return kResultTrue;
#endif
    return kResultFalse;
}

tresult PLUGIN_API PulpPlugView::attached(void* parent, FIDString type) {
    std::string editor_error;
    if (!bridge_.open(&editor_error)) {
        runtime::log_error("VST3 editor: failed to build editor UI ({})", editor_error);
        return kResultFalse;
    }

    const auto& hints = bridge_.size_hints();
    const auto gpu = decide_gpu_host(bridge_);
    view::PluginViewHost::Options opts;
    opts.size = {hints.preferred_width, hints.preferred_height};
    opts.use_gpu = gpu.use_gpu;

    editor_host_ = view::PluginViewHost::create(*bridge_.view(), opts);
    if (!editor_host_) {
        runtime::log_error("VST3 editor: PluginViewHost::create() failed");
        bridge_.close();
        return kResultFalse;
    }
    warn_if_unexpected_cpu_fallback(gpu, editor_host_.get());

    // Pump the scripted UI session (async results, timers, rAF) per vsync.
    editor_host_->set_idle_callback(make_scripted_idle_pump(bridge_));

    // Route navigator.gpu / canvas.getContext('webgpu') through the
    // host's live GpuSurface.
    if (auto* scripted = bridge_.scripted_ui()) {
        scripted->attach_gpu_surface(editor_host_->gpu_surface());
        if (editor_host_->gpu_surface()) {
            runtime::log_info(
                "[plugin-gpu-host] GpuSurface attached to WidgetBridge "
                "via ScriptedUiSession (VST3)");
        }
    }

    editor_host_->attach_to_parent(parent);
    auto result = CPluginView::attached(parent, type);
    if (result != kResultTrue) {
        editor_host_->detach();
        editor_host_.reset();
        bridge_.close();
        return result;
    }

    // Resize contract: the shared should_pin_design_viewport() predicate
    // (plugin_descriptor.hpp) — pin+lock unless the plugin opted into free
    // drag (resizable + aspect_ratio==0), where checkSizeConstraint only
    // clamps min/max and the root reflows via Yoga at the host size.
    //
    // Set AFTER attach succeeds so a failed attach doesn't install the
    // pointTransform block on a host that's about to be destroyed.
    if (should_pin_design_viewport(hints)) {
        editor_host_->set_design_viewport(
            static_cast<float>(hints.preferred_width),
            static_cast<float>(hints.preferred_height));
        editor_host_->set_fixed_aspect_ratio(
            static_cast<float>(hints.preferred_width) /
            static_cast<float>(hints.preferred_height));
    }

    // Attach succeeded — now fire Processor::on_view_opened.
    bridge_.notify_attached();

    runtime::log_info("VST3 editor: attached ({}x{}, mode={}, gpu={})",
                      hints.preferred_width, hints.preferred_height,
                      gpu.mode, editor_host_->is_gpu_backed());
    return result;
}

tresult PLUGIN_API PulpPlugView::removed() {
    // Close the window mid-drag (mouse never lifted) and the host would keep
    // the automation record open forever. Release everything still held before
    // tearing the editor down, so every beginEdit is balanced by an endEdit.
    store_.release_open_gestures();

    if (editor_host_) {
        editor_host_->detach();
        editor_host_.reset();
    }
    bridge_.close();

    runtime::log_info("VST3 editor: removed");
    return CPluginView::removed();
}

tresult PLUGIN_API PulpPlugView::getSize(ViewRect* size) {
    if (!size) return kInvalidArgument;
    const auto& hints = bridge_.size_hints();
    size->left = 0;
    size->top = 0;
    size->right = static_cast<int32>(hints.preferred_width);
    size->bottom = static_cast<int32>(hints.preferred_height);
    return kResultTrue;
}

tresult PLUGIN_API PulpPlugView::canResize() {
    // Resizable iff the plugin declares non-zero min bounds — the same
    // convention CLAP's gui_can_resize uses (clap_entry.hpp). A plugin that
    // uses the base-class default (min==0) is fixed-size: report kResultFalse
    // so the host doesn't offer a resize grip. When resizable, checkSizeConstraint()
    // enforces the per-mode constraint (aspect snap when locked, min/max clamp
    // when free).
    const auto& hints = bridge_.size_hints();
    const bool resizable = hints.min_width > 0 && hints.min_height > 0;
    return resizable ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API PulpPlugView::checkSizeConstraint(ViewRect* rect) {
    if (!rect) return kInvalidArgument;
    const auto& hints = bridge_.size_hints();
    if (hints.preferred_width == 0 || hints.preferred_height == 0) {
        return kResultFalse;
    }
    int32 w = rect->getWidth();
    int32 h = rect->getHeight();
    if (w <= 0 || h <= 0) return kResultFalse;

    auto clamp = [](int32 v, uint32_t lo, uint32_t hi) {
        if (lo > 0 && v < static_cast<int32>(lo)) v = static_cast<int32>(lo);
        if (hi > 0 && v > static_cast<int32>(hi)) v = static_cast<int32>(hi);
        return v;
    };

    // Free-resize contract: resizable (min>0) with aspect_ratio==0 means "any
    // ratio; drag freely within [min,max]". Clamp each axis independently — NO
    // aspect snap — so the host lets the user pick any box in bounds and the
    // root reflows via Yoga at that size.
    const bool resizable = hints.min_width > 0 && hints.min_height > 0;
    const bool free_resize = resizable && hints.aspect_ratio <= 0.0;
    if (free_resize) {
        w = clamp(w, hints.min_width, hints.max_width);
        h = clamp(h, hints.min_height, hints.max_height);
        rect->right = rect->left + w;
        rect->bottom = rect->top + h;
        return kResultTrue;
    }

    // Snap to the design aspect ratio — pick the largest box at the
    // design aspect that fits within the requested rectangle. Same
    // shape the CLAP gui_adjust_size path lands on, mirroring the
    // standalone host's drag-to-resize behavior.
    const double design_aspect =
        static_cast<double>(hints.preferred_width) /
        static_cast<double>(hints.preferred_height);
    const double req_aspect =
        static_cast<double>(w) / static_cast<double>(h);
    if (req_aspect > design_aspect) {
        w = static_cast<int32>(static_cast<double>(h) * design_aspect + 0.5);
    } else if (req_aspect < design_aspect) {
        h = static_cast<int32>(static_cast<double>(w) / design_aspect + 0.5);
    }

    // Respect plugin min/max constraints when defined (zeros pass through).
    // A naive clamp after the aspect snap would re-introduce off-aspect
    // rects (design 2:1, request (500,1000) snaps to (500,250); a
    // min_height=400 clamp gives (500,400) — no longer 2:1). After any
    // clamp, re-snap by EXPANDING the other dimension to restore the
    // design aspect. Matches the CLAP gui_adjust_size shape. (`clamp` is
    // defined above, shared with the free-resize path.)
    auto resnap = [&]() {
        const double aspect_now = static_cast<double>(w) / static_cast<double>(h);
        if (aspect_now > design_aspect) {
            h = static_cast<int32>(static_cast<double>(w) / design_aspect + 0.5);
        } else if (aspect_now < design_aspect) {
            w = static_cast<int32>(static_cast<double>(h) * design_aspect + 0.5);
        }
    };
    const int32 w_clamped = clamp(w, hints.min_width, hints.max_width);
    if (w_clamped != w) { w = w_clamped; resnap(); }
    const int32 h_clamped = clamp(h, hints.min_height, hints.max_height);
    if (h_clamped != h) { h = h_clamped; resnap(); }

    rect->right = rect->left + w;
    rect->bottom = rect->top + h;
    return kResultTrue;
}

tresult PLUGIN_API PulpPlugView::onKeyDown(char16 key, int16 keyCode, int16 modifiers) {
    // REAPER with "send all keyboard input to plug-in" OFF (its default) offers
    // keys through the IPlugView pipeline BEFORE its own accelerator table.
    // Returning kResultFalse for Space is what hands it to transport — the
    // NSView key path (keyDown:/performKeyEquivalent:) never sees the event at
    // all, so no view-host-side routing can save a focused chat field. Space is
    // also the ONLY key REAPER delivers well-formed here (host_quirks:
    // reaper_keyboard_only_space), so the handler is deliberately scoped to it;
    // every other key keeps its existing NSView-path behavior.
    if (key != static_cast<char16>(u' ') && keyCode != KEY_SPACE) return kResultFalse;
    // Cmd/Ctrl-Space is a host/system chord (e.g. Spotlight), never text.
    if ((modifiers & (kCommandKey | kControlKey)) != 0) return kResultFalse;
    view::View* root = bridge_.view();
    if (!root) return kResultFalse;
    // Per-root focus slot (never allocate from the key path). Only a focused
    // TEXT field consumes Space; with none focused the host keeps transport.
    auto* state = root->existing_interaction();
    auto* te = state ? dynamic_cast<view::TextEditor*>(state->focused_input) : nullptr;
    if (!te) return kResultFalse;
    view::TextInputEvent ev;
    ev.text = " ";
    te->on_text_input(ev);
    root->request_repaint();
    return kResultTrue;
}

tresult PLUGIN_API PulpPlugView::onSize(ViewRect* newSize) {
    if (!newSize) return kInvalidArgument;

    auto result = CPluginView::onSize(newSize);

    uint32_t w = static_cast<uint32_t>(newSize->getWidth());
    uint32_t h = static_cast<uint32_t>(newSize->getHeight());
    bridge_.resize(w, h);
    if (editor_host_) {
        editor_host_->set_size(w, h);
    }

    return result;
}

} // namespace pulp::format::vst3

#endif // PULP_VST3_GUI
