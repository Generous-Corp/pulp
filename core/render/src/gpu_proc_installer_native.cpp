// Pulp's default Dawn proc-table installer: Dawn's native procs.
//
// This is the only device source that calls dawnProcSetProcs. GpuSurface and
// GpuCompute reach it solely through ensure_dawn_procs(), which runs it at most
// once per process and never when a host registered its own installer.
#include <string>

#if defined(PULP_HAS_DAWN) && !defined(__EMSCRIPTEN__)
#include "dawn/dawn_proc.h"
#include "dawn/native/DawnNative.h"
#endif

namespace pulp::render::detail {

bool install_native_dawn_procs(void* /*context*/, std::string* error) {
#if defined(PULP_HAS_DAWN) && !defined(__EMSCRIPTEN__)
    (void)error;
    dawnProcSetProcs(&dawn::native::GetProcs());
    return true;
#else
    // Browser Dawn links its procs directly, and a build without Dawn creates
    // no Dawn object, so there is no table to install.
    if (error != nullptr)
        *error = "this build has no native Dawn proc table";
    return false;
#endif
}

} // namespace pulp::render::detail
