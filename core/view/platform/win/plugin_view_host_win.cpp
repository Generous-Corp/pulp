// Windows (HWND) PluginViewHost — native child-window editor host for the
// foreign-host embed SDK + the VST3/CLAP adapters on Windows. Mirrors the
// macOS MacGpuPluginViewHost shape (NSView -> child HWND,
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
#include <pulp/view/pointer_dispatch.hpp>
#include <pulp/view/ui_components.hpp>  // ComboBox::notify_global_click
#include <pulp/view/platform/win_pointer_input.hpp>
#include <pulp/view/repaint_damage.hpp>  // compute_effective_damage (platform-free)
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
#include <pulp/runtime/trace.hpp>  // no-op unless PULP_TRACING=ON

// WIN32_LEAN_AND_MEAN + NOMINMAX, guarded, before <windows.h> so the min/max
// macros don't collide with std::min/std::max or Skia (#384).
#include <pulp/platform/win32_sane.hpp>

#include <ole2.h>        // OleInitialize, RegisterDragDrop, IDropTarget, DoDragDrop
#include <shellapi.h>    // DragQueryFileW, HDROP, CF_HDROP, DROPFILES
#include <shlobj.h>      // SHCreateStdEnumFmtEtc (outbound IDataObject enum)
#include "win_file_drag.hpp"  // shared OLE drag source (win_drag::win_run_file_drag)

#include <pulp/view/drag_drop.hpp>

#include <cmath>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pulp::view {

namespace {

// win_pointer_input.hpp duplicates the MK_* mouse bits as plain constants so
// the header parses off Windows. This TU is the one place both definitions are
// visible, so pin them together — a Windows SDK change becomes a compile error
// here instead of a silently wrong modifier at run time.
static_assert(win_input::kMkLButton == MK_LBUTTON);
static_assert(win_input::kMkRButton == MK_RBUTTON);
static_assert(win_input::kMkShift == MK_SHIFT);
static_assert(win_input::kMkControl == MK_CONTROL);

constexpr const wchar_t* kChildClassName = L"PulpPluginViewHostChild";

class WinPluginViewHost;

// RAII scope guard for a bool flag: sets it true for the scope and restores the
// previous value on exit. Used to suppress the WM_SIZE reconcile while we drive
// a SetWindowPos (which re-enters the wndproc synchronously). Restores (rather
// than clears) so a nested guard can't clear the flag out from under an outer
// one.
struct ScopedFlag {
    bool& flag;
    bool prev;
    explicit ScopedFlag(bool& f) : flag(f), prev(f) { flag = true; }
    ~ScopedFlag() { flag = prev; }
    ScopedFlag(const ScopedFlag&) = delete;
    ScopedFlag& operator=(const ScopedFlag&) = delete;
};

// WndProc: routes WM_PAINT to the host's render path. `this` is stashed in
// GWLP_USERDATA at WM_NCCREATE so ordinary host-driven invalidations
// (InvalidateRect) actually render, not just the synchronous repaint() path.
LRESULT CALLBACK pulp_pvh_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

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
        wc.lpfnWndProc = pulp_pvh_wndproc;
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

// UTF-16 → UTF-8 for dropped text / paths.
std::string wide_to_utf8(const wchar_t* w, int wlen) {
    if (!w || wlen <= 0) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, wlen, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, wlen, out.data(), n, nullptr, nullptr);
    return out;
}

// Pull a DropData out of an OLE IDataObject (files take priority over text,
// matching the SDL + macOS producers).
DropData extract_idata_drop(IDataObject* data) {
    DropData out;
    if (!data) return out;

    // CF_HDROP — a list of file paths.
    FORMATETC fmt_files{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM stg{};
    if (data->GetData(&fmt_files, &stg) == S_OK) {
        if (auto hdrop = static_cast<HDROP>(GlobalLock(stg.hGlobal))) {
            const UINT count = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
            out.type = DropData::Type::files;
            for (UINT i = 0; i < count; ++i) {
                const UINT len = DragQueryFileW(hdrop, i, nullptr, 0);
                std::wstring buf(len + 1, L'\0');
                const UINT got = DragQueryFileW(hdrop, i, buf.data(),
                                                static_cast<UINT>(buf.size()));
                if (got > 0) out.file_paths.push_back(wide_to_utf8(buf.data(),
                                                                   static_cast<int>(got)));
            }
            GlobalUnlock(stg.hGlobal);
        }
        ReleaseStgMedium(&stg);
        if (!out.file_paths.empty()) return out;
    }

    // CF_UNICODETEXT — a single text payload.
    FORMATETC fmt_text{CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM stg_text{};
    if (data->GetData(&fmt_text, &stg_text) == S_OK) {
        if (auto* w = static_cast<const wchar_t*>(GlobalLock(stg_text.hGlobal))) {
            out.type = DropData::Type::text;
            out.text = wide_to_utf8(w, static_cast<int>(wcslen(w)));
            GlobalUnlock(stg_text.hGlobal);
        }
        ReleaseStgMedium(&stg_text);
    }
    return out;
}

// Minimal IDropTarget that routes OLE drops on the child HWND into the shared
// cross-platform view-tree dispatch core (drag_drop.cpp) — the same core the SDL
// and macOS producers use. Owned by WinPluginViewHost; registered via
// RegisterDragDrop on the child HWND.
class PulpWinDropTarget : public IDropTarget {
public:
    PulpWinDropTarget(View& root, HWND hwnd, std::function<Point(Point)> to_root)
        : root_(root), hwnd_(hwnd), to_root_(std::move(to_root)) {}

    // Screen POINTL → client → root coordinates (mirrors the mouse path).
    Point to_root_point(POINTL pt) const {
        POINT p{pt.x, pt.y};
        ScreenToClient(hwnd_, &p);
        Point client{static_cast<float>(p.x), static_cast<float>(p.y)};
        return to_root_ ? to_root_(client) : client;
    }

    // ── IUnknown ──────────────────────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(++ref_);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const long r = --ref_;
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }

    // ── IDropTarget ───────────────────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL pt,
                                        DWORD* effect) override {
        pending_ = extract_idata_drop(data);
        const bool ok = dispatch_drag_enter(root_, session_, pending_,
                                            to_root_point(pt));
        *effect = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* effect) override {
        const bool ok = dispatch_drag_enter(root_, session_, pending_,
                                            to_root_point(pt));
        *effect = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        dispatch_drag_exit(root_, session_);
        pending_ = {};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL pt,
                                   DWORD* effect) override {
        DropData d = extract_idata_drop(data);
        const bool ok = dispatch_drop(root_, session_, d, to_root_point(pt));
        *effect = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        pending_ = {};
        return S_OK;
    }

private:
    View& root_;
    HWND hwnd_;
    std::function<Point(Point)> to_root_;
    DragSession session_;
    DropData pending_;  // payload cached between DragEnter and Drop
    long ref_ = 1;
};

// ── Outbound file drag (source side) ────────────────────────────────────────
// The OLE drag SOURCE (CF_HDROP IDataObject + IDropSource + DoDragDrop) now
// lives in the Skia-free shared header win_file_drag.hpp so the standalone
// window-host path (drag_drop_win.cpp) reuses the exact same backend. The plugin
// host calls win_drag::win_run_file_drag() from its start_file_drag() override.

class WinPluginViewHost : public PluginViewHost {
public:
    WinPluginViewHost(View& root, Size size) : root_(root), size_(size) {
        ensure_window_class();
        // Create a hidden TOP-LEVEL window first (WS_POPUP). A WS_CHILD window
        // with a null parent is not a valid creation shape; we flip the style to
        // WS_CHILD and SetParent() in attach_to_parent(). `this` is passed as
        // lpParam so the wndproc can stash it at WM_NCCREATE.
        hwnd_ = CreateWindowExW(
            0, kChildClassName, L"",
            WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
            0, 0, static_cast<int>(size.width), static_cast<int>(size.height),
            /*parent*/ nullptr, nullptr, GetModuleHandleW(nullptr),
            /*lpParam*/ this);
        if (!hwnd_) {
            runtime::log_warn("WinPluginViewHost: CreateWindowExW failed");
            return;
        }
        scale_ = detect_dpi_scale(hwnd_);
        init_drag_drop();
    }

    ~WinPluginViewHost() override {
        root_.set_plugin_view_host(nullptr);
        shutdown_drag_drop();
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

    // Outbound file drag (drag audio out to Explorer / a Canvas / another app).
    // The host owns its HWND, so this is a plain OLE drag source — no WebView
    // composition dependency. Mirrors the macOS NSDraggingSession override.
    bool start_file_drag(const FileDragRequest& request) override {
        return win_drag::win_run_file_drag(request);
    }

    void attach_to_parent(NativeViewHandle parent) override {
        if (!hwnd_) return;
        HWND parent_hwnd = static_cast<HWND>(parent);
        if (!parent_hwnd) return;
        // Switch WS_POPUP -> WS_CHILD before reparenting (SetParent does not add
        // the style itself); SWP_FRAMECHANGED makes the style edit take effect.
        LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_STYLE);
        style = (style & ~static_cast<LONG_PTR>(WS_POPUP)) | WS_CHILD;
        SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
        if (SetParent(hwnd_, parent_hwnd) == nullptr) {
            runtime::log_warn("WinPluginViewHost: SetParent failed (err {})",
                              static_cast<unsigned>(GetLastError()));
            // Restore top-level style so we don't leave a parentless WS_CHILD.
            SetWindowLongPtrW(hwnd_, GWL_STYLE,
                              (style & ~static_cast<LONG_PTR>(WS_CHILD)) | WS_POPUP);
            SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
            return;  // attached_ stays false; try_attach_to_parent reports failure
        }
        {
            // Bracket our own sizing SetWindowPos: it synchronously re-enters the
            // wndproc (WM_WINDOWPOSCHANGED -> WM_SIZE) on this thread, and we must
            // not reconcile a resize we ourselves drove (would double-fire the
            // resize callback / mis-scale on HiDPI). See handle_wm_size().
            ScopedFlag g(in_set_size_);
            SetWindowPos(hwnd_, nullptr, 0, 0,
                         static_cast<int>(size_.width), static_cast<int>(size_.height),
                         SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
        ShowWindow(hwnd_, SW_SHOW);
        attached_.store(true, std::memory_order_release);
#ifdef PULP_HAS_SKIA
        // Dawn configures its presentation surface for the HWND's current
        // native-window shape. Creating it while this HWND is still the hidden
        // WS_POPUP used during construction leaves presentation stale after
        // SetParent converts it into REAPER's WS_CHILD. Initialize only after
        // the final parent/style/size are in place.
        if (!gpu_surface_ || !skia_surface_)
            init_gpu(static_cast<float>(size_.width),
                     static_cast<float>(size_.height));
#endif
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
#ifdef PULP_HAS_SKIA
        // The presentation surface is tied to the attached native-window
        // shape. Recreate it on the next attach rather than carrying a surface
        // configured for the old parent across a detach/reparent cycle.
        skia_surface_.reset();
        gpu_surface_.reset();
#endif
        ShowWindow(hwnd_, SW_HIDE);
        SetParent(hwnd_, nullptr);
        // Restore the top-level style so the detached window is a valid
        // WS_POPUP again (not a parentless WS_CHILD).
        LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_STYLE);
        style = (style & ~static_cast<LONG_PTR>(WS_CHILD)) | WS_POPUP;
        SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        attached_.store(false, std::memory_order_release);
    }

    // Called from the wndproc on WM_PAINT (host-driven invalidation path).
    void handle_wm_paint() {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
#ifdef PULP_HAS_SKIA
        if (gpu_surface_ && skia_surface_) render_frame(nullptr, nullptr, nullptr);
#endif
        EndPaint(hwnd_, &ps);
    }

    void handle_mouse_down(LPARAM lp, WPARAM wp) {
        if (!hwnd_) return;
        try {
            const Point pt = mouse_point(lp);
            MouseEvent gesture_event;
            gesture_event.position = pt;
            gesture_event.window_position = pt;
            gesture_event.button = MouseButton::left;
            gesture_event.modifiers = mouse_modifiers(wp);
            gesture_event.is_down = true;
            gesture_event.phase = MousePhase::press;
            if (root_.dispatch_gesture_pointer_event(gesture_event)) {
                drag_target_ = nullptr;
                SetCapture(hwnd_);
                return;
            }

            drag_target_ = root_.hit_test(pt);
            ComboBox::notify_global_click(drag_target_);
            if (!drag_target_) return;
            SetFocus(hwnd_);
            SetCapture(hwnd_);
            if (!deliver_mouse_down(root_, drag_target_, pt,
                                    gesture_event.modifiers, 1))
                drag_target_ = nullptr;
            request_repaint_from_input();
        } catch (const std::exception& e) {
            runtime::log_warn("WinPluginViewHost: mouse down handler threw: {}",
                              e.what());
            drag_target_ = nullptr;
        } catch (...) {
            runtime::log_warn("WinPluginViewHost: mouse down handler threw");
            drag_target_ = nullptr;
        }
    }

    void handle_mouse_move(LPARAM lp, WPARAM wp) {
        PULP_TRACE_SCOPE_NAMED("state", "wm_mousemove");
        // WM_MOUSEMOVE also fires for plain hover, and mouse capture keeps
        // delivering moves after the button is released outside the window.
        if (!drag_target_ ||
            !win_input::drag_continues(static_cast<uint32_t>(wp)))
            return;
        try {
            const Point pt = mouse_point(lp);
            MouseEvent gesture_event;
            gesture_event.position = pt;
            gesture_event.window_position = pt;
            gesture_event.button = MouseButton::left;
            gesture_event.modifiers = mouse_modifiers(wp);
            gesture_event.is_down = true;
            gesture_event.phase = MousePhase::drag;
            if (!root_.dispatch_gesture_pointer_event(gesture_event))
                deliver_mouse_drag(root_, drag_target_, pt,
                                   gesture_event.modifiers);
            request_repaint_from_input();
        } catch (const std::exception& e) {
            runtime::log_warn("WinPluginViewHost: mouse move handler threw: {}",
                              e.what());
            drag_target_ = nullptr;
        } catch (...) {
            runtime::log_warn("WinPluginViewHost: mouse move handler threw");
            drag_target_ = nullptr;
        }
    }

    void handle_mouse_up(LPARAM lp, WPARAM wp) {
        if (GetCapture() == hwnd_) ReleaseCapture();
        try {
            const Point pt = mouse_point(lp);
            MouseEvent gesture_event;
            gesture_event.position = pt;
            gesture_event.window_position = pt;
            gesture_event.button = MouseButton::left;
            gesture_event.modifiers = mouse_modifiers(wp);
            gesture_event.is_down = false;
            gesture_event.phase = MousePhase::release;
            if (!root_.dispatch_gesture_pointer_event(gesture_event) &&
                drag_target_) {
                MouseUpHost up_host;
                up_host.fire_click =
                    [](const std::function<void()>& click_handler,
                       const std::string&, uint16_t) {
                        if (click_handler) click_handler();
                    };
                deliver_mouse_up(root_, drag_target_, pt,
                                 gesture_event.modifiers, 1, up_host);
            }
            drag_target_ = nullptr;
            request_repaint_from_input();
        } catch (const std::exception& e) {
            runtime::log_warn("WinPluginViewHost: mouse up handler threw: {}",
                              e.what());
            drag_target_ = nullptr;
        } catch (...) {
            runtime::log_warn("WinPluginViewHost: mouse up handler threw");
            drag_target_ = nullptr;
        }
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

    // Coalesced repaint for INPUT-driven updates (mouse down/move/up).
    //
    // repaint() renders synchronously on the GPU path, and the swapchain is
    // configured Fifo (vsync) by default, so acquiring the next texture blocks
    // until the next refresh. Rendering inline from the wndproc therefore costs
    // a full vsync-blocked frame PER WM_MOUSEMOVE. Windows delivers moves far
    // faster than that, so the queue backs up and a dragged control visibly
    // trails the cursor — the lag is the queued input, not the render cost.
    //
    // Invalidating instead lets Windows collapse every move that arrived during
    // a frame into ONE WM_PAINT, which repaints at the LATEST pointer position.
    // Mouse handling becomes near-free and the control tracks the cursor.
    //
    // Safe for the embed path: these callers only reach us through the wndproc,
    // which means a message pump exists to deliver the WM_PAINT. Callers that
    // drive frames explicitly (pulp_embed_tick) still use repaint() directly.
    void request_repaint_from_input() {
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void set_size(uint32_t width, uint32_t height) override {
        size_ = {width, height};
        if (hwnd_) {
            // SetWindowPos re-enters the wndproc synchronously (WM_SIZE); the
            // guard makes handle_wm_size() ignore that self-driven echo so we
            // don't reconcile (and re-fire the resize callback for) a size WE
            // just set.
            ScopedFlag g(in_set_size_);
            SetWindowPos(hwnd_, nullptr, 0, 0, static_cast<int>(width),
                         static_cast<int>(height), SWP_NOZORDER | SWP_NOMOVE);
        }
#ifdef PULP_HAS_SKIA
        // GPU surface is sized at PHYSICAL pixels (logical × scale); SkiaSurface
        // takes LOGICAL dims + the scale factor and applies the logical→pixel
        // transform itself at paint (mirrors MacGpuWindowHost).
        damage_.mark_full();  // new surface size => first frame is full
        if (gpu_surface_) gpu_surface_->resize(pixel_w(), pixel_h());
        if (skia_surface_) skia_surface_->resize(width, height, scale_);
#endif
        repaint();
    }

    // Reconcile a native (host-driven) resize of our child HWND. Called from the
    // wndproc on WM_SIZE / WM_WINDOWPOSCHANGED when a DAW resizes the editor
    // container. `physical_w/h` are the client area in PHYSICAL device pixels
    // (Windows reports window/client sizes in device pixels). This is the
    // Windows analogue of the macOS -setFrameSize: -> on_native_frame_changed
    // path. It deliberately does NOT call SetWindowPos — the host already sized
    // the window, and calling it here would recurse.
    void handle_wm_size(uint32_t physical_w, uint32_t physical_h) {
        // Ignore the WM_SIZE/WM_WINDOWPOSCHANGED echo of our own SetWindowPos.
        if (in_set_size_) return;
        if (physical_w == 0 || physical_h == 0) return;  // minimized / degenerate
        // PHYSICAL → LOGICAL. size_ / the view tree live in logical units; the
        // GPU swapchain lives at physical pixels. Round to nearest so fractional
        // scales (1.5×) don't drift a pixel each resize.
        const float s = scale_ > 0.0f ? scale_ : 1.0f;
        const uint32_t logical_w =
            static_cast<uint32_t>(static_cast<float>(physical_w) / s + 0.5f);
        const uint32_t logical_h =
            static_cast<uint32_t>(static_cast<float>(physical_h) / s + 0.5f);
        if (logical_w == 0 || logical_h == 0) return;
        if (logical_w == size_.width && logical_h == size_.height) return;  // no-op
        size_ = {logical_w, logical_h};
#ifdef PULP_HAS_SKIA
        // Mirror set_size()'s surface sizing (GPU at physical, Skia at logical +
        // scale) but WITHOUT the SetWindowPos.
        damage_.mark_full();  // new surface size => first frame is full
        if (gpu_surface_) gpu_surface_->resize(pixel_w(), pixel_h());
        if (skia_surface_) skia_surface_->resize(logical_w, logical_h, scale_);
#endif
        if (resize_cb_) resize_cb_(logical_w, logical_h);
        repaint();
    }

    Size get_size() const override { return size_; }

    // ── HiDPI scale seam (W8) ────────────────────────────────────────────
    float scale_factor() const override { return scale_; }

    void set_scale_factor(float scale) override {
        if (scale <= 0.0f) return;  // ignore non-positive; keep current scale
        if (scale == scale_) return;
        scale_ = scale;
#ifdef PULP_HAS_SKIA
        // Re-size surfaces at the new pixel resolution. Logical size is
        // unchanged, so the view tree layout is untouched.
        damage_.mark_full();  // new surface size => first frame is full
        if (gpu_surface_) gpu_surface_->resize(pixel_w(), pixel_h());
        if (skia_surface_) skia_surface_->resize(size_.width, size_.height, scale_);
#endif
        repaint();
    }

    // WM_DPICHANGED: derive the new scale from the wParam DPI and rescale.
    void handle_dpi_changed(uint32_t new_dpi) {
        if (new_dpi == 0) return;
        set_scale_factor(static_cast<float>(new_dpi) / 96.0f);
    }

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
    Size size_;        // LOGICAL (DPI-independent) size; layout coordinate space
    HWND hwnd_ = nullptr;
    std::atomic<bool> attached_{false};
    std::function<void()> idle_callback_;
    std::function<void(uint32_t, uint32_t)> resize_cb_;
    float design_viewport_w_ = 0.0f;
    float design_viewport_h_ = 0.0f;
    float fixed_aspect_ratio_ = 0.0f;
    bool design_top_align_ = false;
    float scale_ = 1.0f;  // HiDPI: logical→physical-pixel factor (DPI/96)
    // True only while WE drive a SetWindowPos (set_size/attach). SetWindowPos
    // re-enters the wndproc synchronously on this thread, so this flag lets
    // handle_wm_size() distinguish a host-driven resize from our own echo.
    bool in_set_size_ = false;
    View* drag_target_ = nullptr;
    // FU-2: when true the host clips the repaint to the damaged rect and blits
    // a retained persistent-scene target. Default OFF.
    bool partial_repaint_enabled_ = false;

    // Window-space mapping lives in win_pointer_input.hpp so the signed-word
    // unpack and the physical→logical divide are unit-tested off Windows (the
    // required CI gate is macOS). Only the design-viewport transform, which
    // needs host state, stays here.
    Point mouse_point(LPARAM lp) const {
        return window_to_root_point(win_input::lparam_to_logical_point(
            static_cast<uint32_t>(static_cast<uintptr_t>(lp)), scale_));
    }

    // Windows packs only Shift/Control into the message; Alt and the Windows
    // key are keyboard state, so they are sampled here and passed to the pure
    // mapper.
    static uint16_t mouse_modifiers(WPARAM wp) {
        const bool alt_down = (GetKeyState(VK_MENU) & 0x8000) != 0;
        const bool meta_down = (GetKeyState(VK_LWIN) & 0x8000) != 0 ||
                               (GetKeyState(VK_RWIN) & 0x8000) != 0;
        return win_input::mouse_modifiers(
            static_cast<uint32_t>(wp), alt_down, meta_down);
    }

    // Physical pixel dimensions = logical × scale (min 1 to avoid 0-sized
    // surfaces). The GPU surface/swapchain is allocated at this resolution.
    uint32_t pixel_w() const {
        const float p = size_.width * scale_;
        return static_cast<uint32_t>(p < 1.0f ? 1.0f : p);
    }
    uint32_t pixel_h() const {
        const float p = size_.height * scale_;
        return static_cast<uint32_t>(p < 1.0f ? 1.0f : p);
    }

    // Derive the DPI scale for a window. GetDpiForWindow returns the effective
    // DPI (96 = 1×, 144 = 1.5×, 192 = 2×). Falls back to 1.0 if it reports 0
    // (per-monitor-DPI unaware context, or a window with no monitor yet).
    static float detect_dpi_scale(HWND hwnd) {
        if (!hwnd) return 1.0f;
        const UINT dpi = GetDpiForWindow(hwnd);
        if (dpi == 0) return 1.0f;
        return static_cast<float>(dpi) / 96.0f;
    }

    // OLE drag-drop on the child HWND. ole_initialized_ tracks whether THIS host
    // brought up COM (so we balance OleUninitialize). drop_target_ is ref-counted
    // by OLE; we hold one reference and Release it on teardown.
    bool ole_initialized_ = false;
    PulpWinDropTarget* drop_target_ = nullptr;

    void init_drag_drop() {
        if (!hwnd_) return;
        // Drag-drop needs an STA. OleInitialize is per-thread + ref-counted; if
        // the host thread is already MTA it returns RPC_E_CHANGED_MODE — then we
        // honest-skip drag-drop rather than fight the apartment model.
        const HRESULT hr = OleInitialize(nullptr);
        if (hr != S_OK && hr != S_FALSE) {
            runtime::log_warn("WinPluginViewHost: OleInitialize failed (0x{:08x}); "
                              "drag-drop disabled", static_cast<unsigned>(hr));
            return;
        }
        ole_initialized_ = true;
        // The drop target hands us CLIENT-space coords in PHYSICAL pixels
        // (ScreenToClient output). Convert pixels→logical (÷ scale) before the
        // logical-space design-viewport inverse, so HiDPI hit-testing lands on
        // the right widget.
        drop_target_ = new PulpWinDropTarget(
            root_, hwnd_, [this](Point px) {
                const float s = scale_ > 0.0f ? scale_ : 1.0f;
                return window_to_root_point({px.x / s, px.y / s});
            });
        if (RegisterDragDrop(hwnd_, drop_target_) != S_OK) {
            runtime::log_warn("WinPluginViewHost: RegisterDragDrop failed");
            drop_target_->Release();
            drop_target_ = nullptr;
        }
    }

    void shutdown_drag_drop() {
        if (drop_target_) {
            if (hwnd_) RevokeDragDrop(hwnd_);
            drop_target_->Release();
            drop_target_ = nullptr;
        }
        if (ole_initialized_) {
            OleUninitialize();
            ole_initialized_ = false;
        }
    }

#ifdef PULP_HAS_SKIA
    std::unique_ptr<render::GpuSurface> gpu_surface_;
    std::unique_ptr<render::SkiaSurface> skia_surface_;

    void init_gpu(float width, float height) {
        runtime::log_info(
            "WinPluginViewHost: init GPU logical={}x{} physical={}x{} scale={}",
            width, height, pixel_w(), pixel_h(), scale_);
        gpu_surface_ = render::GpuSurface::create_dawn();
        if (!gpu_surface_) {
            runtime::log_warn("WinPluginViewHost: gpu create_dawn failed; cpu-capture only");
            return;
        }
        render::GpuSurface::Config cfg{};
        // GPU surface at PHYSICAL pixels (logical × scale) so the swapchain
        // matches the HiDPI display; the view tree stays in logical units.
        cfg.width = pixel_w();
        cfg.height = pixel_h();
        cfg.native_surface_handle = static_cast<void*>(hwnd_);  // HWND
        // Do NOT vsync-block a plug-in editor.
        //
        // The default (vsync = true) selects a Fifo present mode, so the next
        // GetCurrentTexture() blocks until the display's next refresh. That is
        // right for a standalone app that owns its frame loop, and wrong here:
        // this host renders synchronously from the DAW's WM_PAINT, on the DAW's
        // UI thread, and the DAW already composites at its own cadence. Blocking
        // there stalls the message pump that delivers further input.
        //
        // Measured on the REAPER VM during a knob drag (Perfetto): the frame's
        // own work is ~2 ms (paint ~1 ms, submit ~1 ms, present ~0.1 ms) while
        // whole frames took 19-45 ms; the difference was all acquire. Only 7
        // frames were produced across 8 drag sweeps.
        cfg.vsync = false;
        if (!gpu_surface_->initialize(cfg)) {
            runtime::log_warn("WinPluginViewHost: gpu initialize failed; cpu-capture only");
            gpu_surface_.reset();
            return;
        }
        render::SkiaSurface::Config scfg{};
        scfg.width = static_cast<uint32_t>(width);   // LOGICAL
        scfg.height = static_cast<uint32_t>(height);  // LOGICAL
        scfg.scale_factor = scale_;
        skia_surface_ = render::SkiaSurface::create(*gpu_surface_, scfg);
        if (!skia_surface_) {
            runtime::log_warn("WinPluginViewHost: skia surface create failed; cpu-capture only");
            gpu_surface_.reset();
            return;
        }
        // FU-2 partial repaint (default OFF, PULP_PARTIAL_REPAINT=1). The Dawn
        // swapchain does not preserve content between frames, so a clipped
        // repaint is only correct against a retained scene target. If the
        // backend cannot retain one, disable partial repaint outright — we must
        // never clip a non-preserving surface.
        if (const char* env = std::getenv("PULP_PARTIAL_REPAINT"))
            partial_repaint_enabled_ = (env[0] == '1');
        if (partial_repaint_enabled_ && skia_surface_ &&
            !skia_surface_->set_persistent_scene(true)) {
            partial_repaint_enabled_ = false;
            runtime::log_warn("WinPluginViewHost: backend cannot retain a scene; "
                              "partial repaint disabled");
        }
        // A newly created surface has no previous frame to preserve.
        damage_.mark_full();
        runtime::log_info("WinPluginViewHost: GPU and Skia surfaces ready");
    }

    // Shared scene paint (matches the macOS plugin GPU host).
    void paint_scene(canvas::Canvas& canvas, const Rect* clip = nullptr) {
        // Background fill + layout + view-tree paint. The nested
        // layout/layout_children span from View::layout_children() lands inside
        // this one, which is what makes layout-vs-paint attribution possible.
        PULP_TRACE_SCOPE_NAMED("canvas", "paint");
        // FU-2: clip the ENTIRE body, background fill included. Everything
        // outside the clip must remain the retained scene's previous pixels;
        // filling the background unclipped would erase them.
        const int clip_save = canvas.save_count();
        if (clip) {
            canvas.save();
            canvas.clip_rect(clip->x, clip->y, clip->width, clip->height);
        }
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
        if (clip) canvas.restore_to_count(clip_save);
    }

    bool render_frame(std::vector<uint8_t>* cap, uint32_t* cap_w, uint32_t* cap_h) {
        // The real per-frame drive point (paint -> submit -> present below).
        // Mirrors the macOS host so a Windows trace is readable with the same
        // queries; without it the Windows editor showed only gpu_submit /
        // gpu_present and there was no frame span to attribute them to.
        PULP_TRACE_SCOPE_NAMED("render", "frame");
        if (!gpu_surface_ || !skia_surface_) return false;
        if (idle_callback_) idle_callback_();
        // Swapchain acquire, instrumented because it is the one part of the
        // frame that BLOCKS: under a Fifo (vsync) present mode
        // GetCurrentTexture() waits for the next refresh. Without this span a
        // trace shows a frame whose children sum to ~2 ms but whose total is
        // 20-45 ms, and the missing time has nowhere to be attributed.
        bool acquired = false;
        {
            PULP_TRACE_SCOPE_NAMED("gpu", "gpu_acquire");
            acquired = gpu_surface_->begin_frame();
        }
        if (!acquired) return false;
        auto* canvas = skia_surface_->begin_frame();
        if (!canvas) {
            gpu_surface_->end_frame();
            return false;
        }
        // Decide whether this frame can be clipped losslessly. The hazard model
        // (compute_effective_damage) escalates to a full repaint if anything
        // that SAMPLES at a distance — backdrop-filter, blur, mask, sampling
        // effect, render transform — reaches the damage, which is what makes a
        // clipped repaint pixel-identical to a full one.
        //
        // Skipped entirely under a design viewport: paint applies a letterbox
        // scale+translate there, so root-space damage does not map to the
        // clip space without further work.
        Rect clip_rect{};
        const Rect* clip = nullptr;
        if (partial_repaint_enabled_ && !pending_repaint_is_full() &&
            has_pending_dirty_bounds()) {
            const auto b = pending_dirty_bounds();
            // Hazard model runs in ROOT space (where the damage was produced).
            const auto decision = compute_effective_damage(root_, b, scale_);
            if (!decision.full) {
                clip_rect = decision.bounds;
                // Under a design viewport the paint body applies
                // translate(tx,ty) + scale(sx,sy), but the clip below is
                // installed in SURFACE space, before that transform. Map the
                // root-space damage through the same letterbox transform so
                // the two agree; without this the clip lands in the wrong
                // place (which is why plug-in editors — which always set a
                // design viewport — previously had to skip partial repaint
                // entirely).
                float sx, sy, tx, ty;
                if (design_viewport_w_ > 0.0f && design_viewport_h_ > 0.0f &&
                    WindowHost::compute_design_viewport_transform(
                        static_cast<float>(size_.width),
                        static_cast<float>(size_.height),
                        design_viewport_w_, design_viewport_h_,
                        sx, sy, tx, ty, design_top_align_) &&
                    sx > 0.0f && sy > 0.0f) {
                    clip_rect = Rect{tx + clip_rect.x * sx,
                                     ty + clip_rect.y * sy,
                                     clip_rect.width * sx,
                                     clip_rect.height * sy};
                    // Re-snap OUT to whole surface pixels after scaling: a
                    // fractional edge would clip a partially covered pixel.
                    const float x0 = std::floor(clip_rect.x);
                    const float y0 = std::floor(clip_rect.y);
                    clip_rect = Rect{x0, y0,
                                     std::ceil(clip_rect.x + clip_rect.width) - x0,
                                     std::ceil(clip_rect.y + clip_rect.height) - y0};
                }
                clip = &clip_rect;
            }
        }
        paint_scene(*canvas, clip);
        bool readback_ok = true;
        if (cap) {
            // read_current_rgba finalizes + submits the open frame's recording
            // before readback (see SkiaSurface contract), so no separate flush.
            uint32_t pw = 0, ph = 0;
            readback_ok = skia_surface_->read_current_rgba(*cap, pw, ph) &&
                          !cap->empty() && pw > 0 && ph > 0;
            if (cap_w) *cap_w = pw;
            if (cap_h) *cap_h = ph;
        }
        skia_surface_->end_frame();
        gpu_surface_->end_frame();
        clear_pending_dirty();  // frame painted + submitted (mirrors DirtyTracker::clear)
        // A failed/empty readback must report false so capture_back_buffer_png()
        // falls back to the raster path instead of returning a blank frame.
        return cap ? readback_ok : true;
    }

    // Pure-CPU raster capture, GPU-independent — the VM proof path. Sized at
    // PHYSICAL pixels (logical × scale) with the logical→pixel scale applied as
    // a canvas transform, so paint_scene keeps working in logical units and the
    // capture is crisp on HiDPI / matches the GPU surface pixel resolution.
    std::vector<uint8_t> raster_capture_png() {
        const uint32_t w = pixel_w(), h = pixel_h();
        if (w == 0 || h == 0) return {};
        auto cs = SkColorSpace::MakeSRGB();
        SkImageInfo info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType,
                                             kPremul_SkAlphaType, cs);
        auto surface = SkSurfaces::Raster(info);
        if (!surface) return {};
        auto* sk_canvas = surface->getCanvas();
        if (!sk_canvas) return {};
        if (scale_ != 1.0f) sk_canvas->scale(scale_, scale_);
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

LRESULT CALLBACK pulp_pvh_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        // Stash the host pointer passed via CreateWindowEx lpParam.
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    auto* host = reinterpret_cast<WinPluginViewHost*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (host && msg == WM_PAINT) {
        host->handle_wm_paint();
        return 0;
    }
    if (host && msg == WM_LBUTTONDOWN) {
        host->handle_mouse_down(lp, wp);
        return 0;
    }
    if (host && msg == WM_MOUSEMOVE) {
        host->handle_mouse_move(lp, wp);
        return 0;
    }
    if (host && msg == WM_LBUTTONUP) {
        host->handle_mouse_up(lp, wp);
        return 0;
    }
    if (host && msg == WM_DPICHANGED) {
        // wParam LOWORD = new DPI (X); HIWORD = Y (identical on Windows). The
        // view tree stays in logical units — we only rescale the surfaces and
        // repaint at the new pixel resolution. The DAW owns the window frame,
        // so we don't apply the suggested lParam RECT ourselves.
        host->handle_dpi_changed(LOWORD(wp));
        return 0;
    }
    if (host && msg == WM_SIZE) {
        // lParam LOWORD = new client width, HIWORD = height, in PHYSICAL pixels.
        // Skip minimize (a 0×0 client) — handle_wm_size() also no-ops on 0.
        if (wp != SIZE_MINIMIZED)
            host->handle_wm_size(LOWORD(lp), HIWORD(lp));
        return 0;
    }
    if (host && msg == WM_WINDOWPOSCHANGED) {
        // Belt-and-suspenders alongside WM_SIZE: a host that repositions/resizes
        // via SetWindowPos delivers this. When the size actually changed, derive
        // the client size and reconcile. GetClientRect gives the exact client
        // area (WINDOWPOS.cx/cy are the full window size). The no-op-on-equal
        // check in handle_wm_size() dedupes the paired WM_SIZE. We deliberately
        // fall through to DefWindowProcW so the default handling still generates
        // the WM_SIZE/WM_MOVE messages other code may rely on.
        auto* wpos = reinterpret_cast<const WINDOWPOS*>(lp);
        if (wpos && !(wpos->flags & SWP_NOSIZE)) {
            RECT rc{};
            if (GetClientRect(hwnd, &rc)) {
                host->handle_wm_size(static_cast<uint32_t>(rc.right - rc.left),
                                     static_cast<uint32_t>(rc.bottom - rc.top));
            }
        }
        // fall through to DefWindowProcW
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

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
