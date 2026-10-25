// A device library built with PULP_DAWN_PROCS_HOST_OWNED, with the host
// installer registered before any GPU object exists: the supported order. The
// host's installer then decides, and nothing else ever installs the table.
//
// Its own executable: the policy is process-global, and the companion case
// (test_gpu_proc_installer_host_owned.cpp) seals it before registering.

#include <pulp/render/gpu_proc_installer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>

namespace {

std::atomic<int> host_calls{0};

bool host_installer(void*, std::string*) {
    host_calls.fetch_add(1, std::memory_order_relaxed);
    return true;
}

} // namespace

TEST_CASE("Dawn proc installer: a host-owned build uses the host installer registered first",
          "[render][gpu-procs]") {
    using namespace pulp::render;

    std::string error;
    REQUIRE(set_dawn_proc_installer(DawnProcInstaller{&host_installer, nullptr}, &error));
    CHECK(ensure_dawn_procs(&error));
    CHECK(ensure_dawn_procs(&error));
    CHECK(host_calls.load() == 1);
}
