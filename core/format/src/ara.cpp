#include <pulp/format/ara.hpp>

namespace pulp::format {

bool host_supports_ara() {
    // ARA 2.0 requires the ARA SDK and host-side support.
    // Without the SDK integrated, ARA is not available.
    return false;
}

}  // namespace pulp::format
