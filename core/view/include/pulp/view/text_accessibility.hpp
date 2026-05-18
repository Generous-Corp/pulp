#pragma once

// Cross-platform text accessibility scaffold (font v2 Slice 2.6).
//
// Most of Pulp's painted text bypasses the platform a11y APIs
// (NSAccessibility on macOS, UIA on Windows, AccessKit on Linux) because
// glyphs land directly on the GPU canvas without ever entering the
// platform's accessibility tree. This header defines the cross-platform
// SURFACE — a value type that captures the minimum a screen reader needs
// and a registration entry point — plus a no-op default backend that
// stores nodes in a process-local table.
//
// The full per-platform glue (NSAccessibility selectors, UIA com objects,
// AccessKit serialization) is intentionally out of scope here; that lands
// in follow-up slices that overlay this default backend.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::view {

/// Coarse semantic role for a piece of accessible text. The platform
/// backend maps this to its native role (NSAccessibilityStaticTextRole,
/// UIA_TextControlTypeId, AccessKit Role::StaticText, etc.).
enum class TextAccessibilityRole : std::uint8_t {
    Label,
    Button,
    TextEditor,
    Heading,
    Other,
};

/// A single accessible text node — the minimum a screen reader needs to
/// describe a piece of painted text. All offsets are byte offsets into
/// `text` (UTF-8) or code-unit offsets when expressed in UTF-16.
struct TextAccessibilityNode {
    /// Caller-stable identifier. Used as the registration key.
    std::string id;

    /// UTF-8 content of the node.
    std::string text;

    /// Semantic role hint.
    TextAccessibilityRole role = TextAccessibilityRole::Label;

    /// Cluster boundaries in UTF-8 byte offsets. Each entry is the
    /// starting byte index of a grapheme cluster; the vector should
    /// typically include 0 and text.size() as bookends.
    std::vector<std::size_t> cluster_boundaries_utf8;

    /// Cluster boundaries in UTF-16 code units. Mirrors the UTF-8
    /// vector but expressed in the encoding most platform a11y APIs
    /// (NSAccessibility, UIA) expose.
    std::vector<std::size_t> cluster_boundaries_utf16;

    /// Active selection range in UTF-8 byte offsets. Both fields == 0
    /// means "no selection" (i.e. the node has no caret).
    std::size_t selection_start_utf8 = 0;
    std::size_t selection_end_utf8 = 0;
};

/// Returns the active accessibility backend identifier. The default
/// backend is "none"; per-platform overlays return "macos-ax",
/// "windows-uia", or "linux-accesskit". Stable across calls for the
/// lifetime of the process.
std::string_view accessibility_backend_name() noexcept;

/// Register or replace a text node with the active a11y backend. The
/// default backend stores the node in a process-local table that tests
/// can read back via `snapshot_accessibility_nodes()`. Registering with
/// an `id` that already exists replaces the previous entry — registration
/// is idempotent on the id.
void register_text_accessibility_node(const TextAccessibilityNode& node);

/// Drop the node registered under `id`. Silently does nothing if no
/// such node is registered.
void unregister_text_accessibility_node(std::string_view id);

/// Read back the currently-registered nodes. Returns by value (a copy)
/// so callers do not race with concurrent register/unregister activity.
/// Order is unspecified.
std::vector<TextAccessibilityNode> snapshot_accessibility_nodes();

}  // namespace pulp::view
