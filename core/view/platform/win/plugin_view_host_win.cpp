// Windows (HWND) PluginViewHost — native child-window editor host for the
// foreign-host embed SDK + the VST3/CLAP adapters on Windows. Mirrors the
// macOS MacGpuPluginViewHost shape (NSView -> child HWND,
// CAMetalLayer -> HWND-backed Dawn surface).
//
// Two render paths share one class:
//   - GPU: a Dawn surface created from the child HWND, wrapped by SkiaSurface,
//     painted on demand (repaint()/WM_PAINT). Requires a working WebGPU backend
//     (D3D12/Vulkan). When GPU use is disabled or Dawn init fails,
//     WM_PAINT renders the same tree through Skia's CPU raster path and blits a
//     top-down BGRA DIB into the child HWND.
//   - Headless capture: capture_back_buffer_png() always works (raster), so the
//     embed's hidden-window smoke can verify a real non-black frame even with no
//     GPU — the VM-verifiable proof.
//
// Threading: all window ops run on the calling (UI) thread. There is no internal
// message loop — the host that owns the parent HWND pumps messages; repaint()
// invalidates and a WM_PAINT triggers render_frame(). For embed callers that
// drive frames explicitly (pulp_embed_tick), repaint() renders synchronously.

#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/plugin_frame_renderer.hpp>  // shared with the Linux host
#include <pulp/view/pointer_dispatch.hpp>
#include <pulp/view/ui_components.hpp>  // ComboBox::notify_global_click
#include <pulp/view/platform/win_pointer_input.hpp>
#include <pulp/view/platform/win_surface_lifecycle.hpp>
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
// After win32_sane.hpp + the OLE headers, matching where this code sat
// before it was extracted — the include ORDER is load-bearing here.
#include <pulp/view/platform/win_plugin_drop_target.hpp>

#include <pulp/view/drag_drop.hpp>

#include <algorithm>
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
static_assert(win_input::kMkMButton == MK_MBUTTON);
static_assert(win_input::kWmLButtonDown == WM_LBUTTONDOWN);
static_assert(win_input::kWmLButtonUp == WM_LBUTTONUP);
static_assert(win_input::kWmRButtonDown == WM_RBUTTONDOWN);
static_assert(win_input::kWmRButtonUp == WM_RBUTTONUP);
static_assert(win_input::kWmMButtonDown == WM_MBUTTONDOWN);
static_assert(win_input::kWmMButtonUp == WM_MBUTTONUP);
static_assert(win_input::kVkOem1 == VK_OEM_1);
static_assert(win_input::kVkOem7 == VK_OEM_7);

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

// ── Outbound file drag (source side) ────────────────────────────────────────
// The OLE drag SOURCE (CF_HDROP IDataObject + IDropSource + DoDragDrop) now
// lives in the Skia-free shared header win_file_drag.hpp so the standalone
// window-host path (drag_drop_win.cpp) reuses the exact same backend. The plugin
// host calls win_drag::win_run_file_drag() from its start_file_drag() override.

class WinPluginViewHost : public PluginViewHost {
public:
    WinPluginViewHost(View& root, Size size, bool use_gpu)
        : root_(root), size_(size), use_gpu_(use_gpu) {
        // Dawn cannot configure presentation until attach_to_parent() gives the
        // HWND its final parent and style, so gpu_surface() is legitimately
        // null for the whole window between create() and attach. Say so, rather
        // than letting that null read as "this host is CPU".
        if (use_gpu_) mark_gpu_surface_pending();
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
        // Publish the teardown before the surfaces die, so a consumer holding
        // the raw pointer drops it here instead of on its next frame. This host
        // is going away for good, hence `unavailable`.
        release_gpu_surfaces(GpuSurfaceState::unavailable);
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
        SetLastError(ERROR_SUCCESS);
        const HWND previous_parent = SetParent(hwnd_, parent_hwnd);
        const DWORD set_parent_error = GetLastError();
        // A null return is the previous parent. It is success when this hidden
        // construction HWND had no parent; only a non-zero last error is fail.
        if (previous_parent == nullptr && set_parent_error != ERROR_SUCCESS) {
            runtime::log_warn("WinPluginViewHost: SetParent failed (err {})",
                              static_cast<unsigned>(set_parent_error));
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
        if (use_gpu_ && surface_lifecycle_.note_attached()) {
            init_gpu(static_cast<float>(size_.width),
                     static_cast<float>(size_.height));
            // Reset the recreate budget on a FRESH editor session only. Doing
            // it inside init_gpu() would also reset it on the recreate path,
            // and a surface that rebuilds cleanly but never presents would then
            // recreate forever instead of falling back to CPU raster.
            if (gpu_surface_ && skia_surface_)
                frame_renderer_.note_surfaces_created();
            if (!gpu_surface_ || !skia_surface_) {
                surface_lifecycle_.note_creation_failed();
                // GPU init failed for real (no adapter, no Skia surface). This
                // is the ONLY point at which "silently fell back to CPU" is a
                // true statement about this host, so it is the only point that
                // may publish `unavailable` — a consumer that sees it can now
                // legitimately warn.
                release_gpu_surfaces(GpuSurfaceState::unavailable);
            }
        }
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
        cancel_pointer_gesture();
        try {
            transfer_input_focus(root_, nullptr);
        } catch (const std::exception& e) {
            runtime::log_warn("WinPluginViewHost: detach blur threw: {}", e.what());
        } catch (...) {
            runtime::log_warn("WinPluginViewHost: detach blur threw");
        }
#ifdef PULP_HAS_SKIA
        if (surface_lifecycle_.note_detached()) {
            // The presentation surface is tied to the attached native-window
            // shape. Recreate it on the next attach rather than carrying a
            // surface configured for the old parent across a detach/reparent.
            // `pending`, NOT `unavailable`: this host rebuilds the surface the
            // moment it is reattached, so consumers must wait rather than
            // conclude the editor is CPU-only — and the CPU-fallback
            // diagnostic must not fire on an ordinary editor close.
            release_gpu_surfaces(use_gpu_ ? GpuSurfaceState::pending
                                          : GpuSurfaceState::unavailable);
        }
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
        HDC dc = BeginPaint(hwnd_, &ps);
#ifdef PULP_HAS_SKIA
        if (gpu_surface_ && skia_surface_) {
            render_frame(nullptr, nullptr, nullptr);
        } else if (dc) {
            // Same contract as the GPU path: consume the damage for this frame
            // and put it back if the blit never happens, so a failed raster
            // frame does not silently retire a region nobody painted.
            const PendingDamage::Snapshot consumed = damage_.take();
            uint32_t w = 0, h = 0;
            auto pixels = raster_render_rgba(&w, &h);
            if (pixels.empty()) {
                damage_.restore(consumed);
            } else {
                // BI_RGB's 32-bit DIB byte order is BGRA on little-endian
                // Windows. Skia readback is RGBA, so swap red/blue before the
                // blit. A negative height makes the DIB top-down, matching the
                // view coordinate system.
                for (size_t i = 0; i < pixels.size(); i += 4)
                    std::swap(pixels[i], pixels[i + 2]);
                BITMAPINFO info{};
                info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                info.bmiHeader.biWidth = static_cast<LONG>(w);
                info.bmiHeader.biHeight = -static_cast<LONG>(h);
                info.bmiHeader.biPlanes = 1;
                info.bmiHeader.biBitCount = 32;
                info.bmiHeader.biCompression = BI_RGB;
                if (StretchDIBits(dc, 0, 0, static_cast<int>(w),
                                  static_cast<int>(h), 0, 0,
                                  static_cast<int>(w), static_cast<int>(h),
                                  pixels.data(), &info, DIB_RGB_COLORS,
                                  SRCCOPY) == GDI_ERROR)
                    damage_.restore(consumed);
            }
        }
#endif
        EndPaint(hwnd_, &ps);
    }

    void handle_mouse_down(LPARAM lp, WPARAM wp, MouseButton button) {
        if (!hwnd_) return;
        uint64_t generation = 0;
        try {
            // This host owns one capture bracket. Close a prior button before
            // accepting a chord mate instead of overwriting its target.
            if (pointer_session_.active())
                cancel_pointer_gesture();
            if (!pointer_session_.begin(button)) return;
            generation = pointer_session_.generation();
            const auto accepts_original = [this, generation, button] {
                return pointer_session_.accepts(generation, button);
            };
            const Point pt = mouse_point(lp);
            last_pointer_point_ = pt;
            MouseEvent gesture_event;
            gesture_event.position = pt;
            gesture_event.window_position = pt;
            gesture_event.button = button;
            gesture_event.modifiers = mouse_modifiers(wp);
            gesture_event.is_down = true;
            gesture_event.phase = MousePhase::press;
            if (yield_to_gesture(gesture_event)) {
                // A synchronous gesture callback may have terminalized this
                // bracket. Capture only for the still-current claimed session.
                if (accepts_original()) SetCapture(hwnd_);
                return;
            }
            if (!accepts_original()) return;

            drag_target_ = root_.hit_test(pt);
            if (button == MouseButton::left)
                ComboBox::notify_global_click(drag_target_);
            if (!accepts_original()) return;
            if (!drag_target_) {
                cancel_pointer_gesture();
                return;
            }
            SetFocus(hwnd_);
            if (!accepts_original()) return;
            if (!transfer_input_focus(root_, drag_target_)) {
                if (accepts_original()) {
                    drag_target_ = nullptr;
                    cancel_pointer_gesture();
                }
                return;
            }
            if (!accepts_original()) return;
            SetCapture(hwnd_);
            if (!accepts_original()) return;
            MouseDownHost down_host;
            down_host.should_continue = accepts_original;
            const bool target_alive = deliver_mouse_down(
                root_, drag_target_, pt, gesture_event.modifiers, 1, true,
                button, down_host);
            // Only this generation may mutate its captured target. A modern
            // callback may have synchronously cancelled/replaced the session.
            if (!accepts_original()) return;
            if (!target_alive)
                drag_target_ = nullptr;
            if (button == MouseButton::right)
                dispatch_context_menu(root_, pt);
            if (!accepts_original()) return;
            request_repaint_from_input();
        } catch (const std::exception& e) {
            runtime::log_warn("WinPluginViewHost: mouse down handler threw: {}",
                              e.what());
            if (pointer_session_.accepts(generation, button))
                cancel_pointer_gesture();
        } catch (...) {
            runtime::log_warn("WinPluginViewHost: mouse down handler threw");
            if (pointer_session_.accepts(generation, button))
                cancel_pointer_gesture();
        }
    }

    void handle_mouse_move(LPARAM lp, WPARAM wp) {
        PULP_TRACE_SCOPE_NAMED("state", "wm_mousemove");
        try {
            const Point pt = mouse_point(lp);
            last_pointer_point_ = pt;
            if (!tracking_mouse_leave_) {
                TRACKMOUSEEVENT track{};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = hwnd_;
                tracking_mouse_leave_ = TrackMouseEvent(&track) != FALSE;
            }
            const bool held = win_input::drag_continues(
                static_cast<uint32_t>(wp), pointer_session_.button());
            if (pointer_session_.active() && !held)
                cancel_pointer_gesture();
            if (!pointer_session_.active()) {
                root_.simulate_hover(pt);
                request_repaint_from_input();
                return;
            }
            MouseEvent gesture_event;
            gesture_event.position = pt;
            gesture_event.window_position = pt;
            gesture_event.button = pointer_session_.button();
            gesture_event.modifiers = mouse_modifiers(wp);
            gesture_event.is_down = true;
            gesture_event.phase = MousePhase::drag;
            const bool gesture_yielded = yield_to_gesture(gesture_event);
            if (!gesture_yielded && drag_target_)
                deliver_mouse_drag(root_, drag_target_, pt,
                                   gesture_event.modifiers, 1,
                                   PointerType::mouse, 0.5f,
                                   pointer_session_.button());
            request_repaint_from_input();
        } catch (const std::exception& e) {
            runtime::log_warn("WinPluginViewHost: mouse move handler threw: {}",
                              e.what());
            cancel_pointer_gesture();
        } catch (...) {
            runtime::log_warn("WinPluginViewHost: mouse move handler threw");
            cancel_pointer_gesture();
        }
    }

    void handle_mouse_up(LPARAM lp, WPARAM wp, MouseButton button) {
        if (!pointer_session_.accepts(button)) return;
        try {
            // Publish terminal before gesture/raw callbacks. Those callbacks
            // can synchronously pump capture-change or another button-up.
            const auto terminal = pointer_session_.terminalize();
            const Point pt = mouse_point(lp);
            last_pointer_point_ = pt;
            MouseEvent gesture_event;
            gesture_event.position = pt;
            gesture_event.window_position = pt;
            gesture_event.button = button;
            gesture_event.modifiers = mouse_modifiers(wp);
            gesture_event.is_down = false;
            gesture_event.phase = MousePhase::release;
            const bool gesture_yielded =
                should_yield_to_gesture(root_, gesture_event);
            // A callback above may have synchronously cancelled this generation
            // (and closed drag_target_). Never resume the stale outer release.
            if (pointer_session_.phase() !=
                    win_input::PointerSession::Phase::terminal ||
                pointer_session_.generation() != terminal.generation)
                return;
            View* target = drag_target_;
            drag_target_ = nullptr;
            if (gesture_yielded) {
                if (target)
                    deliver_gesture_handoff(root_, target, pt,
                                            gesture_event.modifiers, 1);
            } else if (target) {
                MouseUpHost up_host;
                if (button == MouseButton::left) {
                    up_host.fire_click =
                        [](const std::function<void()>& click_handler,
                           const std::string&, uint16_t) {
                            if (click_handler) click_handler();
                        };
                }
                deliver_mouse_up(root_, target, pt,
                                 gesture_event.modifiers, 1, up_host, button);
            }
            if (pointer_session_.phase() ==
                    win_input::PointerSession::Phase::terminal &&
                pointer_session_.generation() == terminal.generation) {
                release_capture_if_owned();
                pointer_session_.finish_terminal(terminal);
            }
            request_repaint_from_input();
        } catch (const std::exception& e) {
            runtime::log_warn("WinPluginViewHost: mouse up handler threw: {}",
                              e.what());
            cancel_pointer_gesture();
        } catch (...) {
            runtime::log_warn("WinPluginViewHost: mouse up handler threw");
            cancel_pointer_gesture();
        }
    }

    void handle_capture_lost() {
        // WM_CAPTURECHANGED/WM_CANCELMODE can arrive without a matching button
        // up (host modal UI, Alt+Tab, another HWND taking capture). End the
        // gesture explicitly so value widgets and the gesture arbiter cannot
        // remain latched in a drag until the editor is reopened.
        cancel_pointer_gesture();
    }

    void handle_mouse_leave() {
        tracking_mouse_leave_ = false;
        if (pointer_session_.active())
            return;  // capture keeps a drag alive outside HWND
        root_.simulate_hover({-1000000.0f, -1000000.0f});
        request_repaint_from_input();
    }

    void handle_mouse_wheel(LPARAM lp, WPARAM wp, bool horizontal) {
        const auto raw = static_cast<uint32_t>(static_cast<uintptr_t>(lp));
        POINT screen{win_input::lparam_x(raw), win_input::lparam_y(raw)};
        if (!ScreenToClient(hwnd_, &screen)) return;
        const auto packed = MAKELPARAM(static_cast<short>(screen.x),
                                      static_cast<short>(screen.y));
        const Point pt = mouse_point(packed);
        const float steps = win_input::wheel_steps(
            static_cast<uint32_t>(wp), horizontal);
        WheelHost wheel_host;
        wheel_host.request_repaint = [this] { request_repaint_from_input(); };
        deliver_mouse_wheel(root_, pt, horizontal ? steps : 0.0f,
                            horizontal ? 0.0f : steps, wheel_host);
    }

    bool handle_key(WPARAM wp, LPARAM lp, bool is_down) {
        auto* focused = focused_input_under_root(root_);
        if (!focused) return false;
        KeyEvent event;
        event.key = win_input::key_code_from_virtual_key(
            static_cast<uint32_t>(wp));
        event.modifiers = keyboard_modifiers();
        event.is_down = is_down;
        event.is_repeat = is_down && ((static_cast<uintptr_t>(lp) & (1u << 30)) != 0);
        if (event.key == KeyCode::unknown) return false;
        const bool consumed = focused->on_key_event(event);
        if (consumed) request_repaint_from_input();
        return consumed;
    }

    bool handle_text(WPARAM wp) {
        auto* focused = focused_input_under_root(root_);
        if (!focused || !focused->accepts_text_input()) return false;
        const wchar_t unit = static_cast<wchar_t>(wp);
        // Editing/navigation controls are already delivered through KeyEvent.
        // WM_CHAR repeats them as C0 code units; inserting those into the text
        // channel would double-handle Backspace/Tab/Return/Escape.
        if (unit < 0x20) return true;
        wchar_t utf16[2]{};
        int count = 1;
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            pending_high_surrogate_ = unit;
            return true;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF && pending_high_surrogate_) {
            utf16[0] = pending_high_surrogate_;
            utf16[1] = unit;
            count = 2;
        } else {
            utf16[0] = unit;
        }
        pending_high_surrogate_ = 0;
        // Lives in the drop-target header: the OLE path owns the only other
        // caller, and this codebase's Windows TUs each carry their own copy of
        // this WideCharToMultiByte wrapper (winmidi_device, clipboard_win,
        // file_dialog_win, environment_win, wasapi_device all do). Sharing it
        // across those five is a worthwhile cleanup but a separate one.
        const std::string text = win_drop::wide_to_utf8(utf16, count);
        if (text.empty()) return true;
        focused->on_text_input(TextInputEvent{.text = text});
        request_repaint_from_input();
        return true;
    }

    void handle_focus_changed(bool gained) {
        if (gained) return;
        pending_high_surrogate_ = 0;
        if (focused_input_under_root(root_)) {
            transfer_input_focus(root_, nullptr);
            request_repaint_from_input();
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
    bool use_gpu_ = false;
    win_input::SurfaceLifecycle surface_lifecycle_;
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
    win_input::PointerSession pointer_session_;
    Point last_pointer_point_{};
    bool tracking_mouse_leave_ = false;
    wchar_t pending_high_surrogate_ = 0;
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

    void release_capture_if_owned() {
        // State is terminal before ReleaseCapture because it synchronously
        // sends WM_CAPTURECHANGED back through this wndproc on Windows.
        if (GetCapture() == hwnd_) ReleaseCapture();
    }

    bool yield_to_gesture(const MouseEvent& event) {
        if (event.button != MouseButton::left)
            return false;
        const uint64_t generation = pointer_session_.generation();
        const bool yielded = should_yield_to_gesture(root_, event);
        // Gesture callbacks may pump a nested message. Stop this outer frame
        // if that message terminalized/replaced the session; never stamp its
        // result onto the newer generation.
        if (pointer_session_.generation() != generation ||
            !pointer_session_.active())
            return true;
        if (!yielded) return false;

        // Publish claimed and clear raw delivery BEFORE synchronous handoff.
        pointer_session_.mark_claimed();
        View* handoff_target = drag_target_;
        drag_target_ = nullptr;
        if (event.phase != MousePhase::press)
            deliver_gesture_handoff(root_, handoff_target,
                                    event.window_position, event.modifiers, 1);
        return true;
    }

    void cancel_pointer_gesture() noexcept {
        View* target = drag_target_;
        const auto terminal = pointer_session_.terminalize();
        if (!target && !terminal.cancel_gesture && !terminal.owns_terminal)
            return;

        drag_target_ = nullptr;
        try {
            if (terminal.cancel_gesture) {
                MouseEvent event;
                event.position = last_pointer_point_;
                event.window_position = last_pointer_point_;
                event.button = terminal.button;
                event.is_down = false;
                event.phase = MousePhase::release;
                event.is_cancelled = true;
                root_.dispatch_gesture_pointer_event(event);
            }
            if (target) {
                // Empty fire_click deliberately suppresses click on cancellation,
                // while still balancing legacy and modern press/release state.
                deliver_mouse_up(root_, target, last_pointer_point_, 0, 1,
                                 MouseUpHost{}, terminal.button);
            }
        } catch (const std::exception& e) {
            runtime::log_warn("WinPluginViewHost: pointer cancellation threw: {}",
                              e.what());
        } catch (...) {
            runtime::log_warn("WinPluginViewHost: pointer cancellation threw");
        }
        if (terminal.owns_terminal &&
            pointer_session_.phase() ==
                win_input::PointerSession::Phase::terminal &&
            pointer_session_.generation() == terminal.generation) {
            release_capture_if_owned();
            pointer_session_.finish_terminal(terminal);
        }
        request_repaint_from_input();
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

    static uint16_t keyboard_modifiers() {
        const auto down = [](int key) {
            return (GetKeyState(key) & 0x8000) != 0;
        };
        return win_input::key_modifiers(
            down(VK_SHIFT), down(VK_CONTROL), down(VK_MENU),
            down(VK_LWIN) || down(VK_RWIN));
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
    win_drop::PulpWinDropTarget* drop_target_ = nullptr;

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
        drop_target_ = new win_drop::PulpWinDropTarget(
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
    PluginFrameRenderer frame_renderer_;

    // Tear the surface pair down and tell every consumer BEFORE the objects
    // die. Consumers hold a raw GpuSurface*; publishing after the reset would
    // hand them a window in which the pointer is dangling but still "current".
    //
    // `next_state` is not cosmetic. `unavailable` means "there will never be a
    // surface", which is what the CPU-fallback diagnostic warns on — so a
    // detach that is going to rebuild on the next attach must publish
    // `pending` instead, or closing an editor logs a false gpu-init-failed
    // error every time.
    void release_gpu_surfaces(GpuSurfaceState next_state) {
        publish_gpu_surface(nullptr, next_state);
        skia_surface_.reset();
        gpu_surface_.reset();
    }

    // Rebuild the surface pair in place, for a drawable that went bad while the
    // editor stayed attached (device lost, a resize race). The HWND already has
    // its final parent and style, so this is safe to do mid-session — which is
    // exactly what makes it different from the attach-time creation path.
    void recreate_gpu_surfaces() {
        release_gpu_surfaces(GpuSurfaceState::pending);
        init_gpu(static_cast<float>(size_.width),
                 static_cast<float>(size_.height));
        if (!gpu_surface_ || !skia_surface_) {
            release_gpu_surfaces(GpuSurfaceState::unavailable);
            surface_lifecycle_.note_creation_failed();
        }
    }

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
        // The surface is live: tell the scripted UI (navigator.gpu) and anyone
        // else waiting. Publishing here rather than making consumers poll is
        // what fixes the Windows case — every format adapter read
        // gpu_surface() at editor-open time, which on Windows is BEFORE this
        // point, got null, and never looked again.
        publish_gpu_surface(gpu_surface_.get(), GpuSurfaceState::ready);
        runtime::log_info("WinPluginViewHost: GPU and Skia surfaces ready");
    }

    // The size/scale/viewport this frame paints against. One value so the
    // shared renderer's paint body and clip mapping cannot disagree.
    FrameGeometry frame_geometry() const {
        FrameGeometry g;
        g.width = static_cast<float>(size_.width);
        g.height = static_cast<float>(size_.height);
        g.scale = scale_;
        g.design_width = design_viewport_w_;
        g.design_height = design_viewport_h_;
        g.design_top_align = design_top_align_;
        return g;
    }

    // Scene paint. The body lives in the shared PluginFrameRenderer so Windows
    // and Linux paint identically; the trace scope stays here because the
    // nested layout/paint spans must land inside THIS host's frame span for
    // layout-vs-paint attribution to work in a Windows trace.
    void paint_scene(canvas::Canvas& canvas, const Rect* clip = nullptr) {
        PULP_TRACE_SCOPE_NAMED("canvas", "paint");
        paint_plugin_scene(canvas, root_, frame_geometry(), clip);
    }

    bool render_frame(std::vector<uint8_t>* cap, uint32_t* cap_w, uint32_t* cap_h) {
        // The real per-frame drive point (paint -> submit -> present below).
        // Mirrors the macOS host so a Windows trace is readable with the same
        // queries; without it the Windows editor showed only gpu_submit /
        // gpu_present and there was no frame span to attribute them to.
        PULP_TRACE_SCOPE_NAMED("render", "frame");
        if (!gpu_surface_ || !skia_surface_) return false;

        PluginFrameRenderer::Request request;
        request.root = &root_;
        request.geometry = frame_geometry();
        request.partial_repaint = partial_repaint_enabled_;
        request.idle = idle_callback_;
        request.capture = cap;
        request.capture_width = cap_w;
        request.capture_height = cap_h;

        const auto frame =
            frame_renderer_.render(*gpu_surface_, *skia_surface_, damage_, request);

        // A frame that never reached the drawable must not be reported as a
        // rendered frame, and must not retire its damage — the shared renderer
        // already put the damage back. React to a broken drawable here, where
        // the native surface lives.
        if (frame.should_recreate_surface) {
            runtime::log_warn(
                "WinPluginViewHost: frame did not reach the drawable "
                "(attempt {} of {}); recreating GPU surfaces",
                frame_renderer_.consecutive_recreates(),
                PluginFrameRenderer::kMaxConsecutiveRecreates);
            recreate_gpu_surfaces();
        } else if (frame.gpu_path_exhausted) {
            runtime::log_error(
                "WinPluginViewHost: GPU presentation failed {} times in a row; "
                "falling back to CPU raster for this editor",
                PluginFrameRenderer::kMaxConsecutiveRecreates);
            release_gpu_surfaces(GpuSurfaceState::unavailable);
            // Keep the lifecycle state honest: there are no surfaces now, so a
            // later detach/attach cycle must rebuild rather than assume a live
            // pair.
            surface_lifecycle_.note_creation_failed();
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        }

        if (!frame.reached_output()) return false;
        // A failed/empty readback must report false so capture_back_buffer_png()
        // falls back to the raster path instead of returning a blank frame.
        return cap ? frame.readback_ok : true;
    }

    // Pure-CPU raster capture, GPU-independent — the VM proof path. Sized at
    // PHYSICAL pixels (logical × scale) with the logical→pixel scale applied as
    // a canvas transform, so paint_scene keeps working in logical units and the
    // capture is crisp on HiDPI / matches the GPU surface pixel resolution.
    std::vector<uint8_t> raster_render_rgba(uint32_t* out_w,
                                            uint32_t* out_h) {
        const uint32_t w = pixel_w(), h = pixel_h();
        if (w == 0 || h == 0) return {};
        if (idle_callback_) idle_callback_();
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
        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
        return pixels;
    }

    std::vector<uint8_t> raster_capture_png() {
        uint32_t w = 0, h = 0;
        auto pixels = raster_render_rgba(&w, &h);
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
    if (host && (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN ||
                 msg == WM_MBUTTONDOWN)) {
        host->handle_mouse_down(
            lp, wp, win_input::mouse_button_from_message(msg));
        return 0;
    }
    if (host && msg == WM_MOUSEMOVE) {
        host->handle_mouse_move(lp, wp);
        return 0;
    }
    if (host && (msg == WM_LBUTTONUP || msg == WM_RBUTTONUP ||
                 msg == WM_MBUTTONUP)) {
        host->handle_mouse_up(
            lp, wp, win_input::mouse_button_from_message(msg));
        return 0;
    }
    if (host && msg == WM_MOUSELEAVE) {
        host->handle_mouse_leave();
        return 0;
    }
    if (host && (msg == WM_CAPTURECHANGED || msg == WM_CANCELMODE)) {
        host->handle_capture_lost();
        return 0;
    }
    if (host && (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL)) {
        host->handle_mouse_wheel(lp, wp, msg == WM_MOUSEHWHEEL);
        return 0;
    }
    if (host && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)) {
        if (host->handle_key(wp, lp, true)) return 0;
    }
    if (host && (msg == WM_KEYUP || msg == WM_SYSKEYUP)) {
        if (host->handle_key(wp, lp, false)) return 0;
    }
    if (host && msg == WM_CHAR) {
        if (host->handle_text(wp)) return 0;
    }
    if (host && msg == WM_SETFOCUS) {
        host->handle_focus_changed(true);
        return 0;
    }
    if (host && msg == WM_KILLFOCUS) {
        host->handle_focus_changed(false);
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
                return std::make_unique<WinPluginViewHost>(
                    root, opts.size, opts.use_gpu);
            });
    });
}

}  // namespace pulp::view
