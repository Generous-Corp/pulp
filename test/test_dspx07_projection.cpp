#include "../core/format/src/projection_capability.hpp"
#include <cassert>

int main() {
    using namespace pulp::format;
    assert(projection_capability(ProjectionSurface::clap, true, true).supported());
    assert(projection_capability(ProjectionSurface::wam, true, true).supported());
    assert(!projection_capability(ProjectionSurface::clap, false, true).supported());
    assert(!projection_capability(ProjectionSurface::vst3, true, false).supported());
    assert(projection_capability(ProjectionSurface::au, true, true).supported());
    // Capability admission remains fail-closed for the two cases that cannot
    // be projected by any format adapter.
    assert(!projection_capability(ProjectionSurface::au, false, true).supported());
    assert(!projection_capability(ProjectionSurface::au, true, false).supported());
    return 0;
}
