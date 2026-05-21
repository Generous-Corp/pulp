#include <pulp/render/skia_surface.hpp>

#ifdef PULP_HAS_SKIA

#include <pulp/canvas/skia_canvas.hpp>
#include <pulp/runtime/log.hpp>

// Dawn C++ API (from Skia's bundled Dawn)
#include "webgpu/webgpu_cpp.h"

// Skia Graphite headers
#include "include/gpu/graphite/Context.h"
#include "include/gpu/graphite/ContextOptions.h"
#include "include/gpu/graphite/Recorder.h"
#include "include/gpu/graphite/Recording.h"
#include "include/gpu/graphite/Surface.h"
#include "include/gpu/graphite/BackendTexture.h"
#include "include/gpu/graphite/dawn/DawnBackendContext.h"
#include "include/gpu/graphite/dawn/DawnGraphiteTypes.h"
#include "include/gpu/graphite/dawn/DawnUtils.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkSurface.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"
#include <cstring>

namespace pulp::render {

class SkiaSurfaceImpl : public SkiaSurface {
public:
    SkiaSurfaceImpl(GpuSurface& gpu, uint32_t width, uint32_t height, float scale)
        : gpu_(gpu), width_(width), height_(height), scale_(scale) {}

    ~SkiaSurfaceImpl() override {
        if (context_) {
            context_->submit({});
        }
    }

    bool init() {
        // Get the shared Dawn device/queue/instance from GpuSurface.
        // GpuSurface owns these — SkiaSurface borrows them.
        auto* device_ptr = static_cast<wgpu::Device*>(gpu_.dawn_device_handle());
        auto* queue_ptr = static_cast<wgpu::Queue*>(gpu_.dawn_queue_handle());
        auto* instance_ptr = static_cast<wgpu::Instance*>(gpu_.dawn_instance_handle());

        if (!device_ptr || !*device_ptr || !queue_ptr || !*queue_ptr || !instance_ptr || !*instance_ptr) {
            runtime::log_error("SkiaSurface: GpuSurface does not provide Dawn handles");
            return false;
        }

        // Create Skia Graphite context from the SHARED Dawn device
        skgpu::graphite::DawnBackendContext backend_ctx;
        backend_ctx.fInstance = *instance_ptr;
        backend_ctx.fDevice = *device_ptr;
        backend_ctx.fQueue = *queue_ptr;

        skgpu::graphite::ContextOptions ctx_options;
        context_ = skgpu::graphite::ContextFactory::MakeDawn(backend_ctx, ctx_options);

        if (!context_) {
            runtime::log_error("SkiaSurface: failed to create Skia Graphite context");
            return false;
        }

        recorder_ = context_->makeRecorder();
        if (!recorder_) {
            runtime::log_error("SkiaSurface: failed to create recorder");
            return false;
        }

        // Create offscreen fallback target (used when no presentable surface)
        create_offscreen_target();

        runtime::log_info("SkiaSurface: Graphite initialized on shared Dawn device (presentable: {})",
            gpu_.has_surface() ? "yes" : "no");
        return true;
    }

    canvas::Canvas* begin_frame() override {
        if (!recorder_ || !context_) return nullptr;

        SkCanvas* sk_canvas = nullptr;

        if (gpu_.has_surface()) {
            // On-screen path: wrap the current presentable texture from GpuSurface.
            // GpuSurface::begin_frame() must have been called first.
            auto* texture_ptr = static_cast<wgpu::Texture*>(gpu_.current_texture_handle());
            if (texture_ptr && *texture_ptr) {
                WGPUTexture raw_texture = texture_ptr->Get();

                // Create a Graphite BackendTexture from the current surface texture
                skgpu::graphite::BackendTexture backend_tex =
                    skgpu::graphite::BackendTextures::MakeDawn(raw_texture);

                if (!backend_tex.isValid()) {
                    runtime::log_warn("SkiaSurface: BackendTexture::MakeDawn returned invalid texture");
                } else {
                    // Wrap it as an SkSurface for Skia drawing
                    // Android Vulkan swapchains typically use RGBA8; desktop uses BGRA8
#ifdef __ANDROID__
                    auto surface_color_type = kRGBA_8888_SkColorType;
#else
                    auto surface_color_type = kBGRA_8888_SkColorType;
#endif
                    frame_surface_ = SkSurfaces::WrapBackendTexture(
                        recorder_.get(),
                        backend_tex,
                        surface_color_type,
                        SkColorSpace::MakeSRGB(),
                        nullptr);  // props

                    if (frame_surface_) {
                        sk_canvas = frame_surface_->getCanvas();
                        if (sk_canvas && scale_ != 1.0f) {
                            sk_canvas->scale(scale_, scale_);
                        }
                    } else {
                        runtime::log_warn("SkiaSurface: WrapBackendTexture failed — falling back to offscreen");
                    }
                }
            } else {
                runtime::log_warn("SkiaSurface: no current texture from GpuSurface");
            }
        }

        // Fallback to offscreen target if no presentable texture
        if (!sk_canvas && offscreen_surface_) {
            sk_canvas = offscreen_surface_->getCanvas();
        }

        if (!sk_canvas) return nullptr;

        canvas_ = std::make_unique<canvas::SkiaCanvas>(sk_canvas, recorder_.get());
        return canvas_.get();
    }

    void end_frame() override {
        canvas_.reset();
        frame_surface_.reset();  // release per-frame wrapped surface

        if (!recorder_ || !context_) return;

        // Submit the Graphite recording to the shared device/queue.
        // The GPU work targets the same texture that GpuSurface will present.
        auto recording = recorder_->snap();
        if (recording) {
            skgpu::graphite::InsertRecordingInfo info;
            info.fRecording = recording.get();
            context_->insertRecording(info);
            context_->submit({});
        }

        // GpuSurface::end_frame() handles the actual present call.
    }

    void resize(uint32_t width, uint32_t height, float scale) override {
        width_ = width;
        height_ = height;
        scale_ = scale;
        create_offscreen_target();
    }

    bool read_current_rgba(std::vector<uint8_t>& pixels,
                           uint32_t& pixel_width,
                           uint32_t& pixel_height) override {
        auto* source = frame_surface_ ? frame_surface_.get() : offscreen_surface_.get();
        if (!source) return false;

        pixel_width = static_cast<uint32_t>(std::max(1, static_cast<int>(width_ * scale_)));
        pixel_height = static_cast<uint32_t>(std::max(1, static_cast<int>(height_ * scale_)));

        auto info = SkImageInfo::Make(static_cast<int>(pixel_width),
                                      static_cast<int>(pixel_height),
                                      kRGBA_8888_SkColorType,
                                      kPremul_SkAlphaType,
                                      SkColorSpace::MakeSRGB());
        pixels.resize(static_cast<size_t>(pixel_width) * static_cast<size_t>(pixel_height) * 4u);
        const auto row_bytes = static_cast<size_t>(pixel_width) * 4u;

        struct ReadbackState {
            std::vector<uint8_t>* pixels = nullptr;
            size_t row_bytes = 0;
            uint32_t height = 0;
            bool finished = false;
            bool ok = false;
        } state{&pixels, row_bytes, pixel_height, false, false};

        auto callback = [](SkImage::ReadPixelsContext ctx,
                           std::unique_ptr<const SkImage::AsyncReadResult> result) {
            auto* state = static_cast<ReadbackState*>(ctx);
            if (!state) return;
            state->finished = true;
            if (!result || result->count() < 1 || result->data(0) == nullptr) {
                return;
            }

            const auto* src = static_cast<const uint8_t*>(result->data(0));
            const auto src_row_bytes = result->rowBytes(0);
            for (uint32_t y = 0; y < state->height; ++y) {
                std::memcpy(state->pixels->data() + static_cast<size_t>(y) * state->row_bytes,
                            src + static_cast<size_t>(y) * src_row_bytes,
                            state->row_bytes);
            }
            state->ok = true;
        };

        if (context_) {
            context_->asyncRescaleAndReadPixels(source,
                                                info,
                                                SkIRect::MakeWH(static_cast<int>(pixel_width),
                                                                static_cast<int>(pixel_height)),
                                                SkImage::RescaleGamma::kSrc,
                                                SkImage::RescaleMode::kNearest,
                                                callback,
                                                &state);
            if (context_->submit(skgpu::graphite::SyncToCpu::kYes)) {
                context_->checkAsyncWorkCompletion();
                if (state.finished && state.ok) {
                    return true;
                }
            }
        }

        SkPixmap pixmap(info, pixels.data(), row_bytes);
        auto image = source->makeImageSnapshot();
        if (image && image->readPixels(pixmap, 0, 0)) {
            return true;
        }

        return source->readPixels(pixmap, 0, 0);
    }

    bool is_available() const override {
        return context_ != nullptr && recorder_ != nullptr;
    }

    skgpu::graphite::Context* graphite_context() const override {
        return context_.get();
    }

private:
    GpuSurface& gpu_;
    uint32_t width_ = 0, height_ = 0;
    float scale_ = 1.0f;

    std::unique_ptr<skgpu::graphite::Context> context_;
    std::unique_ptr<skgpu::graphite::Recorder> recorder_;

    // Per-frame surface wrapping the current presentable texture
    sk_sp<SkSurface> frame_surface_;

    // Offscreen fallback (used when no native surface is attached)
    sk_sp<SkSurface> offscreen_surface_;

    std::unique_ptr<canvas::SkiaCanvas> canvas_;

    void create_offscreen_target() {
        if (!recorder_) return;

        int pixel_w = static_cast<int>(width_ * scale_);
        int pixel_h = static_cast<int>(height_ * scale_);

        SkImageInfo info = SkImageInfo::MakeN32Premul(pixel_w, pixel_h);
        offscreen_surface_ = SkSurfaces::RenderTarget(recorder_.get(), info);

        if (offscreen_surface_ && scale_ != 1.0f) {
            offscreen_surface_->getCanvas()->scale(scale_, scale_);
        }
    }
};

std::unique_ptr<SkiaSurface> SkiaSurface::create(GpuSurface& gpu, const Config& config) {
    auto surface = std::make_unique<SkiaSurfaceImpl>(gpu, config.width, config.height, config.scale_factor);
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
