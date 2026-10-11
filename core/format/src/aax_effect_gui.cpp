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
#include <pulp/format/aax_display_scale.hpp>
#include <pulp/format/aax_model.hpp>
#include <pulp/format/plugin_descriptor.hpp>
#include <pulp/format/gpu_host_select.hpp>
#include <pulp/format/editor_idle_pump.hpp>
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
#include <optional>
#include <functional>
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
    PulpAaxEffectGUI()
        : aax_scale_(aax_display_scale(nullptr)) {}

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

        auto* model = host_model();
        if (!model) {
            runtime::log_error("AAX editor: data model does not expose an EditorHost");
            return;
        }

        Processor* processor = model->editor_processor();
        state::StateStore* store = model->editor_store();
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
            ViewBridge::Options::hosted_editor());

        std::string editor_error;
        if (!bridge_->open(&editor_error)) {
            runtime::log_error("AAX editor: ViewBridge::open failed ({})", editor_error);
            bridge_.reset();
            return;
        }

        const auto plan = plan_editor_size(bridge_->size_hints());
        const auto gpu = decide_gpu_host(*bridge_);
        const auto opts = editor_host_options(*bridge_, gpu, {plan.width, plan.height});

        host_ = view::PluginViewHost::create(*bridge_->view(), opts);
        if (!host_) {
            runtime::log_error("AAX editor: PluginViewHost::create() failed");
            bridge_->close();
            bridge_.reset();
            return;
        }
        // Run editor automation, restore/reload, and scripted work per vsync.
        idle_pump_ = make_editor_idle_pump(*bridge_);
        host_->set_idle_callback(idle_pump_);

        // Follow the host's GPU surface rather than sampling it once: on
        // Windows it does not exist until try_attach_to_parent() below. Also
        // carries the detach edge and owns the CPU-fallback diagnostic.
        gpu_surface_binding_ =
            bind_gpu_surface(*host_, bridge_->scripted_ui(), gpu, "AAX");

        if (!host_->try_attach_to_parent(parent)) {
            runtime::log_error("AAX editor: attach to native parent failed");
            gpu_surface_binding_.reset();
            host_.reset();
            bridge_->close();
            bridge_.reset();
            return;
        }
        // The Pulp view host remains in logical points. AAX's macOS container
        // transaction is in backing pixels, so keep this conversion at the
        // adapter boundary rather than scaling the view tree itself.
        aax_scale_ = aax_display_scale(parent);

        // AAX has no host-driven size callback: the plug-in reports a size from
        // GetViewSize and pushes later changes up through the container. Forward
        // native size changes so the surfaces resize, Processor::on_view_resized
        // fires, and Pro Tools re-lays-out the plug-in window to match.
        ViewBridge* bridge_ptr = bridge_.get();
        host_->set_resize_callback([this, bridge_ptr](uint32_t w, uint32_t h) {
            bridge_ptr->resize(w, h);
            if (auto* view_container = GetViewContainer()) {
                // AAX_Point's constructor is (vertical, horizontal), despite
                // the fields being named vert/horz.  Use the SDK's constructor
                // order for every host resize transaction; GetViewSize below
                // writes the named fields directly.
                AAX_Point size(
                    static_cast<float>(aax_host_dimension(h, aax_scale_)),
                    static_cast<float>(aax_host_dimension(w, aax_scale_)));
                view_container->SetViewSize(size);
            }
        });

        // AAX does not call back into the editor when the user drags a resize
        // affordance.  Register the editor-to-host transaction explicitly so
        // Processor::request_editor_resize() has a live path to Pro Tools.
        // The bridge updates its preferred size before SetViewSize because the
        // host may synchronously query GetViewSize while applying the change.
        processor->set_editor_resize_handler(
            this,
            [this, bridge_ptr](uint32_t w, uint32_t h) -> bool {
                if (!bridge_ptr || bridge_.get() != bridge_ptr || !GetViewContainer()) {
                    return false;
                }
                const bool accepted = detail::negotiate_preferred_size(
                    *bridge_ptr, w, h,
                    [this](uint32_t requested_width,
                           uint32_t requested_height) -> bool {
                        auto* container = GetViewContainer();
                        if (!container) return false;
                        AAX_Point size(
                            static_cast<float>(aax_host_dimension(requested_height, aax_scale_)),
                            static_cast<float>(aax_host_dimension(requested_width, aax_scale_)));
                        return container->SetViewSize(size) == AAX_SUCCESS;
                    });
                if (!accepted || !host_) return false;
                commit_editor_requested_viewport(
                    *host_, bridge_ptr->size_hints(), w, h);
                return true;
            },
            [this, bridge_ptr]() noexcept {
                return bridge_.get() == bridge_ptr && GetViewContainer() != nullptr;
            });

        // Install the design viewport before publishing the initial host size.
        // Pro Tools may synchronously lay out and paint from SetViewSize; if
        // the aspect lock is installed afterwards, that first transaction can
        // use the host's stale frame and crop the editor (including its resize
        // grip) until the user performs a second resize.
        if (plan.pin_viewport) {
            host_->set_design_viewport(static_cast<float>(plan.width),
                                       static_cast<float>(plan.height));
            host_->set_fixed_aspect_ratio(plan.aspect_ratio);
        }

        // AAX owns the initial outer editor frame. Do not call SetViewSize
        // during CreateViewContainer: Pro Tools may still be constructing its
        // wrapper, and an early write can leave the wrapper and native child in
        // different coordinate spaces. GetViewSize supplies the preferred
        // frame during insertion; SetViewSize is reserved for later explicit
        // editor resize requests.
        host_->set_size(plan.width, plan.height);

        bridge_->notify_attached();
        bridge_->resize(plan.width, plan.height);
        // Content-first: the first composited frame is the mounted document
        // (ViewBridge::prepare_first_frame()).
        bridge_->prepare_first_frame(*host_);

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
        const auto hints = size_hints();
        if (!hints) {
            return AAX_ERROR_NULL_OBJECT;
        }
        const auto plan = plan_editor_size(*hints);
        oViewSize->horz = static_cast<float>(aax_host_dimension(plan.width, aax_scale_));
        oViewSize->vert = static_cast<float>(aax_host_dimension(plan.height, aax_scale_));
        return AAX_SUCCESS;
    }

    /// The floor the host may shrink the editor to. A fixed-size editor reports
    /// its design size, so Pro Tools cannot squash a surface that cannot reflow.
    AAX_Result GetMinimumViewSize(AAX_Point* oMinimumViewSize) const override {
        if (!oMinimumViewSize) {
            return AAX_ERROR_NULL_ARGUMENT;
        }
        const auto hints = size_hints();
        if (!hints) {
            return AAX_ERROR_NULL_OBJECT;
        }
        const auto plan = plan_editor_size(*hints);
        oMinimumViewSize->horz = static_cast<float>(aax_host_dimension(plan.min_width, aax_scale_));
        oMinimumViewSize->vert = static_cast<float>(aax_host_dimension(plan.min_height, aax_scale_));
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

    /// Pro Tools drives AAX editor animation through this callback.  The
    /// generic PluginViewHost display link is not guaranteed to run while the
    /// host is busy or while the editor is hosted in a legacy AAX window, so
    /// relying on it alone leaves host automation and the editor's published
    /// modulation frame stranded in the store.  Run the same lightweight
    /// main-thread pump used by the display-link path and invalidate the view;
    /// the next host frame then advances the native editor's FrameClock and
    /// paints the current modulation state.
    AAX_Result TimerWakeup() override {
        if (!bridge_) return AAX_SUCCESS;
        if (idle_pump_) idle_pump_();
        if (auto* view = bridge_->view()) view->request_repaint();
        return AAX_SUCCESS;
    }

    /// Pulp paints the editor through its own view host (Skia/Dawn or
    /// CoreGraphics), so AAX's draw pass has nothing to do.
    AAX_Result Draw(AAX_Rect* /*iDrawRect*/) override { return AAX_SUCCESS; }

    // ── helpers ────────────────────────────────────────────────────────────

    /// The data model behind this editor.
    ///
    /// `GetEffectParameters()` has a const overload returning a const pointer,
    /// but the model is not itself const — the const-ness belongs to this GUI
    /// object, and the sizing queries AAX declares const still have to reach it.
    EditorHost* host_model() const {
        return dynamic_cast<EditorHost*>(
            const_cast<PulpAaxEffectGUI*>(this)->GetEffectParameters());
    }

    /// The hints the sizing queries answer from, or nullopt when this plug-in
    /// has no editor to size.
    ///
    /// AAX asks for a size whenever it likes, including before any window
    /// exists, so this cannot depend on the bridge. The plug-in's declared
    /// `Processor::view_size()` is reachable through the data model at any
    /// point; once the editor is open the bridge's hints supersede it, because a
    /// scripted UI may revise them as it loads. Deferring to
    /// `AAX_CEffectGUI::GetViewSize()` is not an option: it returns AAX_SUCCESS
    /// without writing the point, so the host would read whatever it passed in.
    std::optional<ViewSize> size_hints() const {
        if (bridge_) {
            return bridge_->size_hints();
        }
        auto* model = host_model();
        return model ? model->editor_view_size() : std::nullopt;
    }

    void teardown() {
        if (!bridge_ && !host_) {
            return;  // editor never opened
        }
        // A window closed mid-drag would strand an open automation record on
        // the host; the router outlives this editor, so it cannot notice.
        if (auto* model = host_model()) {
            model->release_editor_gestures();
            if (auto* processor = model->editor_processor()) {
                processor->set_editor_resize_handler(this, nullptr);
            }
        }
        // Unsubscribe before the host publishes its teardown and before the
        // bridge that owns the scripted session dies.
        gpu_surface_binding_.reset();
        if (host_) {
            // Drop the idle/resize callbacks with the host before the bridge
            // they capture dies.
            host_->detach();
            host_.reset();
        }
        idle_pump_ = {};
        if (bridge_) {
            bridge_->close();
            bridge_.reset();
        }
    }

    std::unique_ptr<ViewBridge> bridge_;
    std::unique_ptr<view::PluginViewHost> host_;
    std::function<void()> idle_pump_;
    view::PluginViewHost::GpuSurfaceSubscription gpu_surface_binding_;
    float aax_scale_ = 1.0f;
};

} // namespace

IACFUnknown* AAX_CALLBACK create_effect_gui() {
    return static_cast<IACFUnknown*>(new PulpAaxEffectGUI());
}

} // namespace pulp::format::aax
