// WebView backend detection smoke.
//
// Validates that `pulp::view::detect_webview_backend()` returns a
// stable, documented identifier on every supported OS — without
// requiring `PULP_BUILD_WEBVIEW=ON` so it runs on every CI lane.
// Companion to the cross-platform gap-doc row that asks for verified
// WKWebView / WebView2 / WebKitGTK backends.

#include <pulp/view/web_view.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

TEST_CASE("WebView backend detection returns a documented identifier",
          "[webview][backend-detect]") {
    const auto id = pulp::view::detect_webview_backend();
    REQUIRE_FALSE(id.empty());

    // Documented identifiers — keep in sync with the doc-comment on
    // `pulp::view::detect_webview_backend()` in web_view.hpp.
    const std::vector<std::string> known = {
        "wkwebview",
        "webview2",
        "webkitgtk",
        "chromium",
        "none",
    };
    REQUIRE(std::find(known.begin(), known.end(), id) != known.end());
}

TEST_CASE("WebView backend availability flag matches identifier",
          "[webview][backend-detect]") {
    const auto id = pulp::view::detect_webview_backend();
    const bool available = pulp::view::webview_backend_available();

    if (id == "none") {
        REQUIRE_FALSE(available);
    } else {
        REQUIRE(available);
    }
}

// Per the detect_webview_backend() doc-comment, the answer depends on
// BOTH the OS and whether PULP_BUILD_WEBVIEW was set ON at build time.
// When the build option is OFF, the WebView TU is excluded entirely and
// detect_webview_backend() MUST return "none" so callers can distinguish
// "this build excluded WebView" from "this OS has no WebView installed"
// (regression: Codex PR #3016 P2).
#if defined(PULP_BUILD_WEBVIEW) && !PULP_BUILD_WEBVIEW
TEST_CASE("WebView backend reports none when PULP_BUILD_WEBVIEW=OFF "
          "(regression: PR #3016 review)",
          "[webview][backend-detect][issue-3016]") {
    REQUIRE(pulp::view::detect_webview_backend() == "none");
    REQUIRE_FALSE(pulp::view::webview_backend_available());
}
#endif

#if defined(__APPLE__)
TEST_CASE("WebView backend on Apple platforms is wkwebview when enabled",
          "[webview][backend-detect][apple]") {
#if defined(PULP_BUILD_WEBVIEW) && PULP_BUILD_WEBVIEW
    REQUIRE(pulp::view::detect_webview_backend() == "wkwebview");
    REQUIRE(pulp::view::webview_backend_available());
#else
    REQUIRE(pulp::view::detect_webview_backend() == "none");
#endif
}
#endif

#if defined(_WIN32)
TEST_CASE("WebView backend on Windows is either webview2 or none",
          "[webview][backend-detect][win]") {
    const auto id = pulp::view::detect_webview_backend();
    REQUIRE((id == "webview2" || id == "none"));
}
#endif

#if defined(__linux__) && !defined(__ANDROID__)
TEST_CASE("WebView backend on Linux is either webkitgtk or none",
          "[webview][backend-detect][linux]") {
    const auto id = pulp::view::detect_webview_backend();
    REQUIRE((id == "webkitgtk" || id == "none"));
}
#endif

#if defined(__ANDROID__)
TEST_CASE("WebView backend on Android is chromium when enabled",
          "[webview][backend-detect][android]") {
#if defined(PULP_BUILD_WEBVIEW) && PULP_BUILD_WEBVIEW
    REQUIRE(pulp::view::detect_webview_backend() == "chromium");
#else
    REQUIRE(pulp::view::detect_webview_backend() == "none");
#endif
}
#endif
