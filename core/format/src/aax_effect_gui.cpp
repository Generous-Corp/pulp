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
#include <pulp/format/aax_native_view.hpp>
#include <pulp/format/aax_model.hpp>
#include <pulp/format/plugin_descriptor.hpp>
#include <pulp/format/gpu_host_select.hpp>
#include <pulp/format/editor_idle_pump.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/runtime/trace.hpp>
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
#include <atomic>
#include <chrono>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#endif

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
#if defined(__APPLE__)
      : geometry_report_alive_(std::make_shared<std::atomic<bool>>(true)),
#else
      :
#endif
        aax_scale_(aax_display_scale(nullptr)) {
  }

  ~PulpAaxEffectGUI() override {
#if defined(__APPLE__)
      geometry_report_alive_->store(false, std::memory_order_release);
#endif
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

        // The editor runs on the processor that renders audio when the host has
        // built one; its removal must take the view tree down first.
        model->set_editor_invalidation_handler([this] {
            runtime::log_info("AAX editor: audio-owner processor going away; closing editor");
            teardown();
        });

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
        // The driver's call stamps itself so TimerWakeup can tell a live
        // driver from a quiet one (see there).
        host_->set_idle_callback([this] {
            last_driver_pump_ = std::chrono::steady_clock::now();
            if (idle_pump_)
                idle_pump_();
        });

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
            // Pro Tools resizes its parent in response to the plug-in's own
            // SetViewSize, and the child follows through autoresizing, so a
            // frame change that matches the size already published is the
            // host's echo of that transaction, not a new size to publish.
            // Forwarding it anyway opens a second host transaction per resize
            // step; forwarding a frame the host never asked for (an
            // autoresizing artefact) can resize the host window to it.
            if (w == published_width_ && h == published_height_) {
                runtime::log_info("AAX editor: native frame -> {}x{} (published)", w, h);
                return;
            }
            publish_view_size(w, h, "native frame");
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
                    [this](uint32_t requested_width, uint32_t requested_height) -> bool {
                        return publish_view_size(requested_width, requested_height,
                                                 "editor request") == AAX_SUCCESS;
                    });
                if (!accepted || !host_)
                    return false;
                commit_editor_requested_viewport(*host_, bridge_ptr->size_hints(), w, h);
                return true;
            },
            [this, bridge_ptr]() noexcept {
                return bridge_.get() == bridge_ptr && GetViewContainer() != nullptr;
            });

        // Viewport pin + aspect lock through the predicate every other adapter
        // uses (VST3 PulpPlugView, the AU v2 Cocoa view): the root lays out in
        // the AUTHORED box and paint scales it into the host frame. Top-align
        // like AU v2 — AAX cannot negotiate the pane aspect either, so slack
        // collects as one bottom strip instead of two bands.
        const ViewSize& hints = bridge_->size_hints();
        configure_native_viewport(*host_, hints);
        if (should_pin_design_viewport(hints)) {
            host_->set_design_viewport_top_align(true);
        }
        runtime::log_info("AAX editor: viewport preferred={}x{} design={}x{} pin={} aspect={}",
                          plan.width, plan.height, plan.design_width, plan.design_height,
                          plan.pin_viewport, plan.aspect_ratio);

        // The host sizes its parent from GetViewSize; the child is seeded at
        // the same preferred size so the first frame, the native child, and
        // the host's preferred frame share one geometry. Reconciling the two
        // against the parent the host actually built happens below, once the
        // first frame exists.
        host_->set_size(plan.width, plan.height);

        bridge_->notify_attached();
        bridge_->resize(plan.width, plan.height);
        // Content-first: the first composited frame is the mounted document
        // (ViewBridge::prepare_first_frame()).
        bridge_->prepare_first_frame(*host_);

        runtime::log_info("AAX editor: attached ({}x{}, mode={}, gpu={})",
                          plan.width, plan.height, gpu.mode, host_->is_gpu_backed());

        reconcile_native_parent(parent, plan);
    }

    /// Fit the native child to the parent Pro Tools handed down.
    ///
    /// `PluginViewHost::attach_to_parent` only adds the child at origin (0,0),
    /// which in AppKit's upward-growing view space is the parent's BOTTOM-LEFT
    /// corner; a parent larger than the child then shows the editor in its
    /// lower-left with the rest of the pane empty. The established AAX
    /// editor contract is to place content at the top-left and re-assert the
    /// plug-in's own size when the parent changes; this does that, once, and
    /// records every frame it saw so the host receipt shows what Pro Tools
    /// requested and accepted.
    /// Should the host keep an off-size parent anyway, the child fills it and
    /// the pinned design viewport letterboxes the content — a whole,
    /// proportional editor rather than a clipped corner.
    void reconcile_native_parent(void* parent, const EditorSizePlan& plan) {
        if (!host_)
            return;
        void* child = host_->native_handle();
        auto geometry = aax_native_geometry(parent, child);
        runtime::log_info("AAX editor: attach geometry {}", aax_describe_geometry(geometry));
        if (!geometry.valid)
            return;

        aax_place_child_top_left(parent, child);

        auto parent_matches = [&](const AaxNativeGeometry& g) {
            return static_cast<uint32_t>(g.parent_bounds.width + 0.5f) == plan.width &&
                   static_cast<uint32_t>(g.parent_bounds.height + 0.5f) == plan.height;
        };
        if (!parent_matches(geometry)) {
            // The host lays its parent out from this transaction, often
            // synchronously and from 0x0. The child must not follow that
            // delta (see aax_set_child_tracks_parent); it is re-fitted below.
            aax_set_child_tracks_parent(child, false);
            const AAX_Result result =
                publish_view_size(plan.width, plan.height, "host parent mismatch");
            runtime::log_info("AAX editor: host parent {}x{} != preferred {}x{}; SetViewSize -> {}",
                              static_cast<int>(geometry.parent_bounds.width),
                              static_cast<int>(geometry.parent_bounds.height), plan.width,
                              plan.height, static_cast<int32_t>(result));
            geometry = aax_native_geometry(parent, child);
            runtime::log_info("AAX editor: geometry after SetViewSize {}",
                              aax_describe_geometry(geometry));
            if (parent_matches(geometry)) {
                aax_place_child_top_left(parent, child);
            } else {
                aax_fill_parent(parent, child);
                runtime::log_info("AAX editor: host kept {}x{}; child fills the parent and the "
                                  "design viewport letterboxes the content",
                                  static_cast<int>(geometry.parent_bounds.width),
                                  static_cast<int>(geometry.parent_bounds.height));
            }
            aax_set_child_tracks_parent(child, true);
        }
        runtime::log_info("AAX editor: settled geometry {}",
                          aax_describe_geometry(aax_native_geometry(parent, child)));
#if defined(__APPLE__)
        schedule_geometry_report(parent, 1.0);
        schedule_geometry_report(parent, 3.0);
#endif
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
        log_size_query("GetViewSize", view_size_queries_, *oViewSize);
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
        oMinimumViewSize->vert =
            static_cast<float>(aax_host_dimension(plan.min_height, aax_scale_));
        log_size_query("GetMinimumViewSize", minimum_size_queries_, *oMinimumViewSize);
        return AAX_SUCCESS;
    }

    /// The one path to `AAX_IViewContainer::SetViewSize`. Records the size
    /// the host was asked for so the resize callback can tell the host's echo
    /// of this transaction from a genuinely new frame. `AAX_Point`'s
    /// constructor is (vertical, horizontal), despite the fields being named
    /// vert/horz; GetViewSize writes the named fields directly.
    AAX_Result publish_view_size(uint32_t w, uint32_t h, const char* why) {
        auto* container = GetViewContainer();
        if (!container)
            return AAX_ERROR_NULL_OBJECT;
        AAX_Point size(static_cast<float>(aax_host_dimension(h, aax_scale_)),
                       static_cast<float>(aax_host_dimension(w, aax_scale_)));
        published_width_ = w;
        published_height_ = h;
        const AAX_Result result = container->SetViewSize(size);
        runtime::log_info("AAX editor: {} -> {}x{}; SetViewSize -> {}", why, w, h,
                          static_cast<int32_t>(result));
        return result;
    }

    /// Host sizing queries, in the receipt. Pro Tools asks before and after
    /// the container exists; which of the two a query came from is the fact
    /// that decides whether a wrong outer frame came from the plug-in's answer
    /// or from the host. Bounded so a host polling per paint cannot flood it.
    void log_size_query(const char* query, std::atomic<uint32_t>& counter,
                        const AAX_Point& size) const {
        constexpr uint32_t kLoggedQueries = 8;
        const uint32_t n = counter.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n > kLoggedQueries)
            return;
        runtime::log_info("AAX editor: {} #{} -> {}x{} ({})", query, n, static_cast<int>(size.horz),
                          static_cast<int>(size.vert), bridge_ ? "editor open" : "no editor yet");
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
        PULP_TRACE_SCOPE_NAMED("render", "aax_timer_wakeup");
        if (!bridge_)
            return AAX_SUCCESS;
        // The view host's frame driver already runs this pump once per
        // presented frame. Running it again from the host's ~20 Hz timer
        // doubled the pump rate in Pro Tools (a trace measured 67 pumps/s for
        // 46 frames, 3 ms each, 25 ms worst case, all on the host's main
        // thread). The timer therefore pumps only when the driver has gone
        // quiet — a legacy AAX window or a busy host — and otherwise just
        // asks for a frame.
        const auto now = std::chrono::steady_clock::now();
        const bool driver_quiet = last_driver_pump_ == std::chrono::steady_clock::time_point{} ||
                                  now - last_driver_pump_ > kDriverQuietAfter;
        if (driver_quiet && idle_pump_)
            idle_pump_();
        if (auto* view = bridge_->view())
            view->request_repaint();
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
            model->set_editor_invalidation_handler({});
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

#if defined(__APPLE__)
    /// Read-only: log the parent/child/window frames after the host has had
    /// `delay_seconds` to finish its own window layout. Pro Tools can resize
    /// the container after SetViewContainer returns, and only a later read
    /// shows the frame the user actually sees. Never writes a frame or opens a
    /// SetViewSize transaction — a repeated size publication from a timer is
    /// what previously fought host and user resizes.
    void schedule_geometry_report(void* parent, double delay_seconds) {
        auto alive = std::weak_ptr<std::atomic<bool>>(geometry_report_alive_);
        auto* owner = this;
        const auto when =
            dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(delay_seconds * NSEC_PER_SEC));
        dispatch_after(when, dispatch_get_main_queue(), ^{
          auto token = alive.lock();
          if (!token || !token->load(std::memory_order_acquire))
              return;
          if (!owner->host_)
              return;
          runtime::log_info(
              "AAX editor: geometry +{}s {}", delay_seconds,
              aax_describe_geometry(aax_native_geometry(parent, owner->host_->native_handle())));
        });
    }
#endif

    std::unique_ptr<ViewBridge> bridge_;
    std::unique_ptr<view::PluginViewHost> host_;
    std::function<void()> idle_pump_;
    view::PluginViewHost::GpuSurfaceSubscription gpu_surface_binding_;
#if defined(__APPLE__)
    std::shared_ptr<std::atomic<bool>> geometry_report_alive_;
#endif
    uint32_t published_width_ = 0; ///< last size handed to SetViewSize
    uint32_t published_height_ = 0;
    /// When the view host's frame driver last ran the idle pump; the timer
    /// defers to the driver while it is within this window.
    static constexpr std::chrono::milliseconds kDriverQuietAfter{120};
    std::chrono::steady_clock::time_point last_driver_pump_{};
    mutable std::atomic<uint32_t> view_size_queries_{0};
    mutable std::atomic<uint32_t> minimum_size_queries_{0};
    float aax_scale_ = 1.0f;
};

} // namespace

IACFUnknown* AAX_CALLBACK create_effect_gui() {
    return static_cast<IACFUnknown*>(new PulpAaxEffectGUI());
}

} // namespace pulp::format::aax
