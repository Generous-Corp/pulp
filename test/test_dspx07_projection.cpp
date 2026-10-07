#include "../core/format/src/projection_capability.hpp"
#include <cassert>

int main() {
    using namespace pulp::format;
    assert(projection_capability(ProjectionSurface::clap, true, true).supported());
    assert(projection_capability(ProjectionSurface::wam, true, true).supported());
    assert(!projection_capability(ProjectionSurface::clap, false, true).supported());
    assert(!projection_capability(ProjectionSurface::vst3, true, false).supported());
    assert(!projection_capability(ProjectionSurface::au, true, true).supported());
    return 0;
}
