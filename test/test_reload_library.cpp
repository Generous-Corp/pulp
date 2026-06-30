// Phase 0 — dynamic-library handle + leak policy for DSP hot reload.
// See core/format/include/pulp/format/reload/reload_library.hpp.
//
// Drives the real loader against a tiny MODULE fixture (test/fixtures/
// reload_probe.cpp) whose absolute path is injected as RELOAD_PROBE_PATH.
#include <catch2/catch_test_macros.hpp>

#include <pulp/format/reload/reload_library.hpp>

#include <string>

using pulp::format::reload::LeakPolicy;
using pulp::format::reload::ReloadLibrary;

#ifndef RELOAD_PROBE_PATH
#error "RELOAD_PROBE_PATH must be defined to the built probe module path"
#endif

namespace {
const char* kProbe = RELOAD_PROBE_PATH;
using AnswerFn = int (*)();
using NameFn = const char* (*)();
}  // namespace

TEST_CASE("ReloadLibrary loads a module and resolves a typed symbol", "[reload][library]") {
    ReloadLibrary lib(kProbe);
    REQUIRE(lib.valid());
    REQUIRE(lib.error().empty());
    REQUIRE(lib.path() == std::string(kProbe));

    auto answer = lib.symbol<AnswerFn>("pulp_reload_probe_answer");
    REQUIRE(answer != nullptr);
    REQUIRE(answer() == 42);

    auto name = lib.symbol<NameFn>("pulp_reload_probe_name");
    REQUIRE(name != nullptr);
    REQUIRE(std::string(name()) == "reload-probe");
}

TEST_CASE("ReloadLibrary reports a failed load instead of crashing", "[reload][library]") {
    ReloadLibrary lib("/no/such/pulp-logic-build.dylib");
    REQUIRE_FALSE(lib.valid());
    REQUIRE_FALSE(static_cast<bool>(lib));
    REQUIRE_FALSE(lib.error().empty());
    REQUIRE(lib.raw_symbol("pulp_reload_probe_answer") == nullptr);
}

TEST_CASE("ReloadLibrary returns nullptr for a missing symbol", "[reload][library]") {
    ReloadLibrary lib(kProbe);
    REQUIRE(lib.valid());
    REQUIRE(lib.raw_symbol("pulp_no_such_symbol") == nullptr);
}

TEST_CASE("ReloadLibrary default policy retains (never unloads)", "[reload][library]") {
    ReloadLibrary lib(kProbe);
    REQUIRE(lib.leak_policy() == LeakPolicy::Retain);
    // Destructor here must NOT unload the image (the live-reload safety
    // contract). We can't observe the non-unload directly, but exercising the
    // Retain destructor path must not crash and must leave no error.
    REQUIRE(lib.valid());
}

TEST_CASE("ReloadLibrary CloseOnDestroy and explicit close() unload safely", "[reload][library]") {
    {
        ReloadLibrary lib(kProbe, LeakPolicy::CloseOnDestroy);
        REQUIRE(lib.valid());
        REQUIRE(lib.leak_policy() == LeakPolicy::CloseOnDestroy);
        // Destructor unloads — must not crash (no live object came from it).
    }

    ReloadLibrary lib(kProbe, LeakPolicy::CloseOnDestroy);
    REQUIRE(lib.valid());
    REQUIRE(lib.close());          // explicit unload succeeds
    REQUIRE_FALSE(lib.valid());
    REQUIRE_FALSE(lib.close());    // idempotent: already closed
}

TEST_CASE("ReloadLibrary move transfers ownership without double-unload", "[reload][library]") {
    ReloadLibrary a(kProbe, LeakPolicy::CloseOnDestroy);
    REQUIRE(a.valid());
    void* handle = a.native_handle();

    ReloadLibrary b(std::move(a));
    REQUIRE(b.valid());
    REQUIRE(b.native_handle() == handle);
    REQUIRE_FALSE(a.valid());            // NOLINT(bugprone-use-after-move) — moved-from is empty
    REQUIRE(a.native_handle() == nullptr);

    // b still resolves symbols; a is inert. b's CloseOnDestroy unloads once.
    auto answer = b.symbol<AnswerFn>("pulp_reload_probe_answer");
    REQUIRE(answer != nullptr);
    REQUIRE(answer() == 42);
}
