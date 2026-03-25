#include <pulp/render/skia_surface.hpp>

#ifdef PULP_HAS_SKIA

#include <pulp/canvas/skia_canvas.hpp>

// Dawn C++ API (from Skia's bundled Dawn)
#include "webgpu/webgpu_cpp.h"
#include "dawn/native/DawnNative.h"
#include "dawn/dawn_proc.h"

// Skia Graphite headers
#include "include/gpu/graphite/Context.h"
#include "include/gpu/graphite/ContextOptions.h"
#include "include/gpu/graphite/Recorder.h"
#include "include/gpu/graphite/Recording.h"
#include "include/gpu/graphite/Surface.h"
#include "include/gpu/graphite/dawn/DawnBackendContext.h"
#include "include/gpu/graphite/dawn/DawnUtils.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkSurface.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"

#include <iostream>

namespace pulp::render {

class SkiaSurfaceImpl : public SkiaSurface {
public:
    SkiaSurfaceImpl(uint32_t width, uint32_t height, float scale)
        : width_(width), height_(height), scale_(scale) {}

    ~SkiaSurfaceImpl() override {
        // Ensure all GPU work is done before destroying
        if (context_) {
            context_->submit({});
        }
    }

    bool init() {
        // Set up Dawn native procs
        const DawnProcTable& procs = dawn::native::GetProcs();
        dawnProcSetProcs(&procs);

        // Create Dawn instance
        wgpu::InstanceDescriptor instance_desc{};
        dawn_instance_ = wgpu::CreateInstance(&instance_desc);
        if (!dawn_instance_) {
            std::cerr << "[pulp-render] Failed to create Dawn instance\n";
            return false;
        }

        // Request adapter
        wgpu::RequestAdapterOptions adapter_opts{};
        adapter_opts.powerPreference = wgpu::PowerPreference::HighPerformance;

        wgpu::Adapter adapter;
        dawn_instance_.RequestAdapter(
            &adapter_opts,
            wgpu::CallbackMode::WaitAnyOnly,
            [&adapter](wgpu::RequestAdapterStatus status, wgpu::Adapter result, wgpu::StringView msg) {
                if (status == wgpu::RequestAdapterStatus::Success) {
                    adapter = std::move(result);
                } else {
                    std::cerr << "[pulp-render] Adapter request failed\n";
                }
            });
        dawn_instance_.ProcessEvents();

        if (!adapter) {
            std::cerr << "[pulp-render] No GPU adapter found\n";
            return false;
        }

        // Request device
        wgpu::DeviceDescriptor device_desc{};
        device_desc.label = "Pulp Skia Device";

        adapter.RequestDevice(
            &device_desc,
            wgpu::CallbackMode::WaitAnyOnly,
            [this](wgpu::RequestDeviceStatus status, wgpu::Device result, wgpu::StringView msg) {
                if (status == wgpu::RequestDeviceStatus::Success) {
                    dawn_device_ = std::move(result);
                } else {
                    std::cerr << "[pulp-render] Device request failed\n";
                }
            });
        dawn_instance_.ProcessEvents();

        if (!dawn_device_) {
            std::cerr << "[pulp-render] Failed to create device\n";
            return false;
        }

        dawn_queue_ = dawn_device_.GetQueue();

        // Create Skia Graphite context from Dawn
        skgpu::graphite::DawnBackendContext backend_ctx;
        backend_ctx.fInstance = dawn_instance_;
        backend_ctx.fDevice = dawn_device_;
        backend_ctx.fQueue = dawn_queue_;

        skgpu::graphite::ContextOptions ctx_options;
        context_ = skgpu::graphite::ContextFactory::MakeDawn(backend_ctx, ctx_options);

        if (!context_) {
            std::cerr << "[pulp-render] Failed to create Skia Graphite context\n";
            return false;
        }

        // Create recorder
        recorder_ = context_->makeRecorder();
        if (!recorder_) {
            std::cerr << "[pulp-render] Failed to create Skia recorder\n";
            return false;
        }

        // Create offscreen surface for rendering
        create_surface();

        std::cout << "[pulp-render] Skia Graphite + Dawn initialized\n";
        return true;
    }

    canvas::Canvas* begin_frame() override {
        if (!sk_surface_) return nullptr;

        SkCanvas* sk_canvas = sk_surface_->getCanvas();
        if (!sk_canvas) return nullptr;

        canvas_ = std::make_unique<canvas::SkiaCanvas>(sk_canvas);
        return canvas_.get();
    }

    void end_frame() override {
        canvas_.reset();

        if (!recorder_ || !context_) return;

        // Snap recording and submit
        auto recording = recorder_->snap();
        if (recording) {
            skgpu::graphite::InsertRecordingInfo info;
            info.fRecording = recording.get();
            context_->insertRecording(info);
            context_->submit({});
        }
    }

    void resize(uint32_t width, uint32_t height, float scale) override {
        width_ = width;
        height_ = height;
        scale_ = scale;
        create_surface();
    }

    bool is_available() const override {
        return context_ != nullptr && recorder_ != nullptr;
    }

private:
    uint32_t width_ = 0, height_ = 0;
    float scale_ = 1.0f;

    wgpu::Instance dawn_instance_;
    wgpu::Device dawn_device_;
    wgpu::Queue dawn_queue_;

    std::unique_ptr<skgpu::graphite::Context> context_;
    std::unique_ptr<skgpu::graphite::Recorder> recorder_;
    sk_sp<SkSurface> sk_surface_;
    std::unique_ptr<canvas::SkiaCanvas> canvas_;

    void create_surface() {
        if (!recorder_) return;

        int pixel_w = static_cast<int>(width_ * scale_);
        int pixel_h = static_cast<int>(height_ * scale_);

        SkImageInfo info = SkImageInfo::MakeN32Premul(pixel_w, pixel_h);
        sk_surface_ = SkSurfaces::RenderTarget(recorder_.get(), info);

        if (sk_surface_ && scale_ != 1.0f) {
            sk_surface_->getCanvas()->scale(scale_, scale_);
        }
    }
};

std::unique_ptr<SkiaSurface> SkiaSurface::create(GpuSurface& gpu, const Config& config) {
    (void)gpu; // We use Dawn directly, not wgpu-native

    auto surface = std::make_unique<SkiaSurfaceImpl>(config.width, config.height, config.scale_factor);
    if (!surface->init()) return nullptr;
    return surface;
}

} // namespace pulp::render

#else // !PULP_HAS_SKIA

namespace pulp::render {
std::unique_ptr<SkiaSurface> SkiaSurface::create(GpuSurface&, const Config&) {
    return nullptr;
}
}

#endif
