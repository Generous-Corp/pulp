// The Dawn proc-table installer policy, driven on fresh registry instances so
// every case starts from an unresolved policy. No GPU and no Dawn are needed:
// the default installer here only counts, standing in for the native one.

#include "gpu_proc_installer_registry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using pulp::render::DawnProcInstaller;
using pulp::render::detail::DawnProcInstallerRegistry;

namespace {

struct Counter {
    std::atomic<int> calls{0};
    bool result = true;
    std::string reason;
};

bool counting_install(void* context, std::string* error) {
    auto& counter = *static_cast<Counter*>(context);
    counter.calls.fetch_add(1, std::memory_order_relaxed);
    // Widen the window in which a second thread could also run the installer.
    std::this_thread::yield();
    if (!counter.result && error != nullptr)
        *error = counter.reason;
    return counter.result;
}

DawnProcInstaller installer_for(Counter& counter) {
    return DawnProcInstaller{&counting_install, &counter};
}

} // namespace

TEST_CASE("Dawn proc installer: the default runs once across repeated calls", "[render][gpu-procs]") {
    Counter fallback;
    DawnProcInstallerRegistry registry(installer_for(fallback));

    REQUIRE(registry.ensure(nullptr));
    REQUIRE(registry.ensure(nullptr));
    REQUIRE(registry.ensure(nullptr));
    CHECK(fallback.calls == 1);
    CHECK(registry.install_calls() == 1);
}

TEST_CASE("Dawn proc installer: the default runs once when two threads race", "[render][gpu-procs]") {
    for (int round = 0; round < 64; ++round) {
        Counter fallback;
        DawnProcInstallerRegistry registry(installer_for(fallback));
        std::atomic<bool> go{false};
        std::atomic<int> ready{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 2; ++t) {
            threads.emplace_back([&] {
                while (!go.load(std::memory_order_acquire)) {
                }
                if (registry.ensure(nullptr))
                    ready.fetch_add(1, std::memory_order_relaxed);
            });
        }
        go.store(true, std::memory_order_release);
        for (auto& thread : threads)
            thread.join();
        REQUIRE(ready == 2);
        REQUIRE(fallback.calls == 1);
    }
}

TEST_CASE("Dawn proc installer: a registered installer runs instead of the default", "[render][gpu-procs]") {
    Counter fallback;
    Counter host;
    DawnProcInstallerRegistry registry(installer_for(fallback));

    std::string error;
    REQUIRE(registry.set(installer_for(host), &error));
    REQUIRE(registry.ensure(&error));
    REQUIRE(registry.ensure(&error));
    CHECK(host.calls == 1);
    CHECK(fallback.calls == 0);
}

TEST_CASE("Dawn proc installer: registration after the policy was used is rejected", "[render][gpu-procs]") {
    Counter fallback;
    Counter host;
    DawnProcInstallerRegistry registry(installer_for(fallback));
    REQUIRE(registry.ensure(nullptr));

    std::string error;
    CHECK_FALSE(registry.set(installer_for(host), &error));
    CHECK(error.find("before creating any Dawn object") != std::string::npos);
    // The rejected installer never runs; the table the default installed stays.
    REQUIRE(registry.ensure(nullptr));
    CHECK(host.calls == 0);
    CHECK(fallback.calls == 1);
}

TEST_CASE("Dawn proc installer: a failed attempt still seals the policy", "[render][gpu-procs]") {
    Counter host;
    host.result = false;
    Counter late;
    DawnProcInstallerRegistry registry(installer_for(host));
    CHECK_FALSE(registry.ensure(nullptr));

    std::string error;
    CHECK_FALSE(registry.set(installer_for(late), &error));
    CHECK_FALSE(error.empty());
    CHECK(late.calls == 0);
}

TEST_CASE("Dawn proc installer: re-registering the same installer is a no-op", "[render][gpu-procs]") {
    Counter fallback;
    Counter host;
    Counter other;
    DawnProcInstallerRegistry registry(installer_for(fallback));

    std::string error;
    REQUIRE(registry.set(installer_for(host), &error));
    REQUIRE(registry.set(installer_for(host), &error));
    CHECK_FALSE(registry.set(installer_for(other), &error));
    CHECK(error.find("different Dawn proc installer") != std::string::npos);

    REQUIRE(registry.ensure(nullptr));
    // Asking again for the installer already in force is not a late change.
    CHECK(registry.set(installer_for(host), &error));
    CHECK(host.calls == 1);
    CHECK(other.calls == 0);
}

TEST_CASE("Dawn proc installer: an installer without a function is rejected", "[render][gpu-procs]") {
    Counter fallback;
    DawnProcInstallerRegistry registry(installer_for(fallback));
    std::string error;
    CHECK_FALSE(registry.set(DawnProcInstaller{}, &error));
    CHECK_FALSE(error.empty());
    REQUIRE(registry.ensure(nullptr));
    CHECK(fallback.calls == 1);
}

TEST_CASE("Dawn proc installer: a verify-only failure is reported and retried", "[render][gpu-procs]") {
    Counter fallback;
    Counter host;
    host.result = false;
    host.reason = "host has not installed the Dawn proc table";
    DawnProcInstallerRegistry registry(installer_for(fallback));
    REQUIRE(registry.set(installer_for(host), nullptr));

    std::string error;
    CHECK_FALSE(registry.ensure(&error));
    CHECK(error == host.reason);
    CHECK_FALSE(registry.ensure(&error));
    CHECK(host.calls == 2);

    // Once the host finishes its own installation the next attempt succeeds,
    // and the native default never runs in its place.
    host.result = true;
    REQUIRE(registry.ensure(&error));
    REQUIRE(registry.ensure(&error));
    CHECK(host.calls == 3);
    CHECK(fallback.calls == 0);
}

namespace {

struct ReentrantContext {
    DawnProcInstallerRegistry* registry = nullptr;
    bool inner_result = true;
    std::string inner_error;
};

bool reentrant_install(void* context, std::string*) {
    auto& ctx = *static_cast<ReentrantContext*>(context);
    ctx.inner_result = ctx.registry->ensure(&ctx.inner_error);
    return true;
}

} // namespace

TEST_CASE("Dawn proc installer: an installer that re-enters fails instead of deadlocking",
          "[render][gpu-procs]") {
    ReentrantContext ctx;
    DawnProcInstallerRegistry registry(DawnProcInstaller{&reentrant_install, &ctx});
    ctx.registry = &registry;

    REQUIRE(registry.ensure(nullptr));
    CHECK_FALSE(ctx.inner_result);
    CHECK(ctx.inner_error.find("re-entered") != std::string::npos);
}
