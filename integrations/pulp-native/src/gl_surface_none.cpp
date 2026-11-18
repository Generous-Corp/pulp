// GlSurface where no windowed or offscreen backend is built: both factories
// answer null, which clients already handle (they fall back to their CPU
// renderer).

#include "gl_surface_impl.h"

namespace pulp::native {

std::unique_ptr<GlSurface::Impl> make_windowed_gl_impl(const GlSurfaceOptions&) {
    return nullptr;
}

std::unique_ptr<GlSurface::Impl> make_offscreen_gl_impl(std::uint32_t, std::uint32_t,
                                                        const GlSurfaceOptions&) {
    return nullptr;
}

std::string windowed_gl_class_name() {
    return {};
}

} // namespace pulp::native
