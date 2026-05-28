// Tests for iOS target foundation
// These tests verify the cross-platform abstractions work correctly
// and that iOS-specific code paths are properly guarded.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <pulp/platform/detect.hpp>
#include <pulp/view/geometry.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/plugin_view_host.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace pulp;
using Catch::Approx;

// ── Platform detection ──────────────────────────────────────────────────────

TEST_CASE("Platform detection: iOS vs macOS", "[ios][platform]") {
    // On the current build host, exactly one of these must be true
    REQUIRE((platform::is_macos || platform::is_ios || platform::is_windows || platform::is_linux));

    // iOS and macOS are mutually exclusive
    REQUIRE_FALSE((platform::is_macos && platform::is_ios));

    // Both are Apple platforms
    if constexpr (platform::is_ios || platform::is_macos) {
        REQUIRE(platform::is_apple);
    }

    // iOS is mobile, macOS is desktop
    if constexpr (platform::is_ios) {
        REQUIRE(platform::is_mobile);
        REQUIRE_FALSE(platform::is_desktop);
    }
    if constexpr (platform::is_macos) {
        REQUIRE(platform::is_desktop);
        REQUIRE_FALSE(platform::is_mobile);
    }
}

// ── Safe area inset geometry ────────────────────────────────────────────────

TEST_CASE("Rect::inset models safe area margins", "[ios][geometry]") {
    // Full screen rect (e.g., iPhone with notch: 390x844)
    view::Rect screen{0, 0, 390, 844};

    // Typical iPhone safe area insets: top=47, bottom=34, left=0, right=0
    float top = 47.0f, bottom = 34.0f, left_i = 0.0f, right_i = 0.0f;

    // Apply safe area insets manually (as the iOS view host does)
    view::Rect safe{
        screen.x + left_i,
        screen.y + top,
        screen.width - left_i - right_i,
        screen.height - top - bottom
    };

    REQUIRE(safe.x == Approx(0.0f));
    REQUIRE(safe.y == Approx(47.0f));
    REQUIRE(safe.width == Approx(390.0f));
    REQUIRE(safe.height == Approx(763.0f));  // 844 - 47 - 34

    // The safe rect should be contained within the screen rect
    REQUIRE(safe.x >= screen.x);
    REQUIRE(safe.y >= screen.y);
    REQUIRE(safe.right() <= screen.right());
    REQUIRE(safe.bottom() <= screen.bottom());
}

TEST_CASE("Rect::inset with uniform padding", "[ios][geometry]") {
    view::Rect r{10, 20, 200, 100};
    auto inset = r.inset(5.0f);

    REQUIRE(inset.x == Approx(15.0f));
    REQUIRE(inset.y == Approx(25.0f));
    REQUIRE(inset.width == Approx(190.0f));
    REQUIRE(inset.height == Approx(90.0f));
}

TEST_CASE("Rect::inset with asymmetric padding", "[ios][geometry]") {
    view::Rect r{0, 0, 400, 800};
    auto inset = r.inset(20.0f, 40.0f);  // horizontal=20, vertical=40

    REQUIRE(inset.x == Approx(20.0f));
    REQUIRE(inset.y == Approx(40.0f));
    REQUIRE(inset.width == Approx(360.0f));
    REQUIRE(inset.height == Approx(720.0f));
}

TEST_CASE("Rect::inset clamps to zero for oversized insets", "[ios][geometry]") {
    view::Rect small{0, 0, 10, 10};
    auto inset = small.inset(20.0f);

    REQUIRE(inset.width == Approx(0.0f));
    REQUIRE(inset.height == Approx(0.0f));
}

// ── Touch/pointer events ────────────────────────────────────────────────────

TEST_CASE("MouseEvent: touch detection via pointer_id", "[ios][input]") {
    view::MouseEvent touch_event;
    touch_event.pointer_id = 1;
    touch_event.modifiers = 0x8000;  // Touch flag
    touch_event.is_down = true;

    REQUIRE(touch_event.isTouch());
    REQUIRE(touch_event.pointer_id > 0);
}

TEST_CASE("MouseEvent: primary mouse is not touch", "[ios][input]") {
    view::MouseEvent mouse_event;
    mouse_event.pointer_id = 0;
    mouse_event.modifiers = 0;

    REQUIRE_FALSE(mouse_event.isTouch());
}

TEST_CASE("MouseEvent: multi-touch uses distinct pointer IDs", "[ios][input]") {
    // Simulate two simultaneous touches
    view::MouseEvent touch1;
    touch1.pointer_id = 0;
    touch1.modifiers = 0x8000;
    touch1.position = {100.0f, 200.0f};

    view::MouseEvent touch2;
    touch2.pointer_id = 1;
    touch2.modifiers = 0x8000;
    touch2.position = {300.0f, 400.0f};

    REQUIRE(touch1.pointer_id != touch2.pointer_id);
    REQUIRE(touch1.isTouch());
    REQUIRE(touch2.isTouch());
    REQUIRE(touch1.position.x != touch2.position.x);
}

// ── PluginViewHost size management ──────────────────────────────────────────

TEST_CASE("PluginViewHost::Size default values", "[ios][view]") {
    view::PluginViewHost::Size size;
    REQUIRE(size.width == 400);
    REQUIRE(size.height == 300);
}

TEST_CASE("PluginViewHost::Size custom values", "[ios][view]") {
    view::PluginViewHost::Size size{390, 844};
    REQUIRE(size.width == 390);
    REQUIRE(size.height == 844);
}

// ── Platform-specific feature flags ─────────────────────────────────────────

// ── AUv3 HostApp template shape (PR #3095, Codex P2) ───────────────────────
//
// Regression test for the Codex review finding on
// templates/ios-auv3/HostApp/ContentView.swift:99: "Match the embedded
// component descriptor exactly". The previous template filtered AUv3
// components by Pulp manufacturer only and returned the first hit,
// which made the picker non-deterministic when more than one Pulp AUv3
// was installed on the same device or simulator. The fix reads the
// HostApp's own Info.plist AudioComponents[0] entry (mirrored from
// the AUv3 extension target by pulp_add_ios_host_app()) and matches on
// the full type/subtype/manufacturer descriptor.
//
// We can't compile-test the Swift template from C++ in this suite (it
// only builds under an iOS Simulator CMake configure), so the test
// asserts the shipped template carries the new descriptor-mirror code
// path. This catches the class of regression where someone deletes the
// Info.plist lookup and falls back to the old manufacturer-only filter
// without updating the test.
TEST_CASE("ios-auv3 HostApp template matches the embedded descriptor exactly",
          "[ios][auv3][template][issue-3095]") {
#ifndef PULP_SOURCE_DIR
    SUCCEED("PULP_SOURCE_DIR not defined — skipping template-shape assertion");
    return;
#else
    namespace fs = std::filesystem;
    fs::path template_path = fs::path{PULP_SOURCE_DIR}
        / "templates" / "ios-auv3" / "HostApp" / "ContentView.swift";

    REQUIRE(fs::exists(template_path));

    std::ifstream in{template_path};
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string body = ss.str();

    // The host MUST read AudioComponents[0] back from the HostApp's own
    // Info.plist via Bundle.main. If this string disappears the template
    // has regressed to the non-deterministic manufacturer-only filter.
    REQUIRE(body.find("AudioComponents") != std::string::npos);
    REQUIRE(body.find("Bundle.main.object(forInfoDictionaryKey:") != std::string::npos);

    // The filter must compare componentType, componentSubType, and
    // componentManufacturer — the three fields that uniquely identify
    // a specific Pulp AUv3 across plug-ins shipped by the same vendor.
    REQUIRE(body.find("componentType == target.componentType") != std::string::npos);
    REQUIRE(body.find("componentSubType == target.componentSubType") != std::string::npos);
    REQUIRE(body.find("componentManufacturer == target.componentManufacturer") != std::string::npos);

    // The manufacturer-only path must remain available as a labelled
    // fallback so HostApps that intentionally strip the AudioComponents
    // key (debugging-only overrides) still surface a usable plug-in.
    REQUIRE(body.find("fallback manufacturer-only") != std::string::npos);
#endif
}

TEST_CASE("Apple platform features are consistent", "[ios][platform]") {
    if constexpr (platform::is_apple) {
        // Apple platforms always use ARM64 on modern devices
        // (or x86_64 on Intel Macs / Simulator)
        REQUIRE(platform::is_64bit);

        // Apple always uses Clang
        REQUIRE((platform::current_compiler == platform::Compiler::AppleClang ||
                 platform::current_compiler == platform::Compiler::Clang));
    }
}
