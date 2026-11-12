#pragma once

// The backend side of GlSurface. A backend owns the context (and, when
// windowed, the child window); gl_surface.cpp owns the order of operations.

#include <pulp/native/gl_surface.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

namespace pulp::native {

struct GlSurface::Impl {
    virtual ~Impl() = default;

    GlSurfaceOptions options;
    GlContextReport report;
    bool created = false;
    std::atomic<bool> tearing_down{false};
    bool torn_down = false;
    bool invalidated = false;
    std::function<void()> release_handler;
    std::function<void(const SurfaceEvent&)> listener;
    mutable std::mutex geometry_mutex;
    SurfaceGeometry geometry;

    virtual bool offscreen() const = 0;

    // Render thread. Walk `options.requests`, create the first context that
    // honours one, leave it current, and fill `out`.
    virtual bool create_context(const GlSurfaceOptions& options, GlContextReport& out) = 0;
    virtual bool make_current() = 0;
    virtual void release_current() = 0;
    virtual bool swap() = 0;
    virtual bool set_interval(int interval) = 0;
    virtual void* proc(const char* name) const = 0;
    virtual std::uint32_t framebuffer() const {
        return 0;
    }
    // UI thread, after the release handler: the client no longer has the
    // context current anywhere.
    virtual void destroy_context() = 0;

    // Windowed backends.
    virtual bool attach(void*) {
        return false;
    }
    virtual bool attached() const {
        return false;
    }
    virtual void detach_window() {}
    virtual void forget_window() {}
    virtual void destroy_window() {}
};

// Defined by the backend files built for this platform; null where there is
// no such backend.
std::unique_ptr<GlSurface::Impl> make_windowed_gl_impl(const GlSurfaceOptions& options);
std::unique_ptr<GlSurface::Impl> make_offscreen_gl_impl(std::uint32_t width, std::uint32_t height,
                                                        const GlSurfaceOptions& options);

} // namespace pulp::native
