#include <pulp/render/skia_surface.hpp>

#ifdef PULP_HAS_SKIA

#include <pulp/canvas/skia_canvas.hpp>
#include <pulp/render/gpu_render_time.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/runtime/trace.hpp>

#include "graphite_image_provider.hpp"

// Dawn C++ API (from Skia's bundled Dawn)
#include "webgpu/webgpu_cpp.h"

// Skia Graphite headers
#include "include/gpu/GpuTypes.h"            // GpuStats, GpuStatsFlags
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
#include "include/core/SkColor.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkRect.h"
#include "include/core/SkSamplingOptions.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

namespace pulp::render {

namespace {

// Map the presentable texture's ACTUAL format to the matching SkColorType.
//
// The swapchain format is negotiated at run time — GpuSurface::configure_surface
// asks Dawn for the surface capabilities and takes BGRA8Unorm only when the
// adapter advertises it, otherwise the adapter's first preferred format. So the
// format is a property of the adapter, not of the operating system, and it must
// be read back rather than assumed.
//
// This previously hardcoded RGBA8 on Android and BGRA8 everywhere else. When
// that guess disagreed with the real format, SkSurfaces::WrapBackendTexture
// returned null and begin_frame() silently fell through to the offscreen
// target: every frame still rendered (so screenshot readback looked perfect)
// but the texture actually being presented was never drawn into, leaving a
// black editor with no error. Seen with the D3D12 WARP adapter that an x64
// plug-in gets when hosted under Windows-on-ARM emulation.
//
// Returns kUnknown_SkColorType for formats Skia cannot wrap; the caller treats
// that as a hard error rather than a silent downgrade.
SkColorType sk_color_type_for(wgpu::TextureFormat format) {
    switch (format) {
        case wgpu::TextureFormat::BGRA8Unorm:
        case wgpu::TextureFormat::BGRA8UnormSrgb:
            // sRGB-ness rides on the SkColorSpace, not the SkColorType.
            return kBGRA_8888_SkColorType;
        case wgpu::TextureFormat::RGBA8Unorm:
        case wgpu::TextureFormat::RGBA8UnormSrgb:
            return kRGBA_8888_SkColorType;
        case wgpu::TextureFormat::RGBA16Float:
            return kRGBA_F16_SkColorType;
        case wgpu::TextureFormat::RGB10A2Unorm:
            return kRGBA_1010102_SkColorType;
        default:
            return kUnknown_SkColorType;
    }
}

const char* wgpu_texture_format_name(wgpu::TextureFormat format) {
    switch (format) {
        case wgpu::TextureFormat::BGRA8Unorm:     return "BGRA8Unorm";
        case wgpu::TextureFormat::BGRA8UnormSrgb: return "BGRA8UnormSrgb";
        case wgpu::TextureFormat::RGBA8Unorm:     return "RGBA8Unorm";
        case wgpu::TextureFormat::RGBA8UnormSrgb: return "RGBA8UnormSrgb";
        case wgpu::TextureFormat::RGBA16Float:    return "RGBA16Float";
        case wgpu::TextureFormat::RGB10A2Unorm:   return "RGB10A2Unorm";
        default:                                  return "unsupported";
    }
}

}  // namespace

class SkiaSurfaceImpl : public SkiaSurface {
public:
    SkiaSurfaceImpl(GpuSurface& gpu, uint32_t width, uint32_t height, float scale)
        : gpu_(gpu), width_(width), height_(height), scale_(scale) {}

    ~SkiaSurfaceImpl() override {
        // Release retained GPU images while their recorder/context are alive.
        canvas_.reset();
        retained_layers_.reset();
        if (image_provider_) image_provider_->clear();
        if (context_) {
            // Drain any outstanding GpuStats finished-with-stats callbacks
            // before `this` dies — they capture `this` as their context, so a
            // pending callback firing after destruction would be a
            // use-after-free. A sync submit + completion check flushes them
            // while the object is still alive.
            context_->submit(skgpu::graphite::SyncToCpu::kYes);
            context_->checkAsyncWorkCompletion();
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

        // Graphite drops any draw whose SkImage is not already GPU-backed
        // unless the Recorder carries an ImageProvider to upload it (Skia's
        // default provider returns nothing). Skia modules that decode images
        // internally — SkSVGDOM's `<image href="data:...">` among them — hand
        // Graphite raster images that no Pulp-side pre-upload can reach, so the
        // provider is what makes those draws land. See
        // graphite_image_provider.hpp.
        image_provider_ = sk_make_sp<GraphiteImageProvider>();
        skgpu::graphite::RecorderOptions recorder_options;
        recorder_options.fImageProvider = image_provider_;
        recorder_ = context_->makeRecorder(recorder_options);
        if (!recorder_) {
            image_provider_.reset();
            runtime::log_error("SkiaSurface: failed to create recorder");
            return false;
        }

        // Probe whether Graphite can report per-recording GPU render time on
        // this device. Graphite backs kElapsedTime with Dawn timestamp queries,
        // so it only advertises the stat when the shared device enabled
        // `timestamp-query` (GpuSurface requests it when the adapter offers it).
        // GpuStatsFlags has no bitmask operators, so test via the underlying
        // uint32_t.
        gpu_elapsed_supported_ =
            (static_cast<std::uint32_t>(context_->supportedGpuStats()) &
             static_cast<std::uint32_t>(skgpu::GpuStatsFlags::kElapsedTime)) != 0;

        // Create offscreen fallback target (used when no presentable surface)
        create_offscreen_target();

        runtime::log_info("SkiaSurface: Graphite initialized on shared Dawn device (presentable: {}, gpu-render-time: {})",
            gpu_.has_surface() ? "yes" : "no",
            gpu_elapsed_supported_ ? "available" : "unavailable");
        return true;
    }

    canvas::Canvas* begin_frame() override {
        // Default the outcome pessimistically: every early return below is a
        // frame that did not reach the screen, and the one place that can
        // upgrade it is a successful end_frame().
        last_outcome_ = FrameOutcome::failed;
        // A frame abandoned without end_frame() must not leave its flag set for
        // the next one.
        frame_work_submitted_ = false;
        if (!recorder_ || !context_) return nullptr;

        // Persistent-scene mode (FU-2): draw into the retained scene surface,
        // not the swapchain. The scene keeps last frame's pixels, so the host
        // can clip the repaint to the damaged rect and everything else stays.
        // The presentable drawable is wrapped later, at end_frame, for the blit.
        if (persistent_scene_) {
            if (!scene_surface_) create_scene_target();
            if (!scene_surface_) {
                // No scene target on a surface that claims persistent-scene
                // mode is a resource failure, not a policy: recreate.
                last_outcome_ = FrameOutcome::recreate;
                return nullptr;
            }
            frame_surface_.reset();
            SkCanvas* sk_canvas = scene_surface_->getCanvas();
            if (!sk_canvas) {
                last_outcome_ = FrameOutcome::recreate;
                return nullptr;
            }
            canvas_ = std::make_unique<canvas::SkiaCanvas>(
                sk_canvas, recorder_.get(), retained_layers_);
            return canvas_.get();
        }

        SkCanvas* sk_canvas = nullptr;
        frame_surface_.reset();

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
                    // Wrap it as an SkSurface for Skia drawing. The color type
                    // comes from the texture itself — the swapchain format is
                    // negotiated with the adapter at run time, so it cannot be
                    // inferred from the platform (see sk_color_type_for).
                    const wgpu::TextureFormat surface_format =
                        texture_ptr->GetFormat();
                    const SkColorType surface_color_type =
                        sk_color_type_for(surface_format);

                    if (surface_color_type == kUnknown_SkColorType) {
                        runtime::log_error(
                            "SkiaSurface: presentable texture format {} has no "
                            "SkColorType mapping — the editor cannot present. "
                            "Add it to sk_color_type_for().",
                            wgpu_texture_format_name(surface_format));
                    } else {
                        frame_surface_ = SkSurfaces::WrapBackendTexture(
                            recorder_.get(),
                            backend_tex,
                            surface_color_type,
                            SkColorSpace::MakeSRGB(),
                            nullptr);  // props
                    }

                    if (frame_surface_) {
                        sk_canvas = frame_surface_->getCanvas();
                        if (sk_canvas && scale_ != 1.0f) {
                            sk_canvas->scale(scale_, scale_);
                        }
                    } else if (surface_color_type != kUnknown_SkColorType) {
                        runtime::log_error(
                            "SkiaSurface: WrapBackendTexture failed for format {} "
                            "— rendering will go to the offscreen target and "
                            "NOTHING will be presented.",
                            wgpu_texture_format_name(surface_format));
                    }
                }
            } else {
                runtime::log_warn("SkiaSurface: no current texture from GpuSurface");
            }
        }

        // The offscreen target is the INTENDED output only when there is no
        // presentable surface (headless capture, render_to_png, tests). When a
        // presentable surface exists and its drawable could not be wrapped,
        // redirecting here would produce a perfect-looking frame that the
        // screen never sees — and, worse, one the caller would count as
        // successful and clear its damage for. Report it instead.
        if (!sk_canvas && gpu_.has_surface()) {
            last_outcome_ = FrameOutcome::recreate;
            return nullptr;
        }

        if (!sk_canvas && offscreen_surface_) {
            sk_canvas = offscreen_surface_->getCanvas();
        }

        if (!sk_canvas) return nullptr;

        canvas_ = std::make_unique<canvas::SkiaCanvas>(
            sk_canvas, recorder_.get(), retained_layers_);
        return canvas_.get();
    }

    FrameOutcome end_frame() override {
        // Graphite recording snap + insert + submit — the GPU-submit stage of
        // the frame pipeline.
        PULP_TRACE_SCOPE_NAMED("gpu", "gpu_submit");
        // Prune BEFORE dropping the canvas: this is the frame boundary, and it
        // is the first moment at which "sealed but never composited" is
        // knowable for a non-cacheable layer. Holding those GPU textures until
        // the store is replaced was an unbounded waste driven by whatever the
        // UI happened to draw (WAH-12).
        if (canvas_) canvas_->prune_abandoned_retained_layers();
        canvas_.reset();  // flush the frame's draws into the recorder

        if (!recorder_ || !context_) return finish_frame(FrameOutcome::failed);

        // Persistent-scene mode (FU-2): the frame drew into the retained scene
        // surface. Record a 1:1 blit of the scene onto the presentable drawable
        // into the SAME recorder, so the single snap() below carries both the
        // scene draws and the blit. Offscreen (no presentable surface) needs no
        // blit — the scene IS the readback target.
        //
        // A failed blit used to log and fall through to a submit that carried
        // only the scene draws, so the host presented a stale drawable and
        // cleared its damage: the retained scene silently stopped reaching the
        // screen while every frame still "succeeded".
        bool blit_ok = true;
        if (persistent_scene_ && scene_surface_)
            blit_ok = blit_scene_to_drawable();

        // Submit the Graphite recording to the shared device/queue.
        // The GPU work targets the same texture that GpuSurface will present.
        // Submitted even when the blit failed: the scene draws are still valid
        // work, and dropping them would force a full repaint of content that is
        // already correct in the retained scene.
        //
        // Success is judged on insertRecording's InsertStatus, NOT on submit()'s
        // bool. submit() reports "was there anything to send", which is legally
        // false on a frame whose work read_current_rgba already flushed — using
        // it would fail every captured frame and make the host repaint in full
        // forever. InsertStatus is the documented accepted/rejected signal.
        bool insert_ok = true;
        auto recording = recorder_->snap();
        if (recording) {
            skgpu::graphite::InsertRecordingInfo info;
            info.fRecording = recording.get();

            // Request GPU render time for this recording. The callback fires
            // on a later submit/completion (so the value lags ~1 frame); it
            // captures `this`, which is kept alive by the destructor drain.
            // Only request when supported so unsupported devices pay nothing.
            if (gpu_elapsed_supported_) {
                info.fGpuStatsFlags = skgpu::GpuStatsFlags::kElapsedTime;
                info.fFinishedContext = this;
                info.fFinishedWithStatsProc = &SkiaSurfaceImpl::on_gpu_stats;
            }

            const auto status = context_->insertRecording(info);
            insert_ok = (status == skgpu::graphite::InsertStatus::kSuccess);
            if (!insert_ok) {
                runtime::log_error(
                    "SkiaSurface: Graphite rejected this frame's recording — "
                    "nothing was drawn into the drawable.");
            }
            context_->submit({});
            frame_work_submitted_ = true;
        }

        // A null snap() with no earlier submit means this frame produced no GPU
        // work at all. When read_current_rgba() already flushed the frame, the
        // absent second recording is expected and the drawable is fine.
        const bool produced_work = recording != nullptr || frame_work_submitted_;
        frame_work_submitted_ = false;

        // GpuSurface::end_frame() handles the actual present call.
        // Keep frame_surface_ alive until the next begin_frame() so callers
        // can perform deterministic post-submit readback of the presentable
        // texture before it is presented/replaced.
        if (!blit_ok) return finish_frame(FrameOutcome::recreate);
        if (!insert_ok || !produced_work) return finish_frame(FrameOutcome::failed);
        return finish_frame(has_presentable_target() ? FrameOutcome::presented
                                                     : FrameOutcome::offscreen);
    }

    FrameOutcome last_frame_outcome() const override { return last_outcome_; }

    bool has_presentable_target() const override { return gpu_.has_surface(); }

    void resize(uint32_t width, uint32_t height, float scale) override {
        if (scale != scale_) {
            // Retained layers are rasterized at record-time device density.
            // Force callers to rebuild them at the new backing scale.
            canvas_.reset();
            retained_layers_ =
                canvas::SkiaCanvas::create_retained_layer_store();
        }
        width_ = width;
        height_ = height;
        scale_ = scale;
        create_offscreen_target();
        // A fresh scene target at the new size; the host's first post-resize
        // frame is a full repaint into it (partial repaint can't clip across a
        // size change).
        if (persistent_scene_) create_scene_target();
    }

    bool set_persistent_scene(bool enable) override {
        persistent_scene_ = enable;
        if (enable) {
            create_scene_target();
        } else {
            scene_surface_.reset();
        }
        return persistent_scene_ && scene_surface_ != nullptr;
    }

    bool persistent_scene() const override {
        return persistent_scene_ && scene_surface_ != nullptr;
    }

    bool read_current_rgba(std::vector<uint8_t>& pixels,
                           uint32_t& pixel_width,
                           uint32_t& pixel_height) override {
        // In persistent-scene mode the retained scene surface is the
        // authoritative frame content (frame_surface_ is only the transient
        // drawable wrapped for the blit, and is null mid-frame). Read the scene.
        SkSurface* source = nullptr;
        if (persistent_scene_ && scene_surface_)
            source = scene_surface_.get();
        else if (frame_surface_)
            source = frame_surface_.get();
        else
            source = offscreen_surface_.get();
        if (!source) return false;

        // Callers such as MacGpuWindowHost::capture_back_buffer_png() read
        // during the current frame, before SkiaSurface::end_frame(). Graphite
        // has not submitted the recording yet in that state, so an async
        // readback can see a valid but cleared texture. Submit pending canvas
        // work first while keeping frame_surface_ alive for the readback; the
        // later end_frame() call will simply find no additional recording.
        if (canvas_ && recorder_ && context_) {
            canvas_.reset();
            auto recording = recorder_->snap();
            if (recording) {
                skgpu::graphite::InsertRecordingInfo info;
                info.fRecording = recording.get();
                context_->insertRecording(info);
                context_->submit({});
                // end_frame()'s second snap() will legitimately find nothing
                // left to record; remember that this frame's work DID reach the
                // GPU so it is not misreported as a failed frame.
                frame_work_submitted_ = true;
            }
        }

        pixel_width = static_cast<uint32_t>(std::max(1, static_cast<int>(width_ * scale_)));
        pixel_height = static_cast<uint32_t>(std::max(1, static_cast<int>(height_ * scale_)));

        auto info = SkImageInfo::Make(static_cast<int>(pixel_width),
                                      static_cast<int>(pixel_height),
                                      kRGBA_8888_SkColorType,
                                      kPremul_SkAlphaType,
                                      SkColorSpace::MakeSRGB());
        const auto row_bytes = static_cast<size_t>(pixel_width) * 4u;

        struct ReadbackState {
            size_t row_bytes = 0;
            uint32_t height = 0;
            std::vector<uint8_t> pixels;
            std::atomic_bool finished{false};
            bool ok = false;
        };
        auto state = std::make_shared<ReadbackState>();
        state->row_bytes = row_bytes;
        state->height = pixel_height;

        auto callback = [](SkImage::ReadPixelsContext ctx,
                           std::unique_ptr<const SkImage::AsyncReadResult> result) {
            std::unique_ptr<std::shared_ptr<ReadbackState>> owned(
                static_cast<std::shared_ptr<ReadbackState>*>(ctx));
            auto state = owned ? *owned : nullptr;
            if (!state) return;
            if (!result || result->count() < 1 || result->data(0) == nullptr ||
                result->rowBytes(0) < state->row_bytes) {
                state->finished.store(true, std::memory_order_release);
                return;
            }

            const auto* src = static_cast<const uint8_t*>(result->data(0));
            const auto src_row_bytes = result->rowBytes(0);
            state->pixels.resize(static_cast<size_t>(state->height) * state->row_bytes);
            for (uint32_t y = 0; y < state->height; ++y) {
                std::memcpy(state->pixels.data() + static_cast<size_t>(y) * state->row_bytes,
                            src + static_cast<size_t>(y) * src_row_bytes,
                            state->row_bytes);
            }
            state->ok = true;
            state->finished.store(true, std::memory_order_release);
        };

        if (context_) {
            // The callback owns this heap context. On a timeout, do not free it:
            // Graphite may still deliver a late callback, and a tiny leaked
            // shared_ptr is safer than reintroducing a callback use-after-free.
            auto* callback_state = new std::shared_ptr<ReadbackState>(state);
            context_->asyncRescaleAndReadPixels(source,
                                                info,
                                                SkIRect::MakeWH(static_cast<int>(pixel_width),
                                                                static_cast<int>(pixel_height)),
                                                SkImage::RescaleGamma::kSrc,
                                                SkImage::RescaleMode::kNearest,
                                                callback,
                                                callback_state);
            context_->submit(skgpu::graphite::SyncToCpu::kYes);

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!state->finished.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline) {
                context_->checkAsyncWorkCompletion();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (state->finished.load(std::memory_order_acquire) && state->ok) {
                pixels = std::move(state->pixels);
                return true;
            }
            if (!state->finished.load(std::memory_order_acquire)) {
                runtime::log_warn(
                    "SkiaSurface: GPU readback timed out; falling back to synchronous readPixels");
            }
        }

        pixels.resize(static_cast<size_t>(pixel_width) * static_cast<size_t>(pixel_height) * 4u);
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

    double gpu_render_time_ms() const override {
        return gpu_render_tracker_.last_ms();
    }

    bool gpu_render_timing_available() const override {
        return gpu_elapsed_supported_;
    }

private:
    // Graphite finished-with-stats callback. Fires on the thread pumping GPU
    // completion (render thread, via Context::submit) once the GPU finishes
    // the recording. Stores the elapsed time (ns) into the cross-thread
    // tracker; treats a failed callback or zero elapsed as "no sample" so the
    // last good value is retained.
    static void on_gpu_stats(skgpu::graphite::GpuFinishedContext ctx,
                             skgpu::CallbackResult result,
                             const skgpu::GpuStats& stats) {
        auto* self = static_cast<SkiaSurfaceImpl*>(ctx);
        if (!self) return;
        self->gpu_render_tracker_.store(
            stats.elapsedTime, result == skgpu::CallbackResult::kSuccess);
    }

    GpuSurface& gpu_;
    uint32_t width_ = 0, height_ = 0;
    float scale_ = 1.0f;

    // GPU render time via Graphite GpuStats.
    bool gpu_elapsed_supported_ = false;  ///< Set once in init() from supportedGpuStats().
    GpuRenderTimeTracker gpu_render_tracker_;  ///< Latest sample; written by on_gpu_stats().

    std::unique_ptr<skgpu::graphite::Context> context_;
    std::unique_ptr<skgpu::graphite::Recorder> recorder_;
    // Uploads raster images Graphite would otherwise drop. The Recorder holds
    // its own ref, so declaration order alone does not guarantee the cached
    // textures are released before the Recorder that provisioned them — the
    // destructor clears the cache explicitly.
    sk_sp<GraphiteImageProvider> image_provider_;

    // Per-frame surface wrapping the current presentable texture
    sk_sp<SkSurface> frame_surface_;

    // Offscreen fallback (used when no native surface is attached)
    sk_sp<SkSurface> offscreen_surface_;

    // Persistent-scene target (FU-2). When persistent_scene_ is on, the frame is
    // drawn here and RETAINED across frames; end_frame blits it to the drawable.
    bool persistent_scene_ = false;
    sk_sp<SkSurface> scene_surface_;

    // Cacheable layer textures belong to the renderer, not the short-lived
    // SkiaCanvas wrapper constructed around each frame's target surface.
    std::shared_ptr<canvas::SkiaCanvas::RetainedLayerStore> retained_layers_ =
        canvas::SkiaCanvas::create_retained_layer_store();

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

    // Create the retained scene surface at physical size. Like the offscreen
    // target, the backing-scale CTM is applied once here so the host paints in
    // logical coordinates. Cleared so a (defensive) partial first frame does not
    // reveal undefined texture memory; the host paints a full first frame.
    void create_scene_target() {
        if (!recorder_) return;

        int pixel_w = static_cast<int>(width_ * scale_);
        int pixel_h = static_cast<int>(height_ * scale_);

        SkImageInfo info = SkImageInfo::MakeN32Premul(pixel_w, pixel_h);
        scene_surface_ = SkSurfaces::RenderTarget(recorder_.get(), info);
        if (!scene_surface_) return;

        SkCanvas* c = scene_surface_->getCanvas();
        c->clear(SK_ColorTRANSPARENT);
        if (scale_ != 1.0f) c->scale(scale_, scale_);
    }

    // Wrap the current presentable drawable and record a 1:1 device-pixel blit
    // of the retained scene onto it (into the shared recorder). Both surfaces
    // are physical size, so the blit is an identity-CTM drawImageRect — no
    // scale (the drawable wrap starts with an identity CTM). Uses drawImageRect
    // (a GPU composite), NOT makeShader, so the ensure_gpu_image() image-shader
    // gotcha is not in play. Offscreen (no presentable surface) is a no-op that
    // reports SUCCESS: the scene is itself the readback target, so there is
    // nothing to blit and nothing has gone wrong.
    //
    // Returns false when the scene did not reach the drawable, so end_frame()
    // can tell the host the frame is not on screen.
    bool blit_scene_to_drawable() {
        if (!gpu_.has_surface()) return true;

        auto* texture_ptr = static_cast<wgpu::Texture*>(gpu_.current_texture_handle());
        if (!texture_ptr || !*texture_ptr) {
            runtime::log_warn("SkiaSurface: no current texture for persistent-scene blit");
            return false;
        }
        WGPUTexture raw_texture = texture_ptr->Get();
        skgpu::graphite::BackendTexture backend_tex =
            skgpu::graphite::BackendTextures::MakeDawn(raw_texture);
        if (!backend_tex.isValid()) {
            runtime::log_warn("SkiaSurface: invalid drawable texture for persistent-scene blit");
            return false;
        }
        // Same contract as begin_frame(): the color type must come from the
        // texture, not from the platform.
        const wgpu::TextureFormat surface_format = texture_ptr->GetFormat();
        const SkColorType surface_color_type = sk_color_type_for(surface_format);
        if (surface_color_type == kUnknown_SkColorType) {
            runtime::log_error(
                "SkiaSurface: persistent-scene blit target format {} has no "
                "SkColorType mapping — nothing will be presented.",
                wgpu_texture_format_name(surface_format));
            return false;
        }
        frame_surface_ = SkSurfaces::WrapBackendTexture(
            recorder_.get(), backend_tex, surface_color_type,
            SkColorSpace::MakeSRGB(), nullptr);
        if (!frame_surface_) {
            runtime::log_error(
                "SkiaSurface: WrapBackendTexture failed for persistent-scene "
                "blit (format {}) — nothing will be presented.",
                wgpu_texture_format_name(surface_format));
            return false;
        }

        sk_sp<SkImage> snapshot = scene_surface_->makeImageSnapshot();
        if (!snapshot) {
            runtime::log_error(
                "SkiaSurface: retained scene produced no snapshot — nothing "
                "will be presented.");
            return false;
        }

        const SkRect full = SkRect::MakeWH(static_cast<float>(snapshot->width()),
                                           static_cast<float>(snapshot->height()));
        frame_surface_->getCanvas()->drawImageRect(
            snapshot, full, full, SkSamplingOptions(), nullptr,
            SkCanvas::kStrict_SrcRectConstraint);
        return true;
    }

    FrameOutcome finish_frame(FrameOutcome outcome) {
        last_outcome_ = outcome;
        return outcome;
    }

    /// Outcome of the frame in flight. begin_frame() seeds it so a caller that
    /// got a null canvas can still ask what went wrong.
    FrameOutcome last_outcome_ = FrameOutcome::offscreen;

    /// True once this frame's recording has reached the GPU — set by
    /// read_current_rgba()'s mid-frame flush as well as by end_frame(), and
    /// cleared at end_frame(). Without it, a captured frame looks like a frame
    /// that produced no work at all.
    bool frame_work_submitted_ = false;
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
