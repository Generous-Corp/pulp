// test_au_v2_contract_shipping.mm: the AU v2 contract view module as a UI
// package plug-in ships it.
//
// Compiles au_v2_contract_view.mm without PULP_AU_V2_TEST_HOOKS and checks the
// test seam for the host-follows-container answer is not in it. The seam is a
// plain function, found by its mangled name. Two controls keep the check
// honest: a function the module always defines must resolve, and the same
// lookup finds the seam in the test build that does define it
// (test_au_v2_contract_view.mm).

#include <catch2/catch_test_macros.hpp>

#include <dlfcn.h>

TEST_CASE("a shipped AU v2 contract view carries no test seam",
          "[auv2][editor-host][au-contract][shipping]") {
    // Control: the module's real function resolves by the same lookup.
    CHECK(dlsym(RTLD_DEFAULT, "_ZN4pulp6format2au33contract_resize_follows_containerEv") !=
          nullptr);
    CHECK(dlsym(RTLD_DEFAULT, "_ZN4pulp6format2au49set_contract_resize_follows_container_for_"
                              "testingENSt3__18optionalIbEE") == nullptr);
}
