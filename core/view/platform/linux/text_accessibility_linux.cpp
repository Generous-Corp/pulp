// Linux AccessKit backend for TextAccessibilityNode (font v2 Slice 2.6).
//
// SCAFFOLD-STUB-LEVEL backend. This file replaces the default "none"
// implementation in core/view/src/text_accessibility.cpp on Linux builds
// so accessibility_backend_name() returns "linux-accesskit-stub" instead
// of "none" — giving the cross-platform layer a way to distinguish "no
// platform a11y wiring at all" from "the Linux scaffold is present but
// not yet talking to AccessKit".
//
// The register / unregister / snapshot surface still routes through a
// process-local std::unordered_map, mirroring the default backend, so:
//   1. The cross-platform tests in test/test_text_accessibility.cpp keep
//      passing on Linux.
//   2. Painted-text sites that call register_text_accessibility_node()
//      get a stable surface to call against today — when the real
//      AccessKit wiring lands, the same call sites publish to AT-SPI
//      automatically without any caller-side change.
//
// ── To flip this stub to a real AccessKit backend ─────────────────────────
//
// AccessKit project: https://github.com/AccessKit/accesskit
// C bindings: https://github.com/AccessKit/accesskit/tree/main/bindings/c
// AT-SPI adapter: https://github.com/AccessKit/accesskit/tree/main/platforms/linux
//
// Required wiring (deferred to a follow-up slice):
//   1. Vendor or FetchContent the accesskit-c crate (BSD-licensed, MIT
//      compatible per Pulp's LICENSE policy) into external/accesskit/.
//      Build it with `cargo build --release -p accesskit_c` and link
//      libaccesskit.a (or .so) into pulp-view via the platform-Linux leg
//      of core/view/CMakeLists.txt.
//   2. On init_accessibility(): construct an accesskit_unix_adapter via
//      `accesskit_unix_adapter_new(...)` with the activation handler that
//      returns the initial TreeUpdate (root node + all currently-
//      registered TextAccessibilityNode entries).
//   3. On register_text_accessibility_node(): build a TreeUpdate
//      containing a single AccessKit node and call
//      `accesskit_unix_adapter_update_if_active(adapter, update)`. Map
//      TextAccessibilityRole → accesskit::Role per the table below.
//   4. On unregister: emit a TreeUpdate that drops the node by id.
//   5. On snapshot_accessibility_nodes(): the C++ shadow map already
//      returns the cross-platform view; no AccessKit round-trip needed.
//
// Role mapping (for the eventual real backend):
//   TextAccessibilityRole::Label      → accesskit::Role::Label
//   TextAccessibilityRole::Button     → accesskit::Role::Button
//   TextAccessibilityRole::TextEditor → accesskit::Role::TextInput
//   TextAccessibilityRole::Heading    → accesskit::Role::Heading
//   TextAccessibilityRole::Other      → accesskit::Role::GenericContainer
//
// Until that work lands, this stub keeps the platform identifier honest:
// `accessibility_backend_name()` returns the explicit "linux-accesskit-stub"
// value, so screen-reader-validation tooling can detect that Linux is
// scaffolded but not yet wired through to AT-SPI.

#if defined(__linux__) && !defined(__ANDROID__)

#include <pulp/view/text_accessibility.hpp>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulp::view {

namespace {

// Role-mapping helper. Defined as a free function so the eventual
// AccessKit wiring can call it directly when it constructs AccessKit
// TreeUpdate payloads; the stub leg returns the numeric enum value as
// an int so we don't depend on AccessKit headers being present today.
//
// Numeric constants below match the layout of `enum accesskit_role` in
// the C bindings as of accesskit_c v0.18 — verify against the upstream
// header before flipping this stub to a real backend.
int accesskit_role_for(TextAccessibilityRole role) {
    switch (role) {
        case TextAccessibilityRole::Label:      return /* Role::Label             */ 90;
        case TextAccessibilityRole::Button:     return /* Role::Button            */ 24;
        case TextAccessibilityRole::TextEditor: return /* Role::TextInput         */ 137;
        case TextAccessibilityRole::Heading:    return /* Role::Heading           */ 84;
        case TextAccessibilityRole::Other:
        default:                                return /* Role::GenericContainer  */ 74;
    }
}

struct Registry {
    std::mutex mu;
    std::unordered_map<std::string, TextAccessibilityNode> nodes;
};

Registry& registry() {
    static Registry r;
    return r;
}

}  // namespace

std::string_view accessibility_backend_name() noexcept {
    // Explicitly distinguishes the Linux scaffold from the default
    // "none" backend, so external tooling can detect that the platform
    // build is wired in even though it doesn't talk to AT-SPI yet.
    // Flip to "linux-accesskit" when the AccessKit adapter actually
    // emits TreeUpdates.
    return "linux-accesskit-stub";
}

void register_text_accessibility_node(const TextAccessibilityNode& node) {
    auto& r = registry();
    std::lock_guard<std::mutex> lock(r.mu);
    r.nodes[node.id] = node;
    // Real backend: build accesskit_tree_update with one node carrying
    // accesskit_role_for(node.role) + node.text as its Name, then call
    // accesskit_unix_adapter_update_if_active(...) — see the file-header
    // comment for the full wiring recipe.
    (void)accesskit_role_for;  // suppress "unused" until the wiring lands
}

void unregister_text_accessibility_node(std::string_view id) {
    auto& r = registry();
    std::lock_guard<std::mutex> lock(r.mu);
    r.nodes.erase(std::string(id));
    // Real backend: emit a TreeUpdate that removes the node by id, then
    // call accesskit_unix_adapter_update_if_active(...).
}

std::vector<TextAccessibilityNode> snapshot_accessibility_nodes() {
    auto& r = registry();
    std::lock_guard<std::mutex> lock(r.mu);
    std::vector<TextAccessibilityNode> out;
    out.reserve(r.nodes.size());
    for (const auto& [_, node] : r.nodes) {
        out.push_back(node);
    }
    return out;
}

// Test hook: exposes the integer role mapping so the Linux unit test can
// verify TextAccessibilityRole → accesskit::Role without needing the
// real AccessKit headers. Returns the same numeric constants the real
// backend will pass to AccessKit's C API.
extern "C" int pulp_text_accessibility_role_linux(int pulp_role) {
    return accesskit_role_for(static_cast<TextAccessibilityRole>(pulp_role));
}

}  // namespace pulp::view

#endif  // __linux__
