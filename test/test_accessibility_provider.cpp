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
    REQUIRE_NOTHROW(accessibility_tree_changed(nullptr));
}

TEST_CASE("shutdown_accessibility: repeated init+shutdown cycles are safe",
          "[a11y][provider][issue-500][issue-514]") {
    // #500 / #485: earlier shutdown ordering released our ref before
    // UIA clients were disconnected from the provider, creating a
    // UAF window if a cached client called any provider method after
    // we deleted the session. The fix calls UiaDisconnectProvider
    // before releasing our ref so in-flight calls drain and no new
    // ones can land.
    //
    // #514 tightens this further: host_provider is now std::atomic<*>,
    // and shutdown exchanges it to null BEFORE calling
    // UiaReturnRawElementProvider(hwnd, 0, 0, nullptr) / disconnecting.
    // That closes a secondary race where WM_GETOBJECT on the UI thread
    // could republish the provider to UIA between "tell UIA we have no
    // provider" and "null out the session pointer", handing UIA an
    // AddRef'd reference we were about to Release.
    //
    // Invariant this test pins: repeated init+shutdown cycles execute
    // without crashing, double-free, or hang. On Windows this drives
    // the real UIA shutdown barrier through the atomic-null-first
    // ordering; on Linux it exercises the stub. Stress with enough
    // cycles that any lingering refcount or ordering regression has a
    // fighting chance to surface.
    View root;
    for (int i = 0; i < 32; ++i) {
        void* h = init_accessibility(root, nullptr);
        REQUIRE(h != nullptr);
        // Exercise the event-raise helpers while the session is live
        // so they go through the atomic acquire-load path at least
        // once per cycle. They must also be a no-op after shutdown —
        // see the follow-up invariant check below.
        accessibility_tree_changed(h);
        notify_accessibility_value_changed(h, root);
        notify_accessibility_focus_changed(h, root);
        notify_accessibility_name_changed(h, root);
        shutdown_accessibility(h);
    }

    REQUIRE_NOTHROW(accessibility_tree_changed(nullptr));
    REQUIRE_NOTHROW(notify_accessibility_value_changed(nullptr, root));
    REQUIRE_NOTHROW(notify_accessibility_focus_changed(nullptr, root));
    REQUIRE_NOTHROW(notify_accessibility_name_changed(nullptr, root));
}
#else
TEST_CASE("accessibility_provider is a no-op on this platform",
          "[a11y][provider]") {
    // macOS / iOS / Android don't link the provider symbols; this
    // stub keeps the suite buildable on every platform.
    SKIP("accessibility_provider bridge is only linked on Windows and Linux desktop");
}
#endif
