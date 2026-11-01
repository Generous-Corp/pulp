// pulp::view-platform excludes WebView, so linked alone its detector has no
// provider and must report "none" on every OS, whatever PULP_BUILD_WEBVIEW is.

#include <pulp/view/web_view.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("view-platform alone reports no WebView backend",
          "[webview][backend-detect][view-platform]") {
    REQUIRE(pulp::view::detect_webview_backend() == "none");
    REQUIRE_FALSE(pulp::view::webview_backend_available());
}
