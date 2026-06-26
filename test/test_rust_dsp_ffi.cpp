// Opt-in Rust lane (PULP_BUILD_NATIVE_COMPONENT_RUST_TESTS). Proves the
// canonical C ABI links and round-trips from a real Rust `staticlib` core, and
// that the RT-safety interception hook actually traps an allocation inside a
// no-alloc scope (the deliberate-allocate self-test runs in a CHILD process so
// the abort cannot take down the Catch2 parent).
//
// Built only when the option is ON; default builds never need a Rust toolchain.

#include <pulp/native_components/native_core.h>
#include <pulp/native_components/native_core.hpp>

#include "native_components/reference_processor.hpp"
#include "native_components/rt_test_scope.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdio>
#include <new>
#include <string_view>

#include <sys/wait.h>
#include <unistd.h>

using namespace pulp::native_components;

// Exported by the Rust staticlib (test/fixtures/native-components/noop-rust-core).
extern "C" const pulp_native_core_v1* pulp_native_core_entry_v1(void);
extern "C" void pulp_noop_selftest_alloc(void);

TEST_CASE("rust core entry returns a compatible vtable", "[rust-dsp][abi]") {
    const pulp_native_core_v1* core = pulp_native_core_entry_v1();
    REQUIRE(core != nullptr);
    REQUIRE(core->abi_version == kAbiVersion);
    REQUIRE(is_compatible(core));
    REQUIRE(core->descriptor != nullptr);
    REQUIRE(core->process != nullptr);
}

TEST_CASE("rust core descriptor + parameters round-trip", "[rust-dsp][abi]") {
    const pulp_native_core_v1* core = pulp_native_core_entry_v1();
    const pulp_native_descriptor_v1* d = core->descriptor();
    REQUIRE(d != nullptr);
    REQUIRE(std::string_view(d->id, d->id_len) == "com.pulp.example.noop-rust");
    REQUIRE(std::string_view(d->name, d->name_len) == "Noop Rust Core");
    REQUIRE((d->capabilities & PULP_NATIVE_CAP_STATE) != 0);

    std::uint32_t count = 0;
    const pulp_native_param_v1* params = core->parameters(&count);
    REQUIRE(count == 1);
    REQUIRE(params != nullptr);
    // The Rust side pins the FNV-1a/64 hash; it must equal what the host computes.
    REQUIRE(params[0].id_hash == param_id_hash("gain"));
    REQUIRE(std::string_view(params[0].id, params[0].id_len) == "gain");
    REQUIRE(params[0].min_value == -60.0);
}

TEST_CASE("rust core lifecycle drives create->process->destroy", "[rust-dsp][lifecycle]") {
    const pulp_native_core_v1* core = pulp_native_core_entry_v1();
    pulp_native_instance* inst = nullptr;
    REQUIRE(core->create(nullptr, &inst) == PULP_NATIVE_OK);
    REQUIRE(inst != nullptr);

    REQUIRE(core->prepare(inst, nullptr) == PULP_NATIVE_OK);
    REQUIRE(core->resume(inst) == PULP_NATIVE_OK);
    // No-op process must report success without touching host buffers.
    REQUIRE(core->process(inst, nullptr) == PULP_NATIVE_OK);
    REQUIRE(core->suspend(inst) == PULP_NATIVE_OK);
    core->release(inst);
    core->destroy(inst);
    SUCCEED("lifecycle completed");
}

TEST_CASE("rust allocation OUTSIDE a no-alloc scope is allowed",
          "[rust-dsp][rt-safety]") {
    // Positive control: the checking allocator must NOT trap when no scope is
    // active. If this aborted, the hook would be over-eager.
    pulp_noop_selftest_alloc();
    SUCCEED("allocation outside scope did not trap");
}

TEST_CASE("rust allocation INSIDE a no-alloc scope traps (death test)",
          "[rust-dsp][rt-safety]") {
    std::fflush(nullptr);  // flush buffered stdio so the child doesn't re-emit it
    pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        // Child: enter a no-alloc scope and deliberately allocate in Rust. The
        // checking global allocator calls the trap, which aborts. We must not
        // reach _exit(0).
        test::RtNoAllocScope guard;
        pulp_noop_selftest_alloc();
        _exit(0);  // reached only if the hook FAILED to trap
    }
    int status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    INFO("child status=" << status);
    // The child must have died by signal (SIGABRT from the trap), not exited 0.
    REQUIRE(WIFSIGNALED(status));
    REQUIRE((WTERMSIG(status) == SIGABRT || WTERMSIG(status) == SIGTRAP));
}

TEST_CASE("C++ allocation inside a no-alloc scope also traps (death test)",
          "[rust-dsp][rt-safety]") {
    std::fflush(nullptr);  // flush buffered stdio so the child doesn't re-emit it
    pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        test::RtNoAllocScope guard;
        // Explicitly call the replaceable global operator new so this death
        // test cannot be optimized into a non-allocating unused `new int`.
        void* leak = ::operator new(sizeof(int));
        *static_cast<volatile int*>(leak) = 7;
        _exit(0);
    }
    int status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    INFO("child status=" << status);
    REQUIRE(WIFSIGNALED(status));
    REQUIRE((WTERMSIG(status) == SIGABRT || WTERMSIG(status) == SIGTRAP));
}

TEST_CASE("parity harness declares the reference matrix",
          "[rust-dsp][parity]") {
    // This only declares the matrix; per-format execution is wired separately.
    REQUIRE(std::size(test::kReferenceProcessors) == 3);
    REQUIRE(test::kReferenceProcessors[0].primary_case == test::EventCase::Effect);
}
