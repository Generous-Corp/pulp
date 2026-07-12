// SkiaSurface backed by Ganesh on WebGL2 (browser only).
//
// The Graphite/Dawn implementation (skia_surface.cpp) needs a GpuSurface for
// its Dawn device/queue/instance handles. WebGL2 has no such object — the
// GL context IS the device — so this backend owns its context outright and
// takes no GpuSurface. Two consequences the rest of the stack must honor:
//
//   * No presentation step. The browser composites the <canvas> element after
//     the current task yields, so end_frame() only submits.
//   * No GPU render timing. Graphite's kElapsedTime stat is backed by Dawn
//     timestamp queries; WebGL2 has no equivalent, so timing reports
//     unavailable rather than a fabricated number, and no finished-with-stats
//     callback is ever armed (which is why this destructor needs no drain).
//   * The context can be LOST at any time, and unlike Dawn there is no
//     device-lost future to await. A page that crosses the browser's live-WebGL
//     context cap (Chrome retires the oldest at ~16), a GPU-process crash, or a
//     backgrounded tab on a mobile GPU all kill the context under us. Nothing in
//     the GL API reports it: emscripten_webgl_make_context_current() still
//     succeeds on a dead handle and WrapBackendRenderTarget still hands back an
//     SkSurface, so every frame would "succeed" into the void and the UI would
//     freeze silently. So this backend registers the webglcontextlost /
//     webglcontextrestored callbacks, abandons the Ganesh context on loss (which
//     is what makes is_available() an honest liveness check rather than a
//     never-false constant), refuses to hand out a canvas while lost, and
//     rebuilds the Ganesh context on restore.
//
// This file is the UI surface. No GPU-compute (GPU-audio) code compiles to wasm
// anywhere in this build: WebGL2 has no compute shaders, and Pulp's GpuCompute
// needs WebGPU (emdawnwebgpu), which is a separate, Skia-free module that does
// not exist yet.

#include <pulp/render/skia_surface.hpp>

#if defined(__EMSCRIPTEN__) && defined(PULP_HAS_SKIA)

#include <pulp/canvas/skia_canvas.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/runtime/trace.hpp>

#include <emscripten/html5.h>
#include <emscripten/html5_webgl.h>
#include <GLES3/gl3.h>

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"
#include "include/gpu/GpuTypes.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"
#include "include/gpu/ganesh/gl/GrGLMakeWebGLInterface.h"
#include "include/gpu/ganesh/gl/GrGLTypes.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>

namespace pulp::render {

namespace {

// Stencil bits requested from the WebGL context and declared on the wrapped
// default framebuffer. Skia's clip stack needs a stencil attachment; the two
// values must agree or WrapBackendRenderTarget rejects the target.
constexpr int kStencilBits = 8;

} // namespace

class SkiaSurfaceGanesh : public SkiaSurface {
public:
    SkiaSurfaceGanesh(std::string selector, uint32_t width, uint32_t height, float scale)
        : selector_(std::move(selector)), width_(width), height_(height), scale_(scale) {}

    ~SkiaSurfaceGanesh() override {
        // Unregister FIRST: a lost/restored event delivered after this object is
        // gone would call back into freed memory.
        emscripten_set_webglcontextlost_callback(selector_.c_str(), nullptr, EM_FALSE, nullptr);
        emscripten_set_webglcontextrestored_callback(selector_.c_str(), nullptr, EM_FALSE, nullptr);

        canvas_.reset();
        frame_surface_.reset();
        offscreen_surface_.reset();
        if (context_) {
            if (!lost_) context_->flushAndSubmit();
            context_->releaseResourcesAndAbandonContext();
            context_.reset();
        }
        if (gl_context_) {
            emscripten_webgl_destroy_context(gl_context_);
            gl_context_ = 0;
        }
    }

    bool init() {
        EmscriptenWebGLContextAttributes attrs{};
        emscripten_webgl_init_context_attributes(&attrs);
        attrs.majorVersion = 2;  // WebGL2 — WebGL1 lacks the GLES3 feature set Skia expects.
        attrs.minorVersion = 0;
        attrs.alpha = EM_TRUE;
        attrs.depth = EM_FALSE;
        attrs.stencil = EM_TRUE;
        attrs.antialias = EM_FALSE;  // Skia does its own AA; MSAA on the default FB just costs.
        // read_current_rgba() reads back the default framebuffer outside the
        // rAF callback that drew it, which the browser is otherwise free to
        // have already cleared.
        attrs.preserveDrawingBuffer = EM_TRUE;
        attrs.premultipliedAlpha = EM_TRUE;

        gl_context_ = emscripten_webgl_create_context(selector_.c_str(), &attrs);
        if (gl_context_ <= 0) {
            runtime::log_error("SkiaSurface(WebGL): failed to create a WebGL2 context for '{}'",
                               selector_);
            return false;
        }
        if (emscripten_webgl_make_context_current(gl_context_) != EMSCRIPTEN_RESULT_SUCCESS) {
            runtime::log_error("SkiaSurface(WebGL): failed to make the WebGL2 context current");
            return false;
        }

        context_ = GrDirectContexts::MakeGL(GrGLInterfaces::MakeWebGL());
        if (!context_) {
            runtime::log_error("SkiaSurface(WebGL): failed to create a Ganesh GL context");
            return false;
        }

        apply_canvas_size();
        create_offscreen_target();

        // A WebGL context is revocable (see the file header). Without these the
        // surface would keep "rendering" into a dead context forever.
        emscripten_set_webglcontextlost_callback(selector_.c_str(), this, EM_FALSE,
                                                 &SkiaSurfaceGanesh::context_lost_cb);
        emscripten_set_webglcontextrestored_callback(selector_.c_str(), this, EM_FALSE,
                                                     &SkiaSurfaceGanesh::context_restored_cb);

        runtime::log_info("SkiaSurface(WebGL): Ganesh initialized on WebGL2 ('{}', {}x{} @{}x)",
                          selector_, width_, height_, scale_);
        return true;
    }

    // Driven by the browser's webglcontextlost / webglcontextrestored events
    // (registered in init()). The browser fixture exercises this for real: it
    // revokes the context through WEBGL_lose_context and asserts that
    // is_available() goes false and then recovers — no simulated event, no test
    // hook.
    void handle_context_lost() {
        if (lost_) return;
        lost_ = true;
        runtime::log_warn("SkiaSurface(WebGL): the WebGL2 context was LOST — the UI is "
                          "frozen until the browser restores it.");
        canvas_.reset();
        frame_surface_.reset();
        offscreen_surface_.reset();
        if (context_) {
            // Do NOT flush: the GL context is dead and every call is a no-op or
            // an error. Abandon so Skia stops trying to talk to it and so
            // is_available() reports the truth.
            context_->releaseResourcesAndAbandonContext();
            context_.reset();
        }
    }

    void handle_context_restored() {
        if (!lost_) return;
        // The browser revives the SAME WebGL context object; all GPU resources
        // are gone, so the Ganesh context has to be rebuilt from scratch.
        if (emscripten_webgl_make_context_current(gl_context_) != EMSCRIPTEN_RESULT_SUCCESS) {
            runtime::log_error("SkiaSurface(WebGL): context restored but could not be made "
                               "current — staying unavailable.");
            return;
        }
        context_ = GrDirectContexts::MakeGL(GrGLInterfaces::MakeWebGL());
        if (!context_) {
            runtime::log_error("SkiaSurface(WebGL): context restored but Ganesh could not be "
                               "rebuilt — staying unavailable.");
            return;
        }
        lost_ = false;
        apply_canvas_size();
        create_offscreen_target();
        runtime::log_info("SkiaSurface(WebGL): the WebGL2 context was RESTORED — Ganesh rebuilt.");
    }

    canvas::Canvas* begin_frame() override {
        // A lost context is not a renderable one. Returning nullptr keeps the
        // host's dirty flag set (window_host_web's render_frame() only clears it
        // on a real frame), so the UI repaints as soon as the context is back.
        if (lost_ || !context_) return nullptr;
        if (emscripten_webgl_make_context_current(gl_context_) != EMSCRIPTEN_RESULT_SUCCESS) {
            return nullptr;
        }

        frame_surface_.reset();

        GrGLFramebufferInfo fb_info{};
        fb_info.fFBOID = 0;  // the canvas' default framebuffer
        fb_info.fFormat = GL_RGBA8;

        GrBackendRenderTarget target = GrBackendRenderTargets::MakeGL(
            static_cast<int>(pixel_width()),
            static_cast<int>(pixel_height()),
            /*sampleCnt*/ 0,
            kStencilBits,
            fb_info);

        frame_surface_ = SkSurfaces::WrapBackendRenderTarget(
            context_.get(),
            target,
            kBottomLeft_GrSurfaceOrigin,  // GL's origin for the default framebuffer
            kRGBA_8888_SkColorType,
            SkColorSpace::MakeSRGB(),
            nullptr);

        SkCanvas* sk_canvas = nullptr;
        if (frame_surface_) {
            // The wrapped render target is rebuilt every frame, so its canvas
            // starts with an identity matrix and takes the HiDPI scale here.
            sk_canvas = frame_surface_->getCanvas();
            if (sk_canvas && scale_ != 1.0f) {
                sk_canvas->scale(scale_, scale_);
            }
        } else {
            runtime::log_warn("SkiaSurface(WebGL): WrapBackendRenderTarget failed — "
                              "falling back to the offscreen target");
            // The offscreen canvas is long-lived; create_offscreen_target()
            // already baked the scale into it.
            sk_canvas = offscreen_surface_ ? offscreen_surface_->getCanvas() : nullptr;
        }
        if (!sk_canvas) return nullptr;

        canvas_ = std::make_unique<canvas::SkiaCanvas>(sk_canvas, /*recorder*/ nullptr);
        canvas_->set_gpu_upload_context(context_.get());
        return canvas_.get();
    }

    void end_frame() override {
        PULP_TRACE_SCOPE_NAMED("gpu", "gpu_submit");
        canvas_.reset();
        if (lost_ || !context_) return;
        // No present call: the browser composites the canvas element itself.
        context_->flushAndSubmit();
    }

    void resize(uint32_t width, uint32_t height, float scale) override {
        width_ = width;
        height_ = height;
        scale_ = scale;
        frame_surface_.reset();  // rewrapped at the new size on the next begin_frame()
        if (lost_) return;       // the canvas is resized again on restore
        apply_canvas_size();
        create_offscreen_target();
    }

    bool read_current_rgba(std::vector<uint8_t>& pixels,
                           uint32_t& pixel_w,
                           uint32_t& pixel_h) override {
        if (lost_) return false;   // no honest pixels exist; never hand back a stale buffer
        SkSurface* source = frame_surface_ ? frame_surface_.get() : offscreen_surface_.get();
        if (!source || !context_) return false;

        // Flush any canvas work still in flight so the readback sees this
        // frame. Under Ganesh readPixels is a synchronous glReadPixels — there
        // is deliberately no async/spin path here: a spin loop in wasm starves
        // the JS event loop that would have to resolve it.
        canvas_.reset();
        context_->flushAndSubmit();

        pixel_w = pixel_width();
        pixel_h = pixel_height();

        auto info = SkImageInfo::Make(static_cast<int>(pixel_w),
                                      static_cast<int>(pixel_h),
                                      kRGBA_8888_SkColorType,
                                      kPremul_SkAlphaType,
                                      SkColorSpace::MakeSRGB());
        const auto row_bytes = static_cast<size_t>(pixel_w) * 4u;
        pixels.resize(static_cast<size_t>(pixel_h) * row_bytes);

        SkPixmap pixmap(info, pixels.data(), row_bytes);
        auto image = source->makeImageSnapshot();
        if (image && image->readPixels(context_.get(), pixmap, 0, 0)) {
            return true;
        }
        return source->readPixels(pixmap, 0, 0);
    }

    // An honest liveness check: false while the WebGL context is lost (see the
    // file header) and true again once it has been restored and Ganesh rebuilt.
    bool is_available() const override { return context_ != nullptr && !lost_; }

    skgpu::graphite::Context* graphite_context() const override { return nullptr; }

    // WebGL2 exposes no timestamp queries, so there is no honest number to
    // report. The inspector renders "GPU timing unavailable".
    double gpu_render_time_ms() const override { return 0.0; }
    bool gpu_render_timing_available() const override { return false; }

private:
    // Returning EM_TRUE calls preventDefault() on the webglcontextlost event,
    // which is what tells the browser we WANT the context back (without it, no
    // webglcontextrestored event is ever fired).
    static EM_BOOL context_lost_cb(int, const void*, void* user_data) {
        static_cast<SkiaSurfaceGanesh*>(user_data)->handle_context_lost();
        return EM_TRUE;
    }
    static EM_BOOL context_restored_cb(int, const void*, void* user_data) {
        static_cast<SkiaSurfaceGanesh*>(user_data)->handle_context_restored();
        return EM_TRUE;
    }

    uint32_t pixel_width() const {
        return static_cast<uint32_t>(std::max(1, static_cast<int>(width_ * scale_)));
    }
    uint32_t pixel_height() const {
        return static_cast<uint32_t>(std::max(1, static_cast<int>(height_ * scale_)));
    }

    // The <canvas> backing store must be sized in device pixels; CSS sizing is
    // the page's job.
    void apply_canvas_size() {
        emscripten_set_canvas_element_size(selector_.c_str(),
                                           static_cast<int>(pixel_width()),
                                           static_cast<int>(pixel_height()));
    }

    void create_offscreen_target() {
        if (!context_) return;

        SkImageInfo info = SkImageInfo::MakeN32Premul(static_cast<int>(pixel_width()),
                                                      static_cast<int>(pixel_height()));
        offscreen_surface_ = SkSurfaces::RenderTarget(context_.get(), skgpu::Budgeted::kYes, info);

        if (offscreen_surface_ && scale_ != 1.0f) {
            offscreen_surface_->getCanvas()->scale(scale_, scale_);
        }
    }

    std::string selector_;
    uint32_t width_ = 0, height_ = 0;
    float scale_ = 1.0f;

    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE gl_context_ = 0;
    sk_sp<GrDirectContext> context_;
    bool lost_ = false;   // true between webglcontextlost and webglcontextrestored

    sk_sp<SkSurface> frame_surface_;
    sk_sp<SkSurface> offscreen_surface_;

    std::unique_ptr<canvas::SkiaCanvas> canvas_;
};

std::unique_ptr<SkiaSurface> SkiaSurface::create_webgl(const WebGlConfig& config) {
    auto surface = std::make_unique<SkiaSurfaceGanesh>(
        config.canvas_selector ? config.canvas_selector : "#pulp",
        config.width,
        config.height,
        config.scale_factor);
    if (!surface->init()) return nullptr;
    return surface;
}

} // namespace pulp::render

#elif defined(__EMSCRIPTEN__)

namespace pulp::render {
std::unique_ptr<SkiaSurface> SkiaSurface::create_webgl(const WebGlConfig&) {
    return nullptr;
}
} // namespace pulp::render

#endif
