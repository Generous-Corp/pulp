// Verifies the cross-platform accessibility_provider entry point.
// Workstream 04 slices 4.1 (Windows UIA) + 4.2 (Linux AT-SPI).
//
// On macOS / iOS the platform bridge lives in the view files directly
// and no init_accessibility() impl is linked, so those platforms are
// exercised only through their own platform suites. The build-system
// guards the suite for the Windows + Linux targets where the header's
// symbols are actually defined.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/accessibility_provider.hpp>
#include <pulp/view/view.hpp>

using namespace pulp::view;

#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
TEST_CASE("init_accessibility returns a non-null handle (stub)",
          "[a11y][provider]") {
    View root;
    void* handle = init_accessibility(root, nullptr);
    REQUIRE(handle != nullptr);
    // Second call should still succeed — the stub is idempotent.
    void* handle2 = init_accessibility(root, nullptr);
    REQUIRE(handle2 != nullptr);
    shutdown_accessibility(handle);
    shutdown_accessibility(handle2);
}

TEST_CASE("accessibility_tree_changed tolerates a null handle",
          "[a11y][provider]") {
    // Widgets that raise events before init_accessibility ran should
    // never crash — pin the invariant.
    accessibility_tree_changed(nullptr);
    SUCCEED();
}

TEST_CASE("shutdown_accessibility: repeated init+shutdown cycles are safe",
          "[a11y][provider][issue-500]") {
    // #500 / #485: earlier shutdown ordering released our ref before
    // UIA clients were disconnected from the provider, creating a
    // UAF window if a cached client called any provider method after
    // we deleted the session. The fix calls UiaDisconnectProvider
    // before releasing our ref so in-flight calls drain and no new
    // ones can land. Exercise many cycles to surface refcount or
    // lifetime regressions; on Windows this drives the real UIA
    // shutdown barrier, on Linux it exercises the stub.
    View root;
    for (int i = 0; i < 32; ++i) {
        void* h = init_accessibility(root, nullptr);
        REQUIRE(h != nullptr);
        shutdown_accessibility(h);
    }
    SUCCEED("no crash across repeated attach/detach");
}
#else
TEST_CASE("accessibility_provider is a no-op on this platform",
          "[a11y][provider]") {
    // macOS / iOS / Android don't link the provider symbols; this
    // placeholder keeps the suite buildable on every platform.
    SUCCEED();
}
#endif
