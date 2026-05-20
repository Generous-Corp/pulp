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

        /// Opaque platform-specific handle for on-screen rendering.
        /// On macOS/iOS: CAMetalLayer*
        /// On Windows:   HWND
        /// On Linux:     platform-dependent (X11 Window, wl_surface*, etc.)
        /// nullptr = offscreen-only mode (no presentation)
        void* native_surface_handle = nullptr;
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

    /// Whether the initialized device supports WebGPU timestamp queries
    /// (the `timestamp-query` feature). Phase 6.5 uses this to decide
    /// whether the inspector perf view can show real GPU-side pass
    /// durations or must fall back to the Phase 6.1 CPU numbers. The
    /// feature is requested best-effort at device creation; an adapter
    /// that lacks it still yields a working surface — this just returns
    /// false. Always false before `initialize()` and on non-Dawn builds.
    virtual bool gpu_timestamps_supported() const { return false; }

    static std::unique_ptr<GpuSurface> create_dawn();
};

} // namespace pulp::render
