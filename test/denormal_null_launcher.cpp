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

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

/// The blob path, made unique per process.
///
/// `catch_discover_tests` turns this file's cases into SEPARATE ctest tests, so
/// a parallel `ctest -j` runs several copies of this binary at once — and each
/// one regenerates, reads, and then deletes the blob. On one shared path that
/// races three ways: a reader can see a half-written file, or find it already
/// removed by a peer that finished first. It surfaced as an intermittent "could
/// not decode denormal-null reference blob", which reads like a numerical
/// failure in the filters and is nothing of the sort.
///
/// The PID suffix removes the race rather than serializing around it — a ctest
/// RESOURCE_LOCK would also work but would cost the parallelism for a file that
/// never needed to be shared in the first place.
std::string process_unique_blob_path() {
#ifdef _WIN32
    const auto pid = static_cast<long long>(_getpid());
#else
    const auto pid = static_cast<long long>(getpid());
#endif
    return std::string(PULP_DENORMAL_NULL_REF_BLOB) + "." + std::to_string(pid);
}

}  // namespace

denormal_null::Reference denormal_null_run_reference() {
    const std::string out_path = process_unique_blob_path();
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
