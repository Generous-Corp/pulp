// macOS NSAccessibility backend for TextAccessibilityNode (font v2 Slice 2.6).
//
// Verifies the platform overlay defined in
// core/view/platform/mac/text_accessibility_macos.mm. The cross-platform
// surface (snapshot, register, unregister, idempotency, threading) is
// covered by test_text_accessibility.cpp; this suite pins down the
// macOS-specific behavior: the backend name flips to "macos-ax", an
// NSAccessibilityElement is created per registered node with the
// correct role/label/value, and the selection range round-trips through
// UTF-8 → UTF-16 conversion.

#include <TargetConditionals.h>

#if TARGET_OS_OSX

#import <AppKit/AppKit.h>

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/text_accessibility.hpp>
#include <pulp/view/view.hpp>

#include <string>

using namespace pulp::view;

// Declared in text_accessibility_macos.mm. We rely on extern "C" so
// the test file doesn't need a private header just to expose one
// debug-only entry point.
extern "C" void* pulp_text_accessibility_lookup_macos(const char* id_utf8);

namespace {

void clear_registry() {
    for (const auto& node : snapshot_accessibility_nodes()) {
        unregister_text_accessibility_node(node.id);
    }
    REQUIRE(snapshot_accessibility_nodes().empty());
}

NSAccessibilityElement* lookup(const std::string& id) {
    return (__bridge NSAccessibilityElement*)
        pulp_text_accessibility_lookup_macos(id.c_str());
}

}  // namespace

TEST_CASE("macOS text-a11y: backend name flips to 'macos-ax'",
          "[view][text-a11y][macos][issue-2255]") {
    // Pinned cross-platform; on macOS the bridge must override this
    // symbol so VoiceOver-aware harnesses can detect the real backend.
    REQUIRE(accessibility_backend_name() == "macos-ax");
}

TEST_CASE("macOS text-a11y: Label node creates an NSAccessibilityElement "
          "with StaticText role",
          "[view][text-a11y][macos][issue-2255]") {
    clear_registry();

    TextAccessibilityNode node;
    node.id = "macos-label-1";
    node.text = "Hello";
    node.role = TextAccessibilityRole::Label;
    register_text_accessibility_node(node);

    auto* el = lookup(node.id);
    REQUIRE(el != nil);
    REQUIRE([[el accessibilityRole] isEqualToString:NSAccessibilityStaticTextRole]);
    REQUIRE([[el accessibilityLabel] isEqualToString:@"Hello"]);
    REQUIRE([[el accessibilityValue] isEqualToString:@"Hello"]);

    clear_registry();
}

TEST_CASE("macOS text-a11y: Button node maps to NSAccessibilityButtonRole",
          "[view][text-a11y][macos][issue-2255]") {
    clear_registry();

    TextAccessibilityNode node;
    node.id = "macos-btn-1";
    node.text = "Save";
    node.role = TextAccessibilityRole::Button;
    register_text_accessibility_node(node);

    auto* el = lookup(node.id);
    REQUIRE(el != nil);
    REQUIRE([[el accessibilityRole] isEqualToString:NSAccessibilityButtonRole]);
    REQUIRE([[el accessibilityLabel] isEqualToString:@"Save"]);

    clear_registry();
}

TEST_CASE("macOS text-a11y: TextEditor node maps to NSAccessibilityTextFieldRole",
          "[view][text-a11y][macos][issue-2255]") {
    clear_registry();

    TextAccessibilityNode node;
    node.id = "macos-edit-1";
    node.text = "draft";
    node.role = TextAccessibilityRole::TextEditor;
    register_text_accessibility_node(node);

    auto* el = lookup(node.id);
    REQUIRE(el != nil);
    REQUIRE([[el accessibilityRole] isEqualToString:NSAccessibilityTextFieldRole]);

    clear_registry();
}

TEST_CASE("macOS text-a11y: TextEditor with ASCII selection round-trips "
          "UTF-8 byte range → UTF-16 code units",
          "[view][text-a11y][macos][issue-2255]") {
    clear_registry();

    // "hello world" — 11 ASCII bytes, all 1 UTF-16 code unit. Selecting
    // bytes 6..11 ("world") must produce NSRange{6, 5}.
    TextAccessibilityNode node;
    node.id = "macos-sel-1";
    node.text = "hello world";
    node.role = TextAccessibilityRole::TextEditor;
    node.selection_start_utf8 = 6;
    node.selection_end_utf8 = 11;
    register_text_accessibility_node(node);

    auto* el = lookup(node.id);
    REQUIRE(el != nil);
    const NSRange range = [el accessibilitySelectedTextRange];
    REQUIRE(range.location == 6);
    REQUIRE(range.length == 5);

    clear_registry();
}

TEST_CASE("macOS text-a11y: selection range honors cluster_boundaries_utf16 "
          "when present (multi-byte UTF-8)",
          "[view][text-a11y][macos][issue-2255]") {
    clear_registry();

    // "héllo" — 'h' (1 byte / 1 unit), 'é' (U+00E9, 2 bytes / 1 unit
    // in UTF-16), 'l' 'l' 'o' (1/1 each). Boundaries at 0, 1, 3, 4, 5,
    // 6 in UTF-8; 0, 1, 2, 3, 4, 5 in UTF-16. Selecting bytes 1..3
    // (the 'é') must surface as UTF-16 range {1, 1}.
    TextAccessibilityNode node;
    node.id = "macos-sel-2";
    node.text = "h\xC3\xA9llo";  // h, é, l, l, o
    node.role = TextAccessibilityRole::TextEditor;
    node.cluster_boundaries_utf8  = {0, 1, 3, 4, 5, 6};
    node.cluster_boundaries_utf16 = {0, 1, 2, 3, 4, 5};
    node.selection_start_utf8 = 1;
    node.selection_end_utf8 = 3;
    register_text_accessibility_node(node);

    auto* el = lookup(node.id);
    REQUIRE(el != nil);
    const NSRange range = [el accessibilitySelectedTextRange];
    REQUIRE(range.location == 1);
    REQUIRE(range.length == 1);

    clear_registry();
}

TEST_CASE("macOS text-a11y: no-selection nodes produce {NSNotFound, 0} range",
          "[view][text-a11y][macos][issue-2255]") {
    clear_registry();

    TextAccessibilityNode node;
    node.id = "macos-no-sel";
    node.text = "static";
    node.role = TextAccessibilityRole::Label;
    register_text_accessibility_node(node);

    auto* el = lookup(node.id);
    REQUIRE(el != nil);
    const NSRange range = [el accessibilitySelectedTextRange];
    REQUIRE(range.location == NSNotFound);
    REQUIRE(range.length == 0);

    clear_registry();
}

TEST_CASE("macOS text-a11y: unregister drops the NSAccessibilityElement",
          "[view][text-a11y][macos][issue-2255]") {
    clear_registry();

    TextAccessibilityNode node;
    node.id = "macos-tmp";
    node.text = "ephemeral";
    node.role = TextAccessibilityRole::Label;
    register_text_accessibility_node(node);
    REQUIRE(lookup(node.id) != nil);

    unregister_text_accessibility_node(node.id);
    REQUIRE(lookup(node.id) == nil);

    // C++ shadow snapshot must also drop the entry — cross-platform
    // contract still holds on the macOS overlay.
    REQUIRE(snapshot_accessibility_nodes().empty());
}

TEST_CASE("macOS text-a11y: PulpPluginView::accessibilityChildren surfaces "
          "registered text elements to VoiceOver",
          "[view][text-a11y][macos][issue-2255]") {
    clear_registry();
    @autoreleasepool {
        [NSApplication sharedApplication];

        View root;
        PluginViewHost::Options opts;
        opts.size = {200, 100};
        opts.use_gpu = false;
        auto host = PluginViewHost::create(root, opts);
        REQUIRE(host != nullptr);

        TextAccessibilityNode node;
        node.id = "macos-discoverable";
        node.text = "Hi VoiceOver";
        node.role = TextAccessibilityRole::Label;
        register_text_accessibility_node(node);

        auto* nsview = (__bridge NSView*)host->native_handle();
        REQUIRE(nsview != nil);

        NSArray* children = [nsview accessibilityChildren];
        REQUIRE(children != nil);

        // The merged children array must contain a node whose label
        // matches the registered text. Pre-fix, the AX tree ignored the
        // text registry entirely and Codex's P1 review (#2307) flagged
        // that VoiceOver could never reach painted text registered
        // through the scaffold.
        BOOL found = NO;
        for (id child in children) {
            if ([child respondsToSelector:@selector(accessibilityLabel)]) {
                NSString* label = [child accessibilityLabel];
                if ([label isEqualToString:@"Hi VoiceOver"]) {
                    found = YES;
                    break;
                }
            }
        }
        REQUIRE(found);
    }
    clear_registry();
}

TEST_CASE("macOS text-a11y: same-id re-register replaces label/role/selection "
          "without churning element identity",
          "[view][text-a11y][macos][issue-2255]") {
    clear_registry();

    TextAccessibilityNode first;
    first.id = "macos-replace";
    first.text = "old";
    first.role = TextAccessibilityRole::Label;
    register_text_accessibility_node(first);
    auto* el_before = lookup(first.id);
    REQUIRE(el_before != nil);

    TextAccessibilityNode second;
    second.id = "macos-replace";
    second.text = "new";
    second.role = TextAccessibilityRole::Heading;
    register_text_accessibility_node(second);
    auto* el_after = lookup(second.id);
    REQUIRE(el_after != nil);

    // Re-register must mutate the existing element rather than swap
    // the pointer — assistive tech tracks elements by identity.
    REQUIRE(el_before == el_after);
    REQUIRE([[el_after accessibilityLabel] isEqualToString:@"new"]);
    // Heading falls back to StaticText (mapped role).
    REQUIRE([[el_after accessibilityRole] isEqualToString:NSAccessibilityStaticTextRole]);

    clear_registry();
}

#endif  // TARGET_OS_OSX
