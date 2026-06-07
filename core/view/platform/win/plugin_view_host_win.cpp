// Windows (HWND) PluginViewHost — native child-window editor host for the
// foreign-host embed SDK + the VST3/CLAP adapters on Windows (#3329 Win/Linux
// parity). Mirrors the macOS MacGpuPluginViewHost shape (NSView -> child HWND,
// CAMetalLayer -> HWND-backed Dawn surface).
//
// Two render paths share one class:
//   - GPU: a Dawn surface created from the child HWND, wrapped by SkiaSurface,
//     painted on demand (repaint()/WM_PAINT). Requires a working WebGPU backend
//     (D3D12/Vulkan). On a GPU-less host the Dawn init fails and is_gpu_backed()
//     reports false; the host still attaches and serves deterministic frames
//     via the Skia raster path in capture_back_buffer_png().
//   - Headless capture: capture_back_buffer_png() always works (raster), so the
//     embed's hidden-window smoke can verify a real non-black frame even with no
//     GPU — the VM-verifiable proof.
//
// Threading: all window ops run on the calling (UI) thread. There is no internal
// message loop — the host that owns the parent HWND pumps messages; repaint()
// invalidates and a WM_PAINT triggers render_frame(). For embed callers that
// drive frames explicitly (pulp_embed_tick), repaint() renders synchronously.

#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/window_host.hpp>

#ifdef PULP_HAS_SKIA
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>
#include <pulp/canvas/skia_canvas.hpp>
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"
#include "include/encode/SkPngEncoder.h"
#endif

#include <pulp/runtime/log.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace pulp::view {

namespace {

constexpr const wchar_t* kChildClassName = L"PulpPluginViewHostChild";

// Register the child window class once per process.
ATOM ensure_window_class() {
    static std::once_flag once;
    static ATOM atom = 0;
    std::call_once(once, [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        // CS_OWNDC: give the child its own device context (matches a GPU surface
        // backing). CS_HREDRAW/VREDRAW: repaint on resize.
        wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = DefWindowProcW;  // we drive paint explicitly via render_frame
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kChildClassName;
        atom = RegisterClassExW(&wc);
        if (!atom) {
            runtime::log_warn("WinPluginViewHost: RegisterClassExW failed");
        }
    });
    return atom;
}

class WinPluginViewHost : public PluginViewHost {
public:
    WinPluginViewHost(View& root, Size size) : root_(root), size_(size) {
        ensure_window_class();
        // Create a hidden child window not yet parented. It is reparented to the
        // DAW's editor HWND in attach_to_parent(). WS_CHILD without a parent is
        // legal at creation; SetParent fixes it up on attach.
        hwnd_ = CreateWindowExW(
            0, kChildClassName, L"", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
            0, 0, static_cast<int>(size.width), static_cast<int>(size.height),
            /*parent*/ nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!hwnd_) {
            runtime::log_warn("WinPluginViewHost: CreateWindowExW failed");
            return;
        }
#ifdef PULP_HAS_SKIA
        init_gpu(static_cast<float>(size.width), static_cast<float>(size.height));
#endif
    }

    ~WinPluginViewHost() override {
        root_.set_plugin_view_host(nullptr);
#ifdef PULP_HAS_SKIA
        skia_surface_.reset();
        gpu_surface_.reset();
#endif
        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

    NativeViewHandle native_handle() override { return static_cast<void*>(hwnd_); }

    void attach_to_parent(NativeViewHandle parent) override {
        if (!hwnd_) return;
        HWND parent_hwnd = static_cast<HWND>(parent);
        if (!parent_hwnd) return;
        // Re-parent and ensure WS_CHILD style is set (SetParent on a top-level
        // window does not implicitly add it).
        LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_STYLE);
        SetWindowLongPtrW(hwnd_, GWL_STYLE, style | WS_CHILD);
        SetParent(hwnd_, parent_hwnd);
        SetWindowPos(hwnd_, nullptr, 0, 0,
                     static_cast<int>(size_.width), static_cast<int>(size_.height),
                     SWP_NOZORDER | SWP_SHOWWINDOW);
        ShowWindow(hwnd_, SW_SHOW);
        attached_.store(true, std::memory_order_release);
        repaint();
    }

    bool is_attached() const noexcept {
        if (!hwnd_) return false;
        // Authoritative check: do we currently have a parent? (Survives a host
        // that detaches us behind our back.)
        return GetParent(hwnd_) != nullptr &&
               attached_.load(std::memory_order_acquire);
    }

    void detach() override {
        if (!hwnd_) return;
        ShowWindow(hwnd_, SW_HIDE);
        SetParent(hwnd_, nullptr);
        attached_.store(false, std::memory_order_release);
    }

    void repaint() override {
#ifdef PULP_HAS_SKIA
        if (gpu_surface_ && skia_surface_) {
            render_frame(nullptr, nullptr, nullptr);
            return;
        }
#endif
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void set_size(uint32_t width, uint32_t height) override {
        size_ = {width, height};
        if (hwnd_) {
            SetWindowPos(hwnd_, nullptr, 0, 0, static_cast<int>(width),
                         static_cast<int>(height), SWP_NOZORDER | SWP_NOMOVE);
        }
#ifdef PULP_HAS_SKIA
        if (gpu_surface_) gpu_surface_->resize(width, height);
        if (skia_surface_) skia_surface_->resize(width, height, 1.0f);
#endif
        repaint();
    }

    Size get_size() const override { return size_; }

    bool is_gpu_backed() const override {
#ifdef PULP_HAS_SKIA
        return gpu_surface_ != nullptr && skia_surface_ != nullptr &&
               skia_surface_->is_available();
#else
        return false;
#endif
    }

#ifdef PULP_HAS_SKIA
    render::GpuSurface* gpu_surface() const override { return gpu_surface_.get(); }
#endif

    void set_idle_callback(std::function<void()> cb) override {
        idle_callback_ = std::move(cb);
    }

    void set_resize_callback(std::function<void(uint32_t, uint32_t)> cb) override {
        resize_cb_ = std::move(cb);
    }

    // Deterministic capture. Prefers the GPU back-buffer readback when the GPU
    // path is live; otherwise falls back to a pure Skia raster render so a
    // GPU-less host (CI VM) still produces a real, non-black frame.
    std::vector<uint8_t> capture_back_buffer_png() override {
#ifdef PULP_HAS_SKIA
        if (gpu_surface_ && skia_surface_) {
            std::vector<uint8_t> pixels;
            uint32_t pw = 0, ph = 0;
            if (render_frame(&pixels, &pw, &ph) && !pixels.empty())
                return encode_rgba_png(pixels, pw, ph);
        }
        return raster_capture_png();
#else
        return {};
#endif
    }

    void set_design_viewport(float design_w, float design_h) override {
        design_viewport_w_ = design_w;
        design_viewport_h_ = design_h;
        repaint();
    }

    void set_design_viewport_top_align(bool top_align) override {
        design_top_align_ = top_align;
        repaint();
    }

    void set_fixed_aspect_ratio(float ratio) override { fixed_aspect_ratio_ = ratio; }

    Point window_to_root_point(Point pt) const override {
        float sx, sy, tx, ty;
        if (!WindowHost::compute_design_viewport_transform(
                static_cast<float>(size_.width), static_cast<float>(size_.height),
                design_viewport_w_, design_viewport_h_, sx, sy, tx, ty,
                design_top_align_)) {
            return pt;
        }
        if (sx <= 0.0f || sy <= 0.0f) return pt;
        return {(pt.x - tx) / sx, (pt.y - ty) / sy};
    }

private:
    View& root_;
    Size size_;
    HWND hwnd_ = nullptr;
    std::atomic<bool> attached_{false};
    std::function<void()> idle_callback_;
    std::function<void(uint32_t, uint32_t)> resize_cb_;
    float design_viewport_w_ = 0.0f;
    float design_viewport_h_ = 0.0f;
    float fixed_aspect_ratio_ = 0.0f;
    bool design_top_align_ = false;

#ifdef PULP_HAS_SKIA
    std::unique_ptr<render::GpuSurface> gpu_surface_;
    std::unique_ptr<render::SkiaSurface> skia_surface_;

    void init_gpu(float width, float height) {
        gpu_surface_ = render::GpuSurface::create_dawn();
        if (!gpu_surface_) {
            runtime::log_warn("WinPluginViewHost: gpu create_dawn failed; cpu-capture only");
            return;
        }
        render::GpuSurface::Config cfg{};
        cfg.width = static_cast<uint32_t>(width);
        cfg.height = static_cast<uint32_t>(height);
        cfg.native_surface_handle = static_cast<void*>(hwnd_);  // HWND
        if (!gpu_surface_->initialize(cfg)) {
            runtime::log_warn("WinPluginViewHost: gpu initialize failed; cpu-capture only");
            gpu_surface_.reset();
            return;
        }
        render::SkiaSurface::Config scfg{};
        scfg.width = static_cast<uint32_t>(width);
        scfg.height = static_cast<uint32_t>(height);
        scfg.scale_factor = 1.0f;
        skia_surface_ = render::SkiaSurface::create(*gpu_surface_, scfg);
        if (!skia_surface_) {
            runtime::log_warn("WinPluginViewHost: skia surface create failed; cpu-capture only");
            gpu_surface_.reset();
        }
    }

    // Shared scene paint (matches the macOS plugin GPU host).
    void paint_scene(canvas::Canvas& canvas) {
        const float w = static_cast<float>(size_.width);
        const float h = static_cast<float>(size_.height);
        canvas.set_fill_color(pulp::canvas::Color::rgba8(30, 30, 46));
        canvas.fill_rect(0, 0, w, h);

        float sx, sy, tx, ty;
        const bool has_viewport =
            design_viewport_w_ > 0.0f && design_viewport_h_ > 0.0f &&
            WindowHost::compute_design_viewport_transform(
                w, h, design_viewport_w_, design_viewport_h_, sx, sy, tx, ty,
                design_top_align_);
        if (has_viewport) {
            root_.set_bounds({0, 0, design_viewport_w_, design_viewport_h_});
            root_.layout_children();
            const int saved = canvas.save_count();
            canvas.save();
            canvas.translate(tx, ty);
            canvas.scale(sx, sy);
            root_.paint_all(canvas);
            View::paint_overlays(canvas, &root_);
            canvas.restore_to_count(saved);
        } else {
            root_.set_bounds({0, 0, w, h});
            root_.layout_children();
            root_.paint_all(canvas);
            View::paint_overlays(canvas, &root_);
        }
    }

    bool render_frame(std::vector<uint8_t>* cap, uint32_t* cap_w, uint32_t* cap_h) {
        if (!gpu_surface_ || !skia_surface_) return false;
        if (idle_callback_) idle_callback_();
        if (!gpu_surface_->begin_frame()) return false;
        auto* canvas = skia_surface_->begin_frame();
        if (!canvas) {
            gpu_surface_->end_frame();
            return false;
        }
        paint_scene(*canvas);
        if (cap) {
            // read_current_rgba finalizes + submits the open frame's recording
            // before readback (see SkiaSurface contract), so no separate flush.
            uint32_t pw = 0, ph = 0;
            skia_surface_->read_current_rgba(*cap, pw, ph);
            if (cap_w) *cap_w = pw;
            if (cap_h) *cap_h = ph;
        }
        skia_surface_->end_frame();
        gpu_surface_->end_frame();
        return true;
    }

    // Pure-CPU raster capture, GPU-independent — the VM proof path.
    std::vector<uint8_t> raster_capture_png() {
        const uint32_t w = size_.width, h = size_.height;
        if (w == 0 || h == 0) return {};
        auto cs = SkColorSpace::MakeSRGB();
        SkImageInfo info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType,
                                             kPremul_SkAlphaType, cs);
        auto surface = SkSurfaces::Raster(info);
        if (!surface) return {};
        auto* sk_canvas = surface->getCanvas();
        if (!sk_canvas) return {};
        pulp::canvas::SkiaCanvas canvas(sk_canvas);
        paint_scene(canvas);
        std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4u);
        SkPixmap pixmap(info, pixels.data(), static_cast<size_t>(w) * 4u);
        if (!surface->readPixels(pixmap, 0, 0)) return {};
        return encode_rgba_png(pixels, w, h);
    }

    static std::vector<uint8_t> encode_rgba_png(const std::vector<uint8_t>& rgba,
                                                uint32_t w, uint32_t h) {
        if (rgba.empty() || w == 0 || h == 0) return {};
        SkImageInfo info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType,
                                             kPremul_SkAlphaType,
                                             SkColorSpace::MakeSRGB());
        SkPixmap pixmap(info, rgba.data(), static_cast<size_t>(w) * 4u);
        // 2-arg pixmap overload returns sk_sp<SkData> (the 3-arg SkWStream*
        // overload returns bool).
        sk_sp<SkData> png = SkPngEncoder::Encode(pixmap, SkPngEncoder::Options{});
        if (!png || png->isEmpty()) return {};
        const auto* p = static_cast<const uint8_t*>(png->data());
        return std::vector<uint8_t>(p, p + png->size());
    }
#endif  // PULP_HAS_SKIA
};

}  // namespace

// Strong definition of the registration hook (the stub's no-op default is
// compiled out via PULP_HAS_PLATFORM_PLUGIN_VIEW_HOST). Idempotent: installs the
// HWND factory only if no factory is already registered, so a host that called
// set_factory() first keeps control.
void register_platform_plugin_view_host() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (PluginViewHost::has_factory()) return;
        PluginViewHost::set_factory(
            [](View& root, const PluginViewHost::Options& opts)
                -> std::unique_ptr<PluginViewHost> {
                return std::make_unique<WinPluginViewHost>(root, opts.size);
            });
    });
}

}  // namespace pulp::view
