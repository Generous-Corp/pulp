// Launches the snap-DISABLED reference binary and decodes its blob.
//
// Test-only: this TU is linked into pulp-test-denormal-null (snap enabled), NOT
// into the refgen. It contains no filter code — the whole point of the split is
// that no executable links two different bodies of the same header-inline
// filter. See denormal_null_reference.hpp.

#include "denormal_null_reference.hpp"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

denormal_null::Reference denormal_null_run_reference() {
    const std::string out_path = PULP_DENORMAL_NULL_REF_BLOB;
    const std::string cmd =
        std::string("\"") + PULP_DENORMAL_NULL_REFGEN + "\" \"" + out_path + "\"";

    const int rc = std::system(cmd.c_str());
    if (rc != 0)
        throw std::runtime_error("denormal-null reference binary failed: " + cmd);

    denormal_null::Reference ref;
    if (!denormal_null::read_reference(out_path.c_str(), ref))
        throw std::runtime_error("could not decode denormal-null reference blob: " +
                                 out_path);

    std::remove(out_path.c_str());
    return ref;
}
