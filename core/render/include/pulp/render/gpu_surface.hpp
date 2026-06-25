#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace pulp::render {

/// Abstract GPU surface — a presentable rendering target backed by a native window/view.
///
/// Ownership model:
/// - Platform code creates native views/layers and passes handles here
/// - GpuSurface owns the Dawn instance, adapter, device, queue, and surface
/// - SkiaSurface receives the device/queue from GpuSurface (single device)
/// - begin_frame()/end_frame() bracket each rendered frame with acquire/present
///
/// Frame lifecycle:
///   gpu.begin_frame()   → acquires current presentable texture
///   skia.begin_frame()  → wraps that texture as Graphite render target
///   ... draw ...
///   skia.end_frame()    → submits Graphite recording to the shared device/queue
///   gpu.end_frame()     → presents to native surface
class GpuSurface {
public:
    enum class AdapterBackendPreference {
        default_backend,
        null_backend,
    };

    struct AdapterInfo {
        bool available = false;
        bool native_bridge = false;
        std::string backend = "unavailable";
        std::string backend_type = "Unknown";
        std::string name = "Mock Dawn Adapter";
        std::string vendor = "Pulp";
        std::string architecture = "unavailable";
        std::string description = "Mock Dawn Adapter";
        std::string preferred_canvas_format = "bgra8unorm";
    };

    struct Config {
        uint32_t width = 800;
        uint32_t height = 600;
        bool vsync = true;
        bool force_fallback_adapter = false;
        AdapterBackendPreference backend_preference =
            AdapterBackendPreference::default_backend;

        /// Opaque platform-specific handle for on-screen rendering.
        /// On macOS/iOS: CAMetalLayer*
        /// On Windows:   HWND
        /// On Linux:     a pointer to an X11NativeHandle (see below). A bare
        ///               X11 Window id is NOT enough — Dawn's Xlib surface
        ///               source needs both the Display* and the Window. Wayland
        ///               (wl_display*/wl_surface*) is not wired yet.
        /// nullptr = offscreen-only mode (no presentation)
        void* native_surface_handle = nullptr;
    };

    /// Typed X11 surface handle. Pass a pointer to one of these as
    /// Config::native_surface_handle on Linux so Dawn can build an
    /// SurfaceSourceXlibWindow. `display` is an `Display*` and `window` is an
    /// X11 `Window` (an `unsigned long`); both are kept opaque here so this
    /// header stays Xlib-include-free for non-Linux and no-X11 consumers. The
    /// pointed-to struct only needs to outlive the initialize() call.
    struct X11NativeHandle {
        void*         display = nullptr;  // Display*
        unsigned long window  = 0;        // Window
    };

    virtual ~GpuSurface() = default;

    virtual bool initialize(const Config& config) = 0;
    virtual void resize(uint32_t width, uint32_t height) = 0;

    /// Acquire the current presentable texture. Returns false if not ready.
    virtual bool begin_frame() = 0;

    /// Present to native surface (or just process events if offscreen).
    virtual void end_frame() = 0;

    virtual bool is_initialized() const = 0;
    virtual bool has_surface() const = 0;
    virtual uint32_t width() const = 0;
    virtual uint32_t height() const = 0;

    /// Opaque accessors for the Dawn device/queue/texture.
    /// SkiaSurface (in the same module) interprets these as Dawn C++ types.
    /// Returns nullptr if not initialized.
    virtual void* dawn_device_handle() const = 0;
    virtual void* dawn_queue_handle() const = 0;
    virtual void* dawn_instance_handle() const = 0;

    /// The current presentable texture, valid only between begin_frame/end_frame.
    /// Returns nullptr in offscreen mode or if begin_frame wasn't called.
    virtual void* current_texture_handle() const = 0;

    /// Snapshot describing the current adapter/device state for higher-level bridges.
    /// Returns an unavailable record when no native adapter has been initialized.
    virtual AdapterInfo adapter_info() const = 0;

    static std::unique_ptr<GpuSurface> create_dawn();
};

} // namespace pulp::render
