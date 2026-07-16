// AAX custom editor. Embeds a Pulp editor view in the Pro Tools plug-in window
// through the shared, format-agnostic PluginViewHost — the same seam the VST3,
// AU v2, AU v3, and CLAP adapters use, so the renderer, JS runtime, Yoga layout,
// and GPU surface selection are identical here.
//
// AAX drives the editor differently from the other formats. The host owns the
// window and hands down a view container; the plug-in never negotiates a size,
// it reports one from GetViewSize and pushes later changes back through
// AAX_IViewContainer::SetViewSize. That is closer to the AU v2 model (forward
// native size changes) than to VST3's (host asks, then tells).

#include <pulp/format/aax_effect_gui.hpp>

#include <pulp/format/aax_editor.hpp>
#include <pulp/format/aax_model.hpp>
#include <pulp/format/gpu_host_select.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/plugin_view_host.hpp>

#include <AAX_CEffectGUI.h>
#include <AAX_Enums.h>
#include <AAX_GUITypes.h>
#include <AAX_IEffectParameters.h>
#include <AAX_IViewContainer.h>

#include <memory>
#include <string>

namespace pulp::format::aax {

namespace {

/// The view-container type carrying a native parent this platform can host.
/// AAX reports UIView on iOS, which Pro Tools does not target; a container Pulp
/// cannot host is refused rather than reinterpreted.
constexpr int32_t kHostableContainerType =
#if defined(__APPLE__)
    AAX_eViewContainer_Type_NSView;
#elif defined(_WIN32)
    AAX_eViewContainer_Type_HWND;
#else
    AAX_eViewContainer_Type_NULL;
#endif

class PulpAaxEffectGUI final : public AAX_CEffectGUI {
public:
    PulpAaxEffectGUI() = default;

    ~PulpAaxEffectGUI() override {
        // The SDK calls DeleteViewContainer from SetViewContainer(null), but a
        // host that drops the GUI without that call would otherwise leak the
        // bridge and strand Processor::on_view_closed.
        teardown();
    }

private:
    // ── AAX_CEffectGUI pure virtual interface ──────────────────────────────

    /// Called from Initialize(), before any window exists. The editor is built
    /// in CreateViewContainer() instead, where the parent is known, so open and
    /// close stay symmetric and a plug-in whose window never appears never
    /// builds a view tree.
    void CreateViewContents() override {}

    /// Called from SetViewContainer() once a valid window is present.
    void CreateViewContainer() override {
        // Defensive: a host that hands over a new container without tearing the
        // old one down would otherwise strand the previous bridge and host.
        teardown();

        auto* container = GetViewContainer();
        if (!container) {
            return;
        }

        const int32_t type = container->GetType();
        if (type != kHostableContainerType) {
            runtime::log_error(
                "AAX editor: unsupported view-container type {} (expected {})",
                type, kHostableContainerType);
            return;
        }

        void* parent = container->GetPtr();
        if (!parent) {
            runtime::log_error("AAX editor: view container has no native parent");
            return;
        }

        auto* host_model = dynamic_cast<EditorHost*>(GetEffectParameters());
        if (!host_model) {
            runtime::log_error("AAX editor: data model does not expose an EditorHost");
            return;
        }

        Processor* processor = host_model->editor_processor();
        state::StateStore* store = host_model->editor_store();
        if (!processor || !store) {
            runtime::log_error("AAX editor: no model-side processor for the editor");
            return;
        }
        if (!processor->has_editor()) {
            runtime::log_info("AAX editor: processor has no editor");
            return;
        }

        bridge_ = std::make_unique<ViewBridge>(
            *processor, *store,
            ViewBridge::Options{
                .enable_hot_reload = dev_editor_hot_reload_enabled(),
                .role = ViewRole::Editor});

        std::string editor_error;
        if (!bridge_->open(&editor_error)) {
            runtime::log_error("AAX editor: ViewBridge::open failed ({})", editor_error);
            bridge_.reset();
            return;
        }

        const auto plan = plan_editor_size(bridge_->size_hints());
        const auto gpu = decide_gpu_host(*bridge_);
        view::PluginViewHost::Options opts;
        opts.size = {plan.width, plan.height};
        opts.use_gpu = gpu.use_gpu;

        host_ = view::PluginViewHost::create(*bridge_->view(), opts);
        if (!host_) {
            runtime::log_error("AAX editor: PluginViewHost::create() failed");
            bridge_->close();
            bridge_.reset();
            return;
        }
        warn_if_unexpected_cpu_fallback(gpu, host_.get());

        // Pump the scripted UI session (async results, timers, rAF) per vsync.
        host_->set_idle_callback(make_scripted_idle_pump(*bridge_));

        // Route navigator.gpu / canvas.getContext('webgpu') through the host's
        // live GpuSurface.
        if (auto* scripted = bridge_->scripted_ui()) {
            scripted->attach_gpu_surface(host_->gpu_surface());
            if (host_->gpu_surface()) {
                runtime::log_info(
                    "[plugin-gpu-host] GpuSurface attached to WidgetBridge "
                    "via ScriptedUiSession (AAX)");
            }
        }

        if (!host_->try_attach_to_parent(parent)) {
            runtime::log_error("AAX editor: attach to native parent failed");
            host_.reset();
            bridge_->close();
            bridge_.reset();
            return;
        }

        // AAX has no host-driven size callback: the plug-in reports a size from
        // GetViewSize and pushes later changes up through the container. Forward
        // native size changes so the surfaces resize, Processor::on_view_resized
        // fires, and Pro Tools re-lays-out the plug-in window to match.
        ViewBridge* bridge_ptr = bridge_.get();
        host_->set_resize_callback([this, bridge_ptr](uint32_t w, uint32_t h) {
            bridge_ptr->resize(w, h);
            if (auto* view_container = GetViewContainer()) {
                AAX_Point size(static_cast<float>(h), static_cast<float>(w));
                view_container->SetViewSize(size);
            }
        });

        // Set after attach succeeds so a failed attach never installs a
        // viewport transform on a host about to be destroyed.
        if (plan.pin_viewport) {
            host_->set_design_viewport(static_cast<float>(plan.width),
                                       static_cast<float>(plan.height));
            host_->set_fixed_aspect_ratio(plan.aspect_ratio);
        }

        bridge_->notify_attached();

        runtime::log_info("AAX editor: attached ({}x{}, mode={}, gpu={})",
                          plan.width, plan.height, gpu.mode, host_->is_gpu_backed());
    }

    /// Called from SetViewContainer() when the window goes away.
    void DeleteViewContainer() override { teardown(); }

    // ── AAX_IEffectGUI ─────────────────────────────────────────────────────

    AAX_Result GetViewSize(AAX_Point* oViewSize) const override {
        if (!oViewSize) {
            return AAX_ERROR_NULL_ARGUMENT;
        }
        // Before the editor is built the plug-in's declared hints are not
        // reachable, so let the base class answer rather than invent a size.
        if (!bridge_) {
            return AAX_CEffectGUI::GetViewSize(oViewSize);
        }
        const auto plan = plan_editor_size(bridge_->size_hints());
        oViewSize->horz = static_cast<float>(plan.width);
        oViewSize->vert = static_cast<float>(plan.height);
        return AAX_SUCCESS;
    }

    /// The floor the host may shrink the editor to. A fixed-size editor reports
    /// its design size, so Pro Tools cannot squash a surface that cannot reflow.
    AAX_Result GetMinimumViewSize(AAX_Point* oMinimumViewSize) const override {
        if (!oMinimumViewSize) {
            return AAX_ERROR_NULL_ARGUMENT;
        }
        if (!bridge_) {
            return AAX_CEffectGUI::GetMinimumViewSize(oMinimumViewSize);
        }
        const auto plan = plan_editor_size(bridge_->size_hints());
        oMinimumViewSize->horz = static_cast<float>(plan.min_width);
        oMinimumViewSize->vert = static_cast<float>(plan.min_height);
        return AAX_SUCCESS;
    }

    /// A host parameter change (automation, control surface, another editor).
    /// The model writes those into the editor store as they arrive, and the
    /// idle pump propagates them to parameter-bound widgets — that path has to
    /// work with no GUI open, so it cannot live here. This is the AAX-native
    /// nudge that the next frame should reflect the new value.
    AAX_Result ParameterUpdated(AAX_CParamID /*paramID*/) override {
        if (auto* view = bridge_ ? bridge_->view() : nullptr) {
            view->request_repaint();
        }
        return AAX_SUCCESS;
    }

    /// Pulp paints the editor through its own view host (Skia/Dawn or
    /// CoreGraphics), so AAX's draw pass has nothing to do.
    AAX_Result Draw(AAX_Rect* /*iDrawRect*/) override { return AAX_SUCCESS; }

    // ── helpers ────────────────────────────────────────────────────────────

    void teardown() {
        if (!bridge_ && !host_) {
            return;  // editor never opened
        }
        // A window closed mid-drag would strand an open automation record on
        // the host; the router outlives this editor, so it cannot notice.
        if (auto* host_model = dynamic_cast<EditorHost*>(GetEffectParameters())) {
            host_model->release_editor_gestures();
        }
        if (host_) {
            // Drop the idle/resize callbacks with the host before the bridge
            // they capture dies.
            host_->detach();
            host_.reset();
        }
        if (bridge_) {
            bridge_->close();
            bridge_.reset();
        }
    }

    std::unique_ptr<ViewBridge> bridge_;
    std::unique_ptr<view::PluginViewHost> host_;
};

} // namespace

IACFUnknown* AAX_CALLBACK create_effect_gui() {
    return static_cast<IACFUnknown*>(new PulpAaxEffectGUI());
}

} // namespace pulp::format::aax
