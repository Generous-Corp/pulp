// Editor for the GPU fault-continuity test plug-in, and the test-only
// render_force_loss hook.
//
// The hook loses the editor's RENDER device through the surface's existing
// device accessor (GpuSurface::dawn_device_handle()); nothing in the render
// slice is edited or called beyond that accessor. GPU audio owns a separate
// Dawn device, so the expected outcome is "the editor goes dark or logs
// errors, audio continues". A host crash is a real finding and the harness
// records it as FAIL. This hook belongs to the pre-Vellum-cutover harness and
// moves to Vellum (or is removed) after it.

#include "gpu_fault_continuity.hpp"

// Test-only: this plug-in fires GPU faults on request and must never ship.
#if !defined(PULP_GPU_FAULT_TEST_ONLY)
#error "the GPU fault-continuity test plug-in builds only with PULP_GPU_FAULT_TEST_ONLY"
#endif

#include <pulp/canvas/canvas.hpp>
#include <pulp/render/gpu_surface.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/view.hpp>

#if defined(PULP_GPU_FAULT_HAS_DAWN)
#include "webgpu/webgpu_cpp.h"
#endif

#include <dispatch/dispatch.h>

#include <memory>
#include <string>

namespace pulp::examples::gpu_fault {
namespace {

// Shared between the view and a queued main-queue task, so a task that runs
// after the editor closed sees `alive == false` instead of a dangling surface.
// The processor is reached only through its link, which it clears when it is
// destroyed, so neither the view nor a queued task can outlive it unsafely.
struct SurfaceSlot {
    render::GpuSurface* surface = nullptr;
    bool alive = true;
    std::shared_ptr<GpuFaultContinuityProcessor::EditorLink> link;
};

template <typename F> void with_processor(SurfaceSlot& slot, F&& f) {
    std::lock_guard lock(slot.link->mutex);
    if (slot.link->processor != nullptr)
        f(*slot.link->processor);
}

void force_render_loss(void* context) {
    std::unique_ptr<std::shared_ptr<SurfaceSlot>> holder(
        static_cast<std::shared_ptr<SurfaceSlot>*>(context));
    auto& slot = **holder;
    if (!slot.alive)
        return;
    const auto report = [&slot](bool performed, const char* detail) {
        with_processor(slot, [&](GpuFaultContinuityProcessor& processor) {
            processor.report_render_loss(performed, detail);
        });
    };
    if (slot.surface == nullptr) {
        report(false, "the editor has no GPU surface");
        return;
    }
#if defined(PULP_GPU_FAULT_HAS_DAWN)
    auto* device = static_cast<wgpu::Device*>(slot.surface->dawn_device_handle());
    if (device == nullptr || !*device) {
        report(false, "the GPU surface exposes no Dawn device");
        return;
    }
    device->ForceLoss(wgpu::DeviceLostReason::Unknown, "WP-h1 render_force_loss");
    report(true, "ForceLoss issued on the editor render device");
#else
    report(false, "built without Dawn headers");
#endif
}

class GpuFaultView final : public view::View {
  public:
    explicit GpuFaultView(GpuFaultContinuityProcessor& processor)
        : slot_(std::make_shared<SurfaceSlot>()) {
        slot_->link = processor.editor_link();
        // render_force_loss needs a Dawn render device to lose, so this editor
        // must run on the GPU host, never the CoreGraphics one.
        set_requires_gpu_host(true);
        std::weak_ptr<SurfaceSlot> weak = slot_;
        processor.set_render_loss_hook([weak] {
            // The device is not pulled out from under a frame that is
            // mid-recording: the loss runs later, from the main queue.
            if (auto slot = weak.lock())
                dispatch_async_f(dispatch_get_main_queue(),
                                 new std::shared_ptr<SurfaceSlot>(std::move(slot)),
                                 &force_render_loss);
        });
    }

    ~GpuFaultView() override {
        with_processor(*slot_, [](GpuFaultContinuityProcessor& processor) {
            processor.set_render_loss_hook(nullptr);
        });
        slot_->alive = false;
        slot_->surface = nullptr;
    }

    void set_plugin_view_host(view::PluginViewHost* host) override {
        View::set_plugin_view_host(host);
        subscription_.reset();
        slot_->surface = nullptr;
        if (host == nullptr)
            return;
        std::weak_ptr<SurfaceSlot> weak = slot_;
        subscription_ =
            host->observe_gpu_surface([weak](const view::PluginViewHost::GpuSurfaceStatus& status) {
                if (auto slot = weak.lock())
                    slot->surface = status.surface;
            });
    }

    void paint(canvas::Canvas& canvas) override {
        const auto b = local_bounds();
        bool lost = false;
        with_processor(*slot_, [&lost](GpuFaultContinuityProcessor& processor) {
            lost = processor.render_loss_requested();
        });
        canvas.set_fill_color(lost ? canvas::Color::rgba(0.45f, 0.08f, 0.08f)
                                   : canvas::Color::rgba(0.08f, 0.30f, 0.16f));
        canvas.fill_rect(0, 0, b.width, b.height);
        request_frame();
    }

  private:
    std::shared_ptr<SurfaceSlot> slot_;
    view::PluginViewHost::GpuSurfaceSubscription subscription_;
};

} // namespace

std::unique_ptr<view::View> GpuFaultContinuityProcessor::create_view() {
    return std::make_unique<GpuFaultView>(*this);
}

} // namespace pulp::examples::gpu_fault
