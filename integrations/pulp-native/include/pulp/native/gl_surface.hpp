#pragma once

/// @file gl_surface.hpp
/// An OpenGL native surface for an editor that brings its own renderer.
///
/// `GlSurface` is the OpenGL counterpart of `MetalSurface`. It owns a child
/// window with a GL-capable pixel format and that window's GL context, hands
/// the context to the client, and leaves rendering and buffer swaps to the
/// client. It names no Pulp view, canvas, render or GPU-runtime type.
///
/// The context belongs to the client's render thread. `GlContext::make_current`
/// creates it, on first use, on the thread that calls it, so the host's UI
/// thread never binds it. The surface itself never swaps.
///
/// Backends:
/// - `create_offscreen`: a context with no window, drawing into a
///   framebuffer object of a fixed size. On macOS it uses CGL, for tests and
///   offline capture only (OpenGL is deprecated there; editors use
///   `MetalSurface`).
/// - `create`: a child window of the host's window. It is null where no
///   windowed backend exists yet (macOS, and Windows and X11 until those
///   backends land).
///
/// Threading:
/// - Construction, `attach`, `detach`, `invalidate`, the setters and
///   destruction run on the UI thread.
/// - `geometry()` may run on any thread. Everything on `GlContext` runs on the
///   client's render thread.
///
/// Teardown is two-phase, as for `MetalSurface`. Teardown first stops new
/// work: `make_current` refuses and `swap_buffers` returns false. Then the
/// release handler runs on the UI thread and returns once the client has
/// stopped using the context (it joins its render thread). Only then is the
/// context destroyed.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <pulp/native/surface.hpp>

namespace pulp::native {

enum class GlProfile : std::uint8_t {
    /// A core profile context.
    core = 0,
    /// A compatibility profile context (fixed-function entry points kept).
    compatibility = 1,
    /// Either; used for OpenGL 2.1, which predates profiles.
    any = 2,
};

/// One entry of the client's ordered list of acceptable contexts.
struct GlContextRequest {
    int major = 3;
    int minor = 3;
    GlProfile profile = GlProfile::core;
    /// Samples per pixel for multisampling; 0 turns it off.
    int samples = 0;
};

/// What a created context actually is, read back from the driver.
struct GlContextReport {
    int major = 0;
    int minor = 0;
    GlProfile profile = GlProfile::any;
    /// `GL_VERSION`, `GL_RENDERER` and `GL_VENDOR` as the driver reports them.
    std::string version;
    std::string renderer;
    std::string vendor;
    int max_texture_size = 0;
    bool npot_textures = false;
    /// A software rasterizer (llvmpipe, softpipe, GDI Generic, Apple Software
    /// Renderer); a client may prefer its CPU renderer over it.
    bool software = false;
    /// Which entry of `GlSurfaceOptions::requests` was honoured.
    std::size_t request_index = 0;
};

struct GlSurfaceOptions {
    /// Tried in order; the first the driver can honour wins. A request above
    /// what the driver offers falls through to the next entry rather than
    /// being clamped.
    std::vector<GlContextRequest> requests{
        {3, 3, GlProfile::core, 0},
        {3, 0, GlProfile::compatibility, 0},
        {2, 1, GlProfile::any, 0},
    };
    DrawableSizing sizing = DrawableSizing::automatic;
    /// 0 swaps immediately; 1 waits for vertical blank.
    int swap_interval = 1;
};

/// The index of the first request a context of version `major.minor` with
/// `profile` honours: the version is at least the request's, and the profile
/// matches unless the request accepts any. Pure; exposed so a backend's
/// choice can be checked without a GL driver.
std::optional<std::size_t> first_honoured_request(const std::vector<GlContextRequest>& requests,
                                                  int major, int minor, GlProfile profile);

/// Whether a `GL_RENDERER` string names a software rasterizer.
bool is_software_renderer(const std::string& renderer);

/// Short stable name ("core", "compatibility", "any").
const char* to_string(GlProfile profile) noexcept;

class GlSurface;

/// The client's handle to a surface's context. Every member runs on the
/// client's render thread.
class GlContext {
  public:
    /// Make the context current on this thread, creating it on first use from
    /// the surface's request list. False when no request can be honoured, or
    /// once teardown has started.
    bool make_current();
    /// Unbind the context from this thread.
    void release_current();
    /// Present the frame: swap buffers on a window, finish on an offscreen
    /// surface. False once teardown has started.
    bool swap_buffers();
    /// 0 or 1. False when the backend cannot set it.
    bool set_swap_interval(int interval);
    /// An entry point of the current context, or null.
    void* get_proc_address(const char* name) const;
    /// What the driver gave. Valid after the first successful `make_current`.
    const GlContextReport& report() const noexcept;
    /// For an offscreen surface, the framebuffer object to draw into; 0 for a
    /// window (draw to the default framebuffer).
    std::uint32_t framebuffer() const noexcept;

  private:
    friend class GlSurface;
    explicit GlContext(GlSurface& owner) : owner_(owner) {}
    GlSurface& owner_;
};

class GlSurface {
  public:
    /// A surface for a child window of the host's window, or null where this
    /// platform has no windowed GL backend. UI thread.
    static std::unique_ptr<GlSurface> create(const GlSurfaceOptions& options = {});
    /// A windowless surface drawing into a `width` x `height` framebuffer
    /// object, or null when the platform has no offscreen GL. UI thread.
    static std::unique_ptr<GlSurface> create_offscreen(std::uint32_t width, std::uint32_t height,
                                                       const GlSurfaceOptions& options = {});

    ~GlSurface();
    GlSurface(const GlSurface&) = delete;
    GlSurface& operator=(const GlSurface&) = delete;

    /// Add the surface's child window to `parent` (an `HWND`, or an X `Window`
    /// id cast to a pointer). False for an offscreen surface, when already
    /// attached, after `invalidate()`, or when `parent` is null.
    bool attach(void* parent);
    /// Two-phase teardown, then remove the child window. A no-op when not
    /// attached.
    void detach();
    /// The parent is already gone. Run the release handler and drop the child
    /// window without touching the parent. Idempotent.
    void invalidate();
    bool attached() const noexcept;
    bool offscreen() const noexcept;

    /// The context the client renders with.
    GlContext& context() noexcept;

    /// The latest consistent size snapshot. Any thread.
    SurfaceGeometry geometry() const;
    /// Called on the UI thread for each `SurfaceEvent`.
    void set_listener(std::function<void(const SurfaceEvent&)> listener);
    /// Called on the UI thread at the start of teardown. It must return only
    /// after the client has stopped using the context.
    void set_release_handler(std::function<void()> handler);

    struct Impl;

  private:
    friend class GlContext;
    explicit GlSurface(std::unique_ptr<Impl> impl);
    void begin_teardown();

    std::unique_ptr<Impl> impl_;
    GlContext context_{*this};
};

} // namespace pulp::native
