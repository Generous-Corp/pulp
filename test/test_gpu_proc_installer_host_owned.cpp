// A device library built with PULP_DAWN_PROCS_HOST_OWNED has no Dawn proc-table
// installer of its own. A GpuSurface or GpuCompute created before the host
// registers one must fail with a clear error instead of installing Dawn's native
// procs, and the policy is then sealed, so the out-of-order host is refused too.
//
// One test case: the policy is process-global and the first GPU object seals it.
// It needs no GPU: every refusal happens before Dawn is touched.

#include <pulp/render/gpu_compute.hpp>
#include <pulp/render/gpu_proc_installer.hpp>
#include <pulp/render/gpu_surface.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>

namespace {

std::atomic<int> late_calls{0};

bool late_installer(void*, std::string*) {
    late_calls.fetch_add(1, std::memory_order_relaxed);
    return true;
}

} // namespace

TEST_CASE("Dawn proc installer: a host-owned build creates nothing before the host registers",
          "[render][gpu-procs]") {
    using namespace pulp::render;

    auto surface = GpuSurface::create_dawn();
    REQUIRE(surface != nullptr);
    GpuSurface::Config config;
    config.width = 64;
    config.height = 64;
    CHECK_FALSE(surface->initialize(config));

    auto compute = GpuCompute::create();
    REQUIRE(compute != nullptr);
    CHECK_FALSE(compute->initialize_standalone());

    std::string error;
    CHECK_FALSE(ensure_dawn_procs(&error));
    CHECK(error.find("host-owned") != std::string::npos);
    CHECK(error.find("set_dawn_proc_installer") != std::string::npos);

    error.clear();
    CHECK_FALSE(set_dawn_proc_installer(DawnProcInstaller{&late_installer, nullptr}, &error));
    CHECK(error.find("before creating any Dawn object") != std::string::npos);
    CHECK(late_calls.load() == 0);
}
