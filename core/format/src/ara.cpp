#include <pulp/format/ara.hpp>

#ifdef PULP_HAS_ARA
#include <ARA_API/ARAInterface.h>
#endif

namespace pulp::format {

bool host_supports_ara() {
#ifdef PULP_HAS_ARA
    // SDK is compiled in; actual host-query lives in the format-adapter
    // companion factories (VST3 / AU / CLAP). Pulp-side availability is
    // gated by those slices landing — workstream 06 slices 6.3..6.5.
    return false;
#else
    return false;
#endif
}

bool ara_sdk_compiled_in() {
#ifdef PULP_HAS_ARA
    return true;
#else
    return false;
#endif
}

int ara_sdk_generation() {
#ifdef PULP_HAS_ARA
    // Newest generation defined by the headers. Callers compare to a
    // concrete constant like kARAAPIGeneration_2_3_Final (= 6) when
    // gating feature use. Touching the ARA::kARAAPIGeneration_2_3_Final
    // symbol here proves the SDK headers are reachable at compile time.
    return static_cast<int>(ARA::kARAAPIGeneration_2_3_Final);
#else
    return 0;
#endif
}

}  // namespace pulp::format
