// macOS NSAccessibility backend for TextAccessibilityNode (font v2 Slice 2.6).
//
// Replaces the default "none" backend defined in
// core/view/src/text_accessibility.cpp on Apple platforms. Painted text
// that registers a TextAccessibilityNode through this backend gets
// surfaced to VoiceOver as a real NSAccessibilityElement keyed by the
// caller-stable node id.
//
// Memory model: Pulp's .mm files compile without ARC, so we retain the
// Objective-C wrapper instances ourselves via an NSMutableDictionary
// (which retains values). Unregister removes the value, which releases
// the retained element. The C++ shadow map mirrors the registered nodes
// so snapshot_accessibility_nodes() stays a pure cross-platform read —
// callers don't have to round-trip through NSAccessibility selectors.
//
// Windows UIA + Linux AccessKit are deferred to follow-up PRs and stay
// on the default "none" backend.

#include <TargetConditionals.h>
#if TARGET_OS_OSX || TARGET_OS_IPHONE

#include <pulp/view/text_accessibility.hpp>

#import <Foundation/Foundation.h>
#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#else
#import <UIKit/UIKit.h>
#endif

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// ── PulpTextAccessibilityElement ────────────────────────────────────────────
//
// One instance per registered node. Subclasses NSAccessibilityElement so
// VoiceOver can read accessibilityLabel / accessibilityValue /
// accessibilityRole / accessibilitySelectedTextRange directly. Pulp
// owns this object via the registry's NSMutableDictionary; AppKit also
// retains it once it lands in -accessibilityChildren on a host view.
//
// iOS doesn't expose NSAccessibilityElement (the comparable surface is
// UIAccessibilityElement), so the iOS leg keeps the registry as a thin
// C++ shadow only. The accessibility_backend_name() still returns
// "macos-ax" on macOS; on iOS we deliberately return "none" so the
// platform-level UIAccessibility wiring (a future slice) can detect the
// gap.

#if TARGET_OS_OSX

@interface PulpTextAccessibilityElement : NSAccessibilityElement {
@public
    NSString* _identifier;
    NSString* _label;
    NSString* _value;
    NSAccessibilityRole _role;
    NSRange _selectedRange;
}
@end

@implementation PulpTextAccessibilityElement

- (NSAccessibilityRole)accessibilityRole {
    return _role ? _role : NSAccessibilityUnknownRole;
}

- (NSString*)accessibilityLabel {
    return _label;
}

- (id)accessibilityValue {
    return _value;
}

- (NSString*)accessibilityIdentifier {
    return _identifier;
}

- (NSRange)accessibilitySelectedTextRange {
    return _selectedRange;
}

- (BOOL)isAccessibilityElement {
    return YES;
}

- (void)dealloc {
    [_identifier release];
    [_label release];
    [_value release];
    [super dealloc];
}

@end

#endif  // TARGET_OS_OSX

namespace pulp::view {

namespace {

#if TARGET_OS_OSX
// Map an enum TextAccessibilityRole → NSAccessibilityRole string.
NSAccessibilityRole ns_role_for(TextAccessibilityRole role) {
    switch (role) {
        case TextAccessibilityRole::Label:      return NSAccessibilityStaticTextRole;
        case TextAccessibilityRole::Button:     return NSAccessibilityButtonRole;
        case TextAccessibilityRole::TextEditor: return NSAccessibilityTextFieldRole;
        case TextAccessibilityRole::Heading:    return NSAccessibilityStaticTextRole;
        case TextAccessibilityRole::Other:
        default:                                return NSAccessibilityUnknownRole;
    }
}
#endif

// Convert a UTF-8 byte offset into a UTF-16 code-unit offset for the
// given UTF-8 text. Prefers the precomputed cluster table when the
// offset lines up with a cluster boundary (the common case for shaped
// painted text), otherwise walks the UTF-8 bytes computing the UTF-16
// width of each code point on the fly. Saturates at the end of the
// string so callers passing past-the-end offsets don't read OOB.
std::size_t utf8_byte_to_utf16_unit(std::string_view text,
                                    const std::vector<std::size_t>& boundaries_utf8,
                                    const std::vector<std::size_t>& boundaries_utf16,
                                    std::size_t byte_offset) {
    if (byte_offset >= text.size()) {
        byte_offset = text.size();
    }

    // Fast path: precomputed cluster tables. If the offset is a
    // boundary, the corresponding UTF-16 boundary is the answer
    // directly.
    if (!boundaries_utf8.empty()
        && boundaries_utf8.size() == boundaries_utf16.size()) {
        for (std::size_t i = 0; i < boundaries_utf8.size(); ++i) {
            if (boundaries_utf8[i] == byte_offset) {
                return boundaries_utf16[i];
            }
        }
    }

    // Slow path: walk UTF-8 bytes. We don't materialize a UTF-16
    // string — just count the code-unit width of each code point. A
    // BMP code point is 1 unit, supplementary code points are 2.
    std::size_t utf16 = 0;
    std::size_t i = 0;
    while (i < byte_offset) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            utf16 += 1;
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            utf16 += 1;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            utf16 += 1;
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            // Supplementary plane → surrogate pair.
            utf16 += 2;
            i += 4;
        } else {
            // Invalid UTF-8 lead byte; treat as a single replacement
            // unit so we don't loop forever.
            utf16 += 1;
            i += 1;
        }
    }
    return utf16;
}

struct Registry {
    std::mutex mu;
    // Shadow map for cross-platform snapshot semantics. The same
    // table the default "none" backend keeps; tests can read it back
    // without touching NSAccessibility.
    std::unordered_map<std::string, TextAccessibilityNode> nodes;
#if TARGET_OS_OSX
    // Per-id retained PulpTextAccessibilityElement instances. The
    // NSMutableDictionary retains values, so registering implicitly
    // owns the element; unregistering releases it.
    NSMutableDictionary<NSString*, PulpTextAccessibilityElement*>* elements;
#endif
};

Registry& registry() {
    static Registry* r = []{
        auto* out = new Registry();
#if TARGET_OS_OSX
        out->elements = [[NSMutableDictionary alloc] init];
#endif
        return out;
    }();
    return *r;
}

}  // namespace

std::string_view accessibility_backend_name() noexcept {
#if TARGET_OS_OSX
    return "macos-ax";
#else
    // iOS lane still rides the default "none" surface; the
    // UIAccessibility overlay is a follow-up slice.
    return "none";
#endif
}

void register_text_accessibility_node(const TextAccessibilityNode& node) {
    auto& r = registry();
    std::lock_guard<std::mutex> lock(r.mu);
    // Idempotent on id: operator[] + assignment so a second register
    // with the same id replaces the first. Same contract as the
    // default backend.
    r.nodes[node.id] = node;

#if TARGET_OS_OSX
    NSString* nsId = [[NSString alloc] initWithUTF8String:node.id.c_str()];
    PulpTextAccessibilityElement* el = [r.elements objectForKey:nsId];
    BOOL inserted = NO;
    if (!el) {
        el = [[PulpTextAccessibilityElement alloc] init];
        el->_identifier = [nsId retain];
        [r.elements setObject:el forKey:nsId];
        // setObject: retained the element; release our local +1.
        [el release];
        inserted = YES;
    }
    (void)inserted;

    // Replace label/value/role in-place so a re-register with the same
    // id doesn't churn the NSAccessibility identity (assistive tech
    // tracks elements by pointer identity).
    [el->_label release];
    el->_label = [[NSString alloc] initWithUTF8String:node.text.c_str()];
    [el->_value release];
    el->_value = [[NSString alloc] initWithUTF8String:node.text.c_str()];
    el->_role = ns_role_for(node.role);

    if (node.selection_start_utf8 == 0 && node.selection_end_utf8 == 0) {
        // No selection. NSRange{NSNotFound, 0} is the conventional
        // "no selection" sentinel for NSAccessibility.
        el->_selectedRange = NSMakeRange(NSNotFound, 0);
    } else {
        const std::size_t start16 = utf8_byte_to_utf16_unit(
            node.text, node.cluster_boundaries_utf8,
            node.cluster_boundaries_utf16, node.selection_start_utf8);
        const std::size_t end16 = utf8_byte_to_utf16_unit(
            node.text, node.cluster_boundaries_utf8,
            node.cluster_boundaries_utf16, node.selection_end_utf8);
        const std::size_t len = (end16 >= start16) ? (end16 - start16) : 0;
        el->_selectedRange = NSMakeRange(static_cast<NSUInteger>(start16),
                                         static_cast<NSUInteger>(len));
    }

    [nsId release];
#endif
}

void unregister_text_accessibility_node(std::string_view id) {
    auto& r = registry();
    std::lock_guard<std::mutex> lock(r.mu);
    const std::string key(id);
    r.nodes.erase(key);
#if TARGET_OS_OSX
    NSString* nsId = [[NSString alloc] initWithUTF8String:key.c_str()];
    [r.elements removeObjectForKey:nsId];
    [nsId release];
#endif
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

// Test-only lookup. Returns the retained NSAccessibilityElement for the
// given id, or nil if no such node is registered. Defined in this TU
// so tests can verify the macOS bridge without exposing a public header
// dependency on Foundation.
//
// Returns a non-owning pointer suitable for an `(__bridge
// NSAccessibilityElement*)` cast on the test side; the registry
// continues to own the +1 retain.
#if TARGET_OS_OSX
extern "C" void* pulp_text_accessibility_lookup_macos(const char* id_utf8) {
    if (!id_utf8) return nullptr;
    auto& r = registry();
    std::lock_guard<std::mutex> lock(r.mu);
    NSString* nsId = [[NSString alloc] initWithUTF8String:id_utf8];
    PulpTextAccessibilityElement* el = [r.elements objectForKey:nsId];
    [nsId release];
    return (__bridge void*)el;
}

// Snapshot the registered NSAccessibilityElements as an autoreleased
// NSArray. Host views (PulpPluginView, PulpView) call this from
// -accessibilityChildren so VoiceOver can actually reach the elements;
// without it the registry would just be a private map and assistive
// tech would never see the painted text. Returns an empty array when
// no nodes are registered.
//
// The array contains strong refs to the live elements — VoiceOver may
// hold pointers across multiple -accessibilityChildren calls, and the
// registry's NSMutableDictionary keeps each element retained for its
// lifetime, so the pointers stay valid until the corresponding id is
// unregistered.
NSArray* pulp_text_accessibility_all_elements_macos() {
    auto& r = registry();
    std::lock_guard<std::mutex> lock(r.mu);
    // -allValues is documented to return an autoreleased array
    // containing the dictionary's current value objects.
    return [r.elements allValues];
}
#endif

}  // namespace pulp::view

#endif  // TARGET_OS_OSX || TARGET_OS_IPHONE
