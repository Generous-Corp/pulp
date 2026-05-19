// Linux AccessKit TextAccessibilityNode backend tests (font v2 Slice 2.6).
//
// Compile-gated on Linux (excluding Android, which uses a different
// platform-a11y path via TalkBack). The Pulp CI matrix currently runs
// macOS as the only required gate; this file is structured to:
//   - compile on every platform with a sentinel SUCCEED("...skipped...")
//     case off Linux, so ctest -N stays stable; AND
//   - run the real backend-name + role-mapping assertions on Linux only.
//
// The Linux backend ships as a documented stub
// (accessibility_backend_name() == "linux-accesskit-stub"); the
// register / unregister / snapshot surface is fully functional through
// the C++ shadow map, so the same cross-platform tests in
// test_text_accessibility.cpp pass on Linux. This file pins the
// platform-identifier override + the AccessKit role mapping so the
// scaffold can be flipped to a real backend later without churning the
// test expectations.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/text_accessibility.hpp>

using namespace pulp::view;

#if !(defined(__linux__) && !defined(__ANDROID__))

TEST_CASE("TextAccessibilityNode linux backend: skipped (non-Linux host)",
          "[view][text-a11y][linux][issue-2255]") {
    // Cross-platform sanity: outside Linux the backend identifier must
    // NOT be the Linux-only stub value.
    REQUIRE(accessibility_backend_name() != "linux-accesskit-stub");
    REQUIRE(accessibility_backend_name() != "linux-accesskit");
    SUCCEED("Linux AccessKit backend test skipped — not a Linux host");
}

#else  // __linux__ && !__ANDROID__

// Forward declaration mirroring the extern "C" hook in
// platform/linux/text_accessibility_linux.cpp. Returns the integer value
// of the accesskit::Role enumerator that the eventual real backend will
// pass to AccessKit's C API. Keeping the mapping pinned in a test
// means the follow-up PR that flips the stub to a live backend cannot
// silently drift the role contract.
extern "C" int pulp_text_accessibility_role_linux(int pulp_role);

TEST_CASE("TextAccessibilityNode linux backend reports 'linux-accesskit-stub'",
          "[view][text-a11y][linux][issue-2255]") {
    // Explicit stub-state identifier — distinguishes "no Linux a11y
    // wiring at all" (the cross-platform default returns "none") from
    // "the Linux scaffold is present but does not yet talk to AccessKit".
    REQUIRE(accessibility_backend_name() == "linux-accesskit-stub");
}

TEST_CASE("TextAccessibilityNode linux: role mapping covers all five enumerators",
          "[view][text-a11y][linux][issue-2255]") {
    // Numeric constants come from accesskit_c v0.18 — see the
    // accesskit_role_for() helper in
    // platform/linux/text_accessibility_linux.cpp for the source-of-
    // truth mapping. Pinning these values here ensures the flip from
    // stub → real backend doesn't silently re-map a role.
    REQUIRE(pulp_text_accessibility_role_linux(
        static_cast<int>(TextAccessibilityRole::Label)) == 90);   // Role::Label
    REQUIRE(pulp_text_accessibility_role_linux(
        static_cast<int>(TextAccessibilityRole::Button)) == 24);  // Role::Button
    REQUIRE(pulp_text_accessibility_role_linux(
        static_cast<int>(TextAccessibilityRole::TextEditor)) == 137);  // Role::TextInput
    REQUIRE(pulp_text_accessibility_role_linux(
        static_cast<int>(TextAccessibilityRole::Heading)) == 84); // Role::Heading
    REQUIRE(pulp_text_accessibility_role_linux(
        static_cast<int>(TextAccessibilityRole::Other)) == 74);   // Role::GenericContainer
}

TEST_CASE("TextAccessibilityNode linux: register round-trips through cross-platform snapshot",
          "[view][text-a11y][linux][issue-2255]") {
    // Drain any previous registrations so this test is independent of
    // ordering.
    for (const auto& node : snapshot_accessibility_nodes()) {
        unregister_text_accessibility_node(node.id);
    }
    REQUIRE(snapshot_accessibility_nodes().empty());

    TextAccessibilityNode node;
    node.id = "linux-label-1";
    node.text = "Hello AccessKit";
    node.role = TextAccessibilityRole::Label;
    register_text_accessibility_node(node);

    auto snap = snapshot_accessibility_nodes();
    REQUIRE(snap.size() == 1);
    REQUIRE(snap[0].id == "linux-label-1");
    REQUIRE(snap[0].text == "Hello AccessKit");
    REQUIRE(snap[0].role == TextAccessibilityRole::Label);

    unregister_text_accessibility_node("linux-label-1");
    REQUIRE(snapshot_accessibility_nodes().empty());
}

#endif  // __linux__ && !__ANDROID__
