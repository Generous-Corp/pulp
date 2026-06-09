// Linux (X11) PluginViewHost — native child-window editor host for the
// foreign-host embed SDK + the VST3/CLAP adapters on Linux (#3329 Win/Linux
// parity). Mirrors the macOS/Windows hosts (NSView/HWND -> X11 child Window).
//
// Render paths (one class):
//   - GPU: a Dawn surface built from the child X11 window via the typed
//     GpuSurface::X11NativeHandle (Display* + Window), wrapped by SkiaSurface.
//     Requires a working WebGPU/Vulkan backend; absent one, is_gpu_backed() is
//     false and the host falls back to:
//   - CPU raster present: Skia raster -> XImage -> XPutImage, so a live child
//     window shows real content even with no GPU (llvmpipe-free).
//   - Headless capture: capture_back_buffer_png() always works via Skia raster
//     — the VM-verifiable proof that needs no X server at all for the pixels
//     (the embed's render_to_rgba path is the primary VM proof; this host's
//     capture is the live-window analog).
//
// X11 caveats (Codex review): a bare Window id is not enough for Dawn — we keep
// a Display*. We open our own connection with XOpenDisplay(nullptr) for the
// proof-level path; robust DAW interop would take the host's Display from the
// format adapter. There is no internal event loop here — the host pumps its own
// X connection; we only create/reparent/map/blit on the UI thread. Visual is
// CopyFromParent so colormap/depth match the parent.

#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/view/drag_drop.hpp>

#include "xdnd_parse.hpp"

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

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pulp::view {

namespace {

class X11PluginViewHost : public PluginViewHost {
public:
    X11PluginViewHost(View& root, Size size) : root_(root), size_(size) {
        // Our own X connection (proof-level). Xlib must be touched from a single
        // thread; this host is created and driven on the UI thread.
        display_ = XOpenDisplay(nullptr);
        if (!display_) {
            runtime::log_warn("X11PluginViewHost: XOpenDisplay(nullptr) failed "
                              "(no DISPLAY?) — headless capture only");
            return;
        }
        screen_ = DefaultScreen(display_);
        Window root_win = RootWindow(display_, screen_);
        // Create the child as a child of the root for now; reparented to the
        // DAW's editor window in attach_to_parent(). CopyFromParent matches the
        // parent's visual/depth/colormap.
        child_ = XCreateSimpleWindow(
            display_, root_win, 0, 0, size.width, size.height, 0,
            BlackPixel(display_, screen_), BlackPixel(display_, screen_));
        if (!child_) {
            runtime::log_warn("X11PluginViewHost: XCreateSimpleWindow failed");
            return;
        }
        // PropertyChangeMask is needed so the SelectionNotify path can read
        // back the XdndSelection property the source writes; StructureNotify +
        // Exposure keep the existing repaint/reparent behavior.
        XSelectInput(display_, child_,
                     ExposureMask | StructureNotifyMask | PropertyChangeMask);
        gc_ = XCreateGC(display_, child_, 0, nullptr);
        init_xdnd();
        XFlush(display_);
#ifdef PULP_HAS_SKIA
        init_gpu(static_cast<float>(size.width), static_cast<float>(size.height));
#endif
    }

    ~X11PluginViewHost() override {
        root_.set_plugin_view_host(nullptr);
#ifdef PULP_HAS_SKIA
        skia_surface_.reset();
        gpu_surface_.reset();
#endif
        if (display_) {
            if (gc_) XFreeGC(display_, gc_);
            if (child_) XDestroyWindow(display_, child_);
            XFlush(display_);
            XCloseDisplay(display_);
            display_ = nullptr;
        }
    }

    // native_handle returns the X11 Window id cast to void* (the cross-format
    // convention used by clap_entry/vst3 on Linux, which pass only the Window).
    NativeViewHandle native_handle() override {
        return reinterpret_cast<void*>(static_cast<uintptr_t>(child_));
    }

    void attach_to_parent(NativeViewHandle parent) override {
        if (!display_ || !child_) return;
        Window parent_win = static_cast<Window>(reinterpret_cast<uintptr_t>(parent));
        if (!parent_win) return;
        XReparentWindow(display_, child_, parent_win, 0, 0);
        XResizeWindow(display_, child_, size_.width, size_.height);
        XMapWindow(display_, child_);
        XFlush(display_);
        attached_.store(true, std::memory_order_release);
        repaint();
    }

    bool is_attached() const noexcept {
        return attached_.load(std::memory_order_acquire);
    }

    void detach() override {
        if (!display_ || !child_) return;
        XUnmapWindow(display_, child_);
        Window root_win = RootWindow(display_, screen_);
        XReparentWindow(display_, child_, root_win, 0, 0);
        XFlush(display_);
        attached_.store(false, std::memory_order_release);
    }

    void repaint() override {
        // Service drag-and-drop on our own X connection. The standalone host
        // repaints ~each frame, so this is the steady-state XDND pump; a
        // DAW-embedded driver can also call pump_x_events() directly.
        pump_x_events();
#ifdef PULP_HAS_SKIA
        if (gpu_surface_ && skia_surface_) {
            render_frame_gpu(nullptr, nullptr, nullptr);
            return;
        }
        // No GPU: present a raster frame into the live window if attached.
        if (display_ && child_ && attached_.load(std::memory_order_acquire)) {
            present_raster();
        }
#endif
    }

    void set_size(uint32_t width, uint32_t height) override {
        size_ = {width, height};
        if (display_ && child_) {
            XResizeWindow(display_, child_, width, height);
            XFlush(display_);
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

    std::vector<uint8_t> capture_back_buffer_png() override {
#ifdef PULP_HAS_SKIA
        if (gpu_surface_ && skia_surface_) {
            std::vector<uint8_t> pixels;
            uint32_t pw = 0, ph = 0;
            if (render_frame_gpu(&pixels, &pw, &ph) && !pixels.empty())
                return encode_rgba_png(pixels, pw, ph);
        }
        uint32_t pw = 0, ph = 0;
        auto pixels = raster_render(&pw, &ph);
        if (pixels.empty()) return {};
        return encode_rgba_png(pixels, pw, ph);
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

    // Drain pending X events on our own connection and run the XDND target
    // protocol (drag-and-drop from another X client onto the editor child
    // window). There is no internal event loop in this host, so a driver must
    // call this — the standalone Linux GPU host pumps it each frame via
    // repaint(), and a DAW-embedded host can call it from its editor idle hook.
    // Honest no-op when display_/child_ are null (headless capture mode).
    void pump_x_events() override {
        if (!display_ || !child_) return;
        while (XPending(display_) > 0) {
            XEvent ev;
            XNextEvent(display_, &ev);
            switch (ev.type) {
                case ClientMessage:
                    handle_client_message(ev.xclient);
                    break;
                case SelectionNotify:
                    handle_selection_notify(ev.xselection);
                    break;
                default:
                    break;
            }
        }
    }

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
    Display* display_ = nullptr;
    int screen_ = 0;
    Window child_ = 0;
    GC gc_ = nullptr;
    std::atomic<bool> attached_{false};
    std::function<void()> idle_callback_;
    std::function<void(uint32_t, uint32_t)> resize_cb_;
    float design_viewport_w_ = 0.0f;
    float design_viewport_h_ = 0.0f;
    float fixed_aspect_ratio_ = 0.0f;
    bool design_top_align_ = false;

    // ── XDND (X11 drag-and-drop) target state ────────────────────────────
    // Atoms interned once in init_xdnd(). 0 (None) until interned.
    Atom xa_XdndAware_ = 0;
    Atom xa_XdndEnter_ = 0;
    Atom xa_XdndPosition_ = 0;
    Atom xa_XdndStatus_ = 0;
    Atom xa_XdndLeave_ = 0;
    Atom xa_XdndDrop_ = 0;
    Atom xa_XdndFinished_ = 0;
    Atom xa_XdndSelection_ = 0;
    Atom xa_XdndActionCopy_ = 0;
    Atom xa_XdndTypeList_ = 0;
    Atom xa_text_uri_list_ = 0;
    Atom xa_UTF8_STRING_ = 0;
    Atom xa_pulp_xdnd_prop_ = 0;  // our property the selection lands in

    // Per-drag hover state owned by this host (one in-flight pointer), routed
    // through the shared dispatch core — never a process global.
    DragSession drag_session_;

    // In-flight XDND source/transfer state, valid between XdndEnter and the
    // XdndFinished we send after a drop. Reset on Leave / after Finished.
    Window xdnd_source_ = 0;          // the source client's window
    int xdnd_source_version_ = 0;     // min(our v5, source's advertised version)
    Atom xdnd_chosen_type_ = 0;       // type we asked the source to convert to
    int xdnd_drop_root_x_ = 0;        // last XdndPosition pointer (root coords)
    int xdnd_drop_root_y_ = 0;
    bool xdnd_drop_pending_ = false;  // awaiting SelectionNotify after XdndDrop

    // Intern the XDND atoms and advertise the child window as an XDND-aware
    // target (version 5). Safe no-op without display_/child_.
    void init_xdnd() {
        if (!display_ || !child_) return;
        xa_XdndAware_ = XInternAtom(display_, "XdndAware", False);
        xa_XdndEnter_ = XInternAtom(display_, "XdndEnter", False);
        xa_XdndPosition_ = XInternAtom(display_, "XdndPosition", False);
        xa_XdndStatus_ = XInternAtom(display_, "XdndStatus", False);
        xa_XdndLeave_ = XInternAtom(display_, "XdndLeave", False);
        xa_XdndDrop_ = XInternAtom(display_, "XdndDrop", False);
        xa_XdndFinished_ = XInternAtom(display_, "XdndFinished", False);
        xa_XdndSelection_ = XInternAtom(display_, "XdndSelection", False);
        xa_XdndActionCopy_ = XInternAtom(display_, "XdndActionCopy", False);
        xa_XdndTypeList_ = XInternAtom(display_, "XdndTypeList", False);
        xa_text_uri_list_ = XInternAtom(display_, "text/uri-list", False);
        xa_UTF8_STRING_ = XInternAtom(display_, "UTF8_STRING", False);
        xa_pulp_xdnd_prop_ = XInternAtom(display_, "PULP_XDND_SELECTION", False);

        // Advertise XDND v5 support on the child window.
        const long version = 5;
        XChangeProperty(display_, child_, xa_XdndAware_, XA_ATOM, 32,
                        PropModeReplace,
                        reinterpret_cast<const unsigned char*>(&version), 1);
    }

    // Read the absolute (root-window) origin of the child window so XdndPosition
    // root coordinates can be converted to child-local coordinates.
    void child_root_origin(int& x, int& y) const {
        x = 0;
        y = 0;
        if (!display_ || !child_) return;
        Window root_win = RootWindow(display_, screen_);
        Window dummy_child = 0;
        int rx = 0, ry = 0;
        if (XTranslateCoordinates(display_, child_, root_win, 0, 0, &rx, &ry,
                                  &dummy_child)) {
            x = rx;
            y = ry;
        }
    }

    // Map an XdndPosition root point to the root-view coordinate space the
    // dispatch core expects: root → child-local (subtract child origin) →
    // window_to_root_point (design-viewport inverse). Mirrors the mac producer.
    Point xdnd_root_to_view(int root_x, int root_y) const {
        int ox = 0, oy = 0;
        child_root_origin(ox, oy);
        Point local = xdnd::root_to_child_local(root_x, root_y, ox, oy);
        return window_to_root_point(local);
    }

    // Read the source's offered types. XdndEnter packs up to 3 types inline
    // (data.l[2..4]); when the low bit of data.l[1] is set, the full list lives
    // in the XdndTypeList property on the source window.
    std::vector<Atom> read_offered_types(const XClientMessageEvent& msg) {
        std::vector<Atom> types;
        const bool more = (msg.data.l[1] & 1) != 0;
        if (more) {
            Window source = static_cast<Window>(msg.data.l[0]);
            Atom actual_type = 0;
            int actual_format = 0;
            unsigned long nitems = 0, bytes_after = 0;
            unsigned char* prop = nullptr;
            if (XGetWindowProperty(display_, source, xa_XdndTypeList_, 0, 1024,
                                   False, XA_ATOM, &actual_type, &actual_format,
                                   &nitems, &bytes_after, &prop) == Success &&
                prop) {
                const Atom* atoms = reinterpret_cast<const Atom*>(prop);
                for (unsigned long i = 0; i < nitems; ++i) types.push_back(atoms[i]);
                XFree(prop);
            }
        } else {
            for (int i = 2; i <= 4; ++i) {
                Atom a = static_cast<Atom>(msg.data.l[i]);
                if (a != None) types.push_back(a);
            }
        }
        return types;
    }

    // Pick the best target type from the offered list: prefer files
    // (text/uri-list), fall back to UTF8 text. None (0) if neither is offered.
    Atom choose_type(const std::vector<Atom>& offered) const {
        for (Atom a : offered)
            if (a == xa_text_uri_list_) return xa_text_uri_list_;
        for (Atom a : offered)
            if (a == xa_UTF8_STRING_) return xa_UTF8_STRING_;
        return None;
    }

    void send_xdnd_status(Window source, bool accept) {
        XEvent reply;
        std::memset(&reply, 0, sizeof(reply));
        reply.xclient.type = ClientMessage;
        reply.xclient.display = display_;
        reply.xclient.window = source;
        reply.xclient.message_type = xa_XdndStatus_;
        reply.xclient.format = 32;
        reply.xclient.data.l[0] = static_cast<long>(child_);
        // bit0: accept; bit1: send XdndPosition for every motion (we set it so
        // we keep getting position updates for hover feedback).
        reply.xclient.data.l[1] = accept ? 3 : 0;
        reply.xclient.data.l[2] = 0;  // empty rect → always send Position
        reply.xclient.data.l[3] = 0;
        reply.xclient.data.l[4] =
            accept ? static_cast<long>(xa_XdndActionCopy_) : 0;
        XSendEvent(display_, source, False, NoEventMask, &reply);
        XFlush(display_);
    }

    void send_xdnd_finished(Window source, bool accepted) {
        XEvent reply;
        std::memset(&reply, 0, sizeof(reply));
        reply.xclient.type = ClientMessage;
        reply.xclient.display = display_;
        reply.xclient.window = source;
        reply.xclient.message_type = xa_XdndFinished_;
        reply.xclient.format = 32;
        reply.xclient.data.l[0] = static_cast<long>(child_);
        // v5: bit0 = accepted; data.l[2] = action performed.
        reply.xclient.data.l[1] = accepted ? 1 : 0;
        reply.xclient.data.l[2] =
            accepted ? static_cast<long>(xa_XdndActionCopy_) : 0;
        XSendEvent(display_, source, False, NoEventMask, &reply);
        XFlush(display_);
    }

    void reset_xdnd_drag() {
        xdnd_source_ = 0;
        xdnd_source_version_ = 0;
        xdnd_chosen_type_ = 0;
        xdnd_drop_pending_ = false;
        dispatch_drag_exit(root_, drag_session_);
    }

    void handle_client_message(const XClientMessageEvent& msg) {
        if (msg.message_type == xa_XdndEnter_) {
            xdnd_source_ = static_cast<Window>(msg.data.l[0]);
            xdnd_source_version_ =
                static_cast<int>((msg.data.l[1] >> 24) & 0xff);
            auto offered = read_offered_types(msg);
            xdnd_chosen_type_ = choose_type(offered);
            return;
        }
        if (msg.message_type == xa_XdndPosition_) {
            Window source = static_cast<Window>(msg.data.l[0]);
            xdnd_drop_root_x_ = static_cast<int>((msg.data.l[2] >> 16) & 0xffff);
            xdnd_drop_root_y_ = static_cast<int>(msg.data.l[2] & 0xffff);
            const bool can_accept = xdnd_chosen_type_ != None;
            if (can_accept) {
                // Hover feedback through the shared dispatch core. The data's
                // concrete payload isn't known until the drop, so feed a typed
                // shell (files vs text) so DropReceivers can highlight.
                DropData hover;
                hover.type = (xdnd_chosen_type_ == xa_text_uri_list_)
                                 ? DropData::Type::files
                                 : DropData::Type::text;
                dispatch_drag_move(root_, drag_session_, hover,
                                   xdnd_root_to_view(xdnd_drop_root_x_,
                                                     xdnd_drop_root_y_));
            }
            send_xdnd_status(source, can_accept);
            return;
        }
        if (msg.message_type == xa_XdndLeave_) {
            reset_xdnd_drag();
            return;
        }
        if (msg.message_type == xa_XdndDrop_) {
            Window source = static_cast<Window>(msg.data.l[0]);
            if (xdnd_chosen_type_ == None) {
                // Nothing droppable was offered — finish without accepting.
                send_xdnd_finished(source, false);
                reset_xdnd_drag();
                return;
            }
            xdnd_drop_pending_ = true;
            Time t = (xdnd_source_version_ >= 1)
                         ? static_cast<Time>(msg.data.l[2])
                         : CurrentTime;
            // Ask the source to convert the selection to our chosen type; the
            // payload arrives later as a SelectionNotify.
            XConvertSelection(display_, xa_XdndSelection_, xdnd_chosen_type_,
                              xa_pulp_xdnd_prop_, child_, t);
            XFlush(display_);
            return;
        }
    }

    void handle_selection_notify(const XSelectionEvent& sel) {
        if (!xdnd_drop_pending_) return;
        if (sel.property == None) {
            // Source failed to convert — finish unaccepted.
            if (xdnd_source_) send_xdnd_finished(xdnd_source_, false);
            reset_xdnd_drag();
            return;
        }

        Atom actual_type = 0;
        int actual_format = 0;
        unsigned long nitems = 0, bytes_after = 0;
        unsigned char* prop = nullptr;
        std::string payload;
        if (XGetWindowProperty(display_, child_, sel.property, 0, 0x7fffffff,
                               True /* delete */, AnyPropertyType, &actual_type,
                               &actual_format, &nitems, &bytes_after,
                               &prop) == Success &&
            prop) {
            payload.assign(reinterpret_cast<const char*>(prop),
                           static_cast<size_t>(nitems));
            XFree(prop);
        }

        DropData data;
        if (xdnd_chosen_type_ == xa_text_uri_list_) {
            data.type = DropData::Type::files;
            data.file_paths = xdnd::parse_uri_list(payload);
        } else {
            data.type = DropData::Type::text;
            data.text = payload;
        }

        const bool consumed = dispatch_drop(
            root_, drag_session_, data,
            xdnd_root_to_view(xdnd_drop_root_x_, xdnd_drop_root_y_));

        if (xdnd_source_) send_xdnd_finished(xdnd_source_, consumed);
        reset_xdnd_drag();
    }

#ifdef PULP_HAS_SKIA
    std::unique_ptr<render::GpuSurface> gpu_surface_;
    std::unique_ptr<render::SkiaSurface> skia_surface_;
    render::GpuSurface::X11NativeHandle x11_handle_{};

    void init_gpu(float width, float height) {
        if (!display_ || !child_) return;
        gpu_surface_ = render::GpuSurface::create_dawn();
        if (!gpu_surface_) {
            runtime::log_warn("X11PluginViewHost: gpu create_dawn failed; cpu raster only");
            return;
        }
        x11_handle_.display = display_;
        x11_handle_.window = static_cast<unsigned long>(child_);
        render::GpuSurface::Config cfg{};
        cfg.width = static_cast<uint32_t>(width);
        cfg.height = static_cast<uint32_t>(height);
        cfg.native_surface_handle = &x11_handle_;  // typed X11 handle
        if (!gpu_surface_->initialize(cfg)) {
            runtime::log_warn("X11PluginViewHost: gpu initialize failed; cpu raster only");
            gpu_surface_.reset();
            return;
        }
        render::SkiaSurface::Config scfg{};
        scfg.width = static_cast<uint32_t>(width);
        scfg.height = static_cast<uint32_t>(height);
        scfg.scale_factor = 1.0f;
        skia_surface_ = render::SkiaSurface::create(*gpu_surface_, scfg);
        if (!skia_surface_) {
            runtime::log_warn("X11PluginViewHost: skia surface create failed; cpu raster only");
            gpu_surface_.reset();
        }
    }

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

    bool render_frame_gpu(std::vector<uint8_t>* cap, uint32_t* cap_w, uint32_t* cap_h) {
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

    // Render the scene to RGBA via pure Skia raster (no GPU). out_w/out_h get
    // the pixel dims. Empty on failure.
    std::vector<uint8_t> raster_render(uint32_t* out_w, uint32_t* out_h) {
        const uint32_t w = size_.width, h = size_.height;
        if (w == 0 || h == 0) return {};
        if (idle_callback_) idle_callback_();
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
        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
        return pixels;
    }

    // Live CPU present: raster the scene, convert RGBA -> the X visual's pixel
    // format (assume 32-bit BGRX TrueColor, the common case), and XPutImage.
    void present_raster() {
        uint32_t w = 0, h = 0;
        auto rgba = raster_render(&w, &h);
        if (rgba.empty()) return;

        Visual* visual = DefaultVisual(display_, screen_);
        int depth = DefaultDepth(display_, screen_);
        // Build a 32-bit BGRA buffer for XImage (X expects BGRX on little-endian
        // TrueColor visuals). XDestroyImage frees this buffer.
        auto* buf = static_cast<char*>(malloc(static_cast<size_t>(w) * h * 4u));
        if (!buf) return;
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
            uint8_t r = rgba[i * 4 + 0], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2];
            buf[i * 4 + 0] = static_cast<char>(b);
            buf[i * 4 + 1] = static_cast<char>(g);
            buf[i * 4 + 2] = static_cast<char>(r);
            buf[i * 4 + 3] = 0;
        }
        XImage* img = XCreateImage(display_, visual, static_cast<unsigned>(depth),
                                   ZPixmap, 0, buf, w, h, 32, 0);
        if (!img) {
            free(buf);
            return;
        }
        XPutImage(display_, child_, gc_, img, 0, 0, 0, 0, w, h);
        XFlush(display_);
        XDestroyImage(img);  // frees buf
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
// X11 factory only if no factory is already registered.
void register_platform_plugin_view_host() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (PluginViewHost::has_factory()) return;
        PluginViewHost::set_factory(
            [](View& root, const PluginViewHost::Options& opts)
                -> std::unique_ptr<PluginViewHost> {
                return std::make_unique<X11PluginViewHost>(root, opts.size);
            });
    });
}

}  // namespace pulp::view
