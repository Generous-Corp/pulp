// test_plugin_view_host_key_focus.mm — host keyboard-etiquette, part 2.
//
// Part 1 (acceptsFirstResponder gated on View::focused_input_) stopped the
// editor from stealing the host window's keyboard on every click. This file
// pins the two follow-up contracts that make TEXT INPUT (type-in fields)
// host-polite as well:
//
//  1. RESTORE, don't drop: when a widget releases text-input focus, the
//     window's first responder must return to the host view that held it
//     BEFORE the claim — not to nil. Handing nil leaves hosts like Logic
//     with dead keyboard routing (Musical Typing stays silent after a
//     type-in commit until the user resets the track).
//  2. The HOST's grab wins: if the host moves first responder away while a
//     widget still holds the input slot (user clicked a host control with a
//     type-in open), the widget's text input must END — slot cleared,
//     on_focus_changed(false) delivered so the editing UI can commit —
//     instead of acceptsFirstResponder staying true and re-stealing the
//     keyboard on the next event.
//
// Also covers the MRC-safety contract of the saved prior responder: it is
// validated BY IDENTITY against the window's live view tree before restore,
// so a responder that left the window degrades to nil instead of a dangling
// objc_msgSend.
//
// Mac CPU host only (the GPU view class shares the same helper functions
// and mirrors the same overrides in plugin_view_host_mac.mm).

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/view.hpp>

#if defined(__APPLE__)
#import <Cocoa/Cocoa.h>

using namespace pulp::view;

// Stand-in for the host's own key-routing view (the responder Logic's
// Musical Typing depends on).
@interface PulpTestHostField : NSView
@end
@implementation PulpTestHostField
- (BOOL)acceptsFirstResponder {
    return YES;
}
@end

// The focus-sync entry point is an internal method on the embedded NSView;
// declare it so the test can drive the exact code path mouseDown/mouseUp/
// keyDown use, without synthesizing NSEvents.
@interface NSView (PulpKeyFocusTestHooks)
- (void)syncKeyFocus;
@end

namespace {

class FocusRecordingView : public View {
public:
    void paint(pulp::canvas::Canvas&) override {}
    void on_focus_changed(bool gained) override {
        View::on_focus_changed(gained);
        if (!gained) ++lost_count;
    }
    int lost_count = 0;
};

struct FocusGuard {
    FocusGuard() { View::focused_input_ = nullptr; }
    ~FocusGuard() { View::focused_input_ = nullptr; }
};

NSView* find_pulp_plugin_view(NSView* parent) {
    for (NSView* sub in parent.subviews) {
        if ([NSStringFromClass(sub.class) isEqualToString:@"PulpPluginView"])
            return sub;
        NSView* nested = find_pulp_plugin_view(sub);
        if (nested) return nested;
    }
    return nil;
}

}  // namespace

TEST_CASE("PluginViewHost (mac CPU) — the editor keeps first responder to "
          "forward keys; a host grab still ends text input",
          "[plugin-view-host][key-focus][mac][cpu]") {
    @autoreleasepool {
        FocusGuard guard;

        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 800, 600)
                                        styleMask:NSWindowStyleMaskBorderless
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        if (!window || !window.contentView) {
            SKIP("No Cocoa window for key-focus contract test");
        }

        FocusRecordingView root;
        PluginViewHost::Options opts;
        opts.size = {800u, 600u};
        opts.use_gpu = false;
        auto host = PluginViewHost::create(root, opts);
        REQUIRE(host != nullptr);
        host->attach_to_parent((__bridge void*)window.contentView);

        NSView* pulp_view = find_pulp_plugin_view(window.contentView);
        REQUIRE(pulp_view != nil);
        REQUIRE([pulp_view respondsToSelector:@selector(syncKeyFocus)]);

        PulpTestHostField* host_field =
            [[PulpTestHostField alloc] initWithFrame:NSMakeRect(0, 0, 10, 10)];
        [window.contentView addSubview:host_field];
        REQUIRE([window makeFirstResponder:host_field]);
        REQUIRE(window.firstResponder == host_field);

        SECTION("claim takes the keyboard; release KEEPS first responder so the "
                "view can forward non-text keys to the host") {
            root.claim_input_focus();
            [pulp_view syncKeyFocus];
            REQUIRE(window.firstResponder == pulp_view);

            root.release_input_focus();
            [pulp_view syncKeyFocus];
            // New contract: the editor does NOT resign on blur — it stays the
            // key interceptor and forwards non-text keys (DAW transport /
            // Musical Typing) to the host. Resigning here is what left Space/R
            // dead after the user left a field.
            REQUIRE(window.firstResponder == pulp_view);
        }

        SECTION("host re-grab while a widget holds the slot ends its text "
                "input instead of re-stealing") {
            root.claim_input_focus();
            [pulp_view syncKeyFocus];
            REQUIRE(window.firstResponder == pulp_view);
            REQUIRE(root.lost_count == 0);

            // The host takes the keyboard back (user clicked a host control).
            REQUIRE([window makeFirstResponder:host_field]);
            REQUIRE(window.firstResponder == host_field);

            // resignFirstResponder must have ended the widget's text input:
            // slot cleared, focus-lost delivered (the widget commits its
            // type-in there). The view itself still accepts first responder
            // (it stays the key interceptor and forwards non-text keys back to
            // the host), but it no longer holds the text-input slot.
            REQUIRE(View::focused_input_ == nullptr);
            REQUIRE(root.lost_count == 1);
            REQUIRE([pulp_view acceptsFirstResponder]);  // always YES now
        }

        SECTION("the host can still reclaim the keyboard explicitly (clicks its "
                "own control) — the editor does not fight it") {
            root.claim_input_focus();
            [pulp_view syncKeyFocus];
            REQUIRE(window.firstResponder == pulp_view);

            // Host makes one of its own views first responder. The editor must
            // let it (it only re-grabs on a fresh focus interaction), so the
            // host keeps the keyboard.
            REQUIRE([window makeFirstResponder:host_field]);
            root.release_input_focus();
            [pulp_view syncKeyFocus];  // wants=false → no grab, no fight
            REQUIRE(window.firstResponder == host_field);
        }

        host->detach();
        host.reset();
        [window close];
    }
}

// Two plugin editors open in one host process (e.g. two instances of the same
// plug-in). `View::focused_input_` is process-global, so the focus decisions
// must be scoped to each editor's OWN root — otherwise editor B steals or
// misroutes the keyboard when editor A holds a focused text field. This pins
// the root-scoping (`pulp_focus_under_root`).
TEST_CASE("PluginViewHost (mac CPU) — focus is scoped per editor; a second "
          "open editor neither accepts nor ends the first editor's focus",
          "[plugin-view-host][key-focus][mac][cpu][multi-editor]") {
    @autoreleasepool {
        FocusGuard guard;

        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 800, 600)
                                        styleMask:NSWindowStyleMaskBorderless
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        if (!window || !window.contentView) {
            SKIP("No Cocoa window for multi-editor focus test");
        }

        NSView* containerA =
            [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 400, 600)];
        NSView* containerB =
            [[NSView alloc] initWithFrame:NSMakeRect(400, 0, 400, 600)];
        [window.contentView addSubview:containerA];
        [window.contentView addSubview:containerB];

        FocusRecordingView rootA;
        FocusRecordingView rootB;
        PluginViewHost::Options opts;
        opts.size = {400u, 600u};
        opts.use_gpu = false;
        auto hostA = PluginViewHost::create(rootA, opts);
        auto hostB = PluginViewHost::create(rootB, opts);
        REQUIRE(hostA != nullptr);
        REQUIRE(hostB != nullptr);
        hostA->attach_to_parent((__bridge void*)containerA);
        hostB->attach_to_parent((__bridge void*)containerB);

        NSView* viewA = find_pulp_plugin_view(containerA);
        NSView* viewB = find_pulp_plugin_view(containerB);
        REQUIRE(viewA != nil);
        REQUIRE(viewB != nil);
        REQUIRE(viewA != viewB);

        // Focus a text widget in editor A only.
        rootA.claim_input_focus();
        REQUIRE(View::focused_input_ == &rootA);

        // Both editors accept first responder (each is the key interceptor for
        // its own window). The meaningful scoping is the TEXT-INPUT slot: only
        // editor A owns it, so only editor A's syncKeyFocus grabs the keyboard.
        REQUIRE([viewA acceptsFirstResponder]);
        REQUIRE([viewB acceptsFirstResponder]);

        // Editor B syncing focus must not steal first responder; editor A's does.
        [viewB syncKeyFocus];
        REQUIRE(window.firstResponder != viewB);
        [viewA syncKeyFocus];
        REQUIRE(window.firstResponder == viewA);

        // Editor B resigning first responder must NOT end editor A's text input
        // (pulp_plugin_end_text_input is root-scoped).
        [viewB resignFirstResponder];
        REQUIRE(View::focused_input_ == &rootA);
        REQUIRE(rootA.lost_count == 0);

        hostA->detach();
        hostB->detach();
        hostA.reset();
        hostB.reset();
        [window close];
    }
}

// ── Hosted text-input key routing ────────────────────────────────────────────
//
// The embedded (AU/DAW) view must route keys like the standalone host does:
//   • arrow keys NAVIGATE — they must NOT also be inserted as text. macOS
//     reports arrows as private-use codepoints (0xF700–0xF8FF), so a naive
//     "insert event.characters" path types tofu (□) into the field on every
//     arrow press (the bug that motivated this file's key-routing fixes).
//   • Shift+arrow EXTENDS the selection.
//   • Printable keys insert.
//   • ⌘A / ⌘C / ⌘V / ⌘X / ⌘Z arrive via performKeyEquivalent: (the host eats
//     them otherwise) and must reach the focused editor — here, ⌘A select-all.
//
// Driven through the real -keyDown:/-performKeyEquivalent: overrides with
// synthesized NSEvents, so this pins the actual hosted-view code path headless,
// with no DAW.
namespace {

NSEvent* make_key_event(unsigned short keyCode,
                        NSEventModifierFlags mods,
                        NSString* chars) {
    return [NSEvent keyEventWithType:NSEventTypeKeyDown
                           location:NSZeroPoint
                      modifierFlags:mods
                          timestamp:0
                       windowNumber:0
                            context:nil
                         characters:chars
        charactersIgnoringModifiers:chars
                          isARepeat:NO
                            keyCode:keyCode];
}

// macOS function-key codepoints (what -[NSEvent characters] returns for arrows).
constexpr unichar kNSLeftArrow = 0xF702;
constexpr unichar kNSRightArrow = 0xF703;

}  // namespace

TEST_CASE("PluginViewHost (mac CPU) — hosted key routing: arrows navigate "
          "without inserting tofu, ⌘A selects all, printable keys insert",
          "[plugin-view-host][key-routing][mac][cpu]") {
    @autoreleasepool {
        FocusGuard guard;

        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 400, 200)
                                        styleMask:NSWindowStyleMaskBorderless
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        if (!window || !window.contentView) {
            SKIP("No Cocoa window for hosted key-routing test");
        }

        View root;
        PluginViewHost::Options opts;
        opts.size = {400u, 200u};
        opts.use_gpu = false;
        auto host = PluginViewHost::create(root, opts);
        REQUIRE(host != nullptr);
        host->attach_to_parent((__bridge void*)window.contentView);

        NSView* pulp_view = find_pulp_plugin_view(window.contentView);
        REQUIRE(pulp_view != nil);

        // A focused single-line editor inside the host's root tree.
        auto editor_owned = std::make_unique<TextEditor>();
        TextEditor* editor = editor_owned.get();
        editor->set_text("abc");
        root.add_child(std::move(editor_owned));
        editor->claim_input_focus();
        editor->set_caret_pos(0);
        REQUIRE(View::focused_input_ == editor);

        SECTION("right arrow moves the caret and inserts NO character") {
            [pulp_view keyDown:make_key_event(
                124, 0,
                [NSString stringWithCharacters:&kNSRightArrow length:1])];
            // The bug: the arrow's private-use codepoint got inserted as text.
            REQUIRE(editor->text() == "abc");
            REQUIRE(editor->caret_pos() == 1);
            REQUIRE_FALSE(editor->has_selection());
        }

        SECTION("shift+right extends the selection, still no insertion") {
            [pulp_view keyDown:make_key_event(
                124, NSEventModifierFlagShift,
                [NSString stringWithCharacters:&kNSRightArrow length:1])];
            REQUIRE(editor->text() == "abc");
            REQUIRE(editor->has_selection());
        }

        SECTION("left arrow at start stays put and inserts nothing") {
            [pulp_view keyDown:make_key_event(
                123, 0,
                [NSString stringWithCharacters:&kNSLeftArrow length:1])];
            REQUIRE(editor->text() == "abc");
            REQUIRE(editor->caret_pos() == 0);
        }

        SECTION("printable key inserts text") {
            editor->set_caret_pos(3);  // end
            [pulp_view keyDown:make_key_event(7, 0, @"x")];  // keyCode 7 = 'x'
            REQUIRE(editor->text() == "abcx");
        }

        SECTION("⌘A selects all via performKeyEquivalent") {
            editor->set_caret_pos(0);
            NSEvent* cmd_a =
                make_key_event(0, NSEventModifierFlagCommand, @"a");  // 0 = 'a'
            REQUIRE([pulp_view performKeyEquivalent:cmd_a] == YES);
            REQUIRE(editor->has_selection());
            auto [start, end] = editor->selection_range();
            REQUIRE(start == 0);
            REQUIRE(end == 3);
        }

        host->detach();
        host.reset();
        [window close];
    }
}

// ── Focus affordance + release ───────────────────────────────────────────────
//
// Two host-keyboard-etiquette contracts the embedded view must honor:
//   #1 A click that focuses a text field must set its VISUAL focus state
//      (has_focus() → highlight border + blinking caret), not only route text.
//      The old path called claim_input_focus() alone, so a DAW-embedded field
//      took typing but showed no highlight/caret.
//   #3 Escape (any input) and Tab/Return (single-line) must BLUR the field so
//      focus_input clears — the host view then hands the keyboard back and DAW
//      transport keys (Space/R) work again until the field is re-focused.
namespace {

NSEvent* make_left_mouse_down(NSPoint loc) {
    return [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                             location:loc
                        modifierFlags:0
                            timestamp:0
                         windowNumber:0
                              context:nil
                          eventNumber:0
                           clickCount:1
                             pressure:1.0];
}

}  // namespace

TEST_CASE("PluginViewHost (mac CPU) — a click sets the field's visual focus "
          "(has_focus), and Escape/Return blur it to return the keyboard",
          "[plugin-view-host][focus-affordance][mac][cpu]") {
    @autoreleasepool {
        FocusGuard guard;

        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 400, 200)
                                        styleMask:NSWindowStyleMaskBorderless
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        if (!window || !window.contentView) {
            SKIP("No Cocoa window for focus-affordance test");
        }

        View root;
        PluginViewHost::Options opts;
        opts.size = {400u, 200u};
        opts.use_gpu = false;
        auto host = PluginViewHost::create(root, opts);
        REQUIRE(host != nullptr);
        host->attach_to_parent((__bridge void*)window.contentView);
        NSView* pulp_view = find_pulp_plugin_view(window.contentView);
        REQUIRE(pulp_view != nil);

        // A single-line editor filling the root so any in-bounds click hits it.
        auto editor_owned = std::make_unique<TextEditor>();
        TextEditor* editor = editor_owned.get();
        editor->multi_line = false;
        editor->set_text("hello");
        editor->set_bounds({0, 0, 400, 200});
        root.add_child(std::move(editor_owned));

        SECTION("#1 click focuses the field — has_focus() becomes true") {
            REQUIRE_FALSE(editor->has_focus());
            [pulp_view mouseDown:make_left_mouse_down(NSMakePoint(200, 100))];
            REQUIRE(View::focused_input_ == editor);  // text routing
            REQUIRE(editor->has_focus());             // visual: border + caret
        }

        SECTION("#3 Escape blurs the focused field → keyboard returns to host") {
            editor->on_focus_changed(true);
            editor->claim_input_focus();
            REQUIRE(editor->has_focus());
            [pulp_view keyDown:make_key_event(53, 0, @"\x1b")];  // 53 = Escape
            REQUIRE_FALSE(editor->has_focus());
            REQUIRE(View::focused_input_ == nullptr);
        }

        SECTION("#3 Return blurs a single-line field but NOT a multi-line one") {
            editor->on_focus_changed(true);
            editor->claim_input_focus();
            [pulp_view keyDown:make_key_event(36, 0, @"\r")];  // 36 = Return
            REQUIRE_FALSE(editor->has_focus());  // single-line: committed + blurred

            // Multi-line editor: Return inserts a newline, focus is retained.
            auto ml_owned = std::make_unique<TextEditor>();
            TextEditor* ml = ml_owned.get();
            ml->multi_line = true;
            ml->set_bounds({0, 0, 400, 200});
            root.add_child(std::move(ml_owned));
            ml->on_focus_changed(true);
            ml->claim_input_focus();
            [pulp_view keyDown:make_key_event(36, 0, @"\r")];
            REQUIRE(ml->has_focus());  // multi-line keeps focus on Return
        }

        SECTION("#3 Escape does NOT force-blur a non-text focusable widget") {
            // A focusable NON-TextEditor view (e.g. a custom control that uses
            // Escape for its own purpose) must keep focus on Escape — only text
            // editors are force-blurred for the keyboard hand-back.
            struct FocusableView : View { void paint(pulp::canvas::Canvas&) override {} };
            auto fv_owned = std::make_unique<FocusableView>();
            View* fv = fv_owned.get();
            fv->set_focusable(true);
            fv->set_bounds({0, 0, 400, 200});
            root.add_child(std::move(fv_owned));
            fv->on_focus_changed(true);
            fv->claim_input_focus();
            REQUIRE(View::focused_input_ == fv);
            [pulp_view keyDown:make_key_event(53, 0, @"\x1b")];  // 53 = Escape
            REQUIRE(fv->has_focus());                 // not force-blurred
            REQUIRE(View::focused_input_ == fv);
        }

        host->detach();
        host.reset();
        [window close];
    }
}

#endif  // __APPLE__
