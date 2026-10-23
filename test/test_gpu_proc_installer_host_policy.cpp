// A host that owns Dawn's proc table registers a verify-only installer before
// any GPU object exists. While the host reports the table missing, GpuSurface
// and GpuCompute must refuse to create a Dawn object rather than install the
// native procs themselves, and a later registration must be refused.
//
// The policy is process-global and is sealed by the first GPU object, so this
// executable holds exactly one test case: no other case may create a GPU
// object in the same process first. It needs no GPU: every refusal happens
// before Dawn is touched.

#include <pulp/render/gpu_compute.hpp>
#include <pulp/render/gpu_proc_installer.hpp>
#include <pulp/render/gpu_surface.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>

namespace {

std::atomic<int> verify_calls{0};
std::atomic<int> late_calls{0};

bool host_reports_missing_table(void*, std::string* error) {
    verify_calls.fetch_add(1, std::memory_order_relaxed);
    if (error != nullptr)
        *error = "host Dawn proc table is not installed";
    return false;
}

bool late_installer(void*, std::string*) {
    late_calls.fetch_add(1, std::memory_order_relaxed);
    return true;
}

} // namespace

TEST_CASE("Dawn proc installer: a verify-only host without a table blocks GPU creation",
          "[render][gpu-procs]") {
#if defined(PULP_HAS_DAWN) && !defined(__EMSCRIPTEN__)
    using namespace pulp::render;

    std::string error;
    REQUIRE(set_dawn_proc_installer(DawnProcInstaller{&host_reports_missing_table, nullptr},
                                    &error));

    auto surface = GpuSurface::create_dawn();
    REQUIRE(surface != nullptr);
    GpuSurface::Config config;
    config.width = 64;
    config.height = 64;
    CHECK_FALSE(surface->initialize(config));
    const int after_surface = verify_calls.load();
    CHECK(after_surface >= 1);

    auto compute = GpuCompute::create();
    REQUIRE(compute != nullptr);
    CHECK_FALSE(compute->initialize_standalone());
    CHECK(verify_calls.load() > after_surface);

    CHECK_FALSE(ensure_dawn_procs(&error));
    CHECK(error == "host Dawn proc table is not installed");

    // The policy was consulted, so a host that registers now is out of order.
    error.clear();
    CHECK_FALSE(set_dawn_proc_installer(DawnProcInstaller{&late_installer, nullptr}, &error));
    CHECK(error.find("before creating any Dawn object") != std::string::npos);
    CHECK_FALSE(ensure_dawn_procs(nullptr));
    CHECK(late_calls.load() == 0);
#else
    SKIP("this build has no native Dawn");
#endif
}
