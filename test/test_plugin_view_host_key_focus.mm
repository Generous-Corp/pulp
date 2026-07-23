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
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>

#if defined(__APPLE__)
#include <atomic>
#include <new>
#include <stdexcept>

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
    // The editor grabs the DAW keyboard ONLY for a focused TEXT field. Defaults
    // true so the existing contract tests model a type-in; flip to false to model
    // a focused NON-text widget (a ComboBox dropdown, a focusable container) that
    // must NOT steal the host keyboard.
    bool accepts_text_input() const override { return text_input; }
    bool text_input = true;
    int lost_count = 0;
};

class FocusRecordingTextEditor : public TextEditor {
public:
    void on_focus_changed(bool gained) override {
        TextEditor::on_focus_changed(gained);
        if (!gained) ++lost_count;
    }
    int lost_count = 0;
};

class BlurMutatingTextEditor : public TextEditor {
public:
    View* root = nullptr;
    View* remove_on_blur = nullptr;
    std::unique_ptr<View> removed;
    bool throw_on_blur = false;

    void on_focus_changed(bool gained) override {
        TextEditor::on_focus_changed(gained);
        if (!gained && root && remove_on_blur && !removed)
            removed = root->remove_child(remove_on_blur);
        if (!gained && throw_on_blur)
            throw std::runtime_error("blur callback failed");
    }
};

class ReusedAddressTextEditor : public FocusRecordingTextEditor {
public:
    static void* operator new(std::size_t size);
    static void operator delete(void* ptr) noexcept;
    static std::function<void()>& key_event_hook() {
        static std::function<void()> hook;
        return hook;
    }

    bool on_key_event(const KeyEvent& e) override {
        auto& hook = key_event_hook();
        if (hook && e.is_down && e.key == KeyCode::enter) {
            hook();
            return true;
        }
        return FocusRecordingTextEditor::on_key_event(e);
    }
};

alignas(ReusedAddressTextEditor) unsigned char
    reused_text_editor_storage[sizeof(ReusedAddressTextEditor)];
bool reused_text_editor_in_use = false;

void* ReusedAddressTextEditor::operator new(std::size_t size) {
    if (reused_text_editor_in_use || size > sizeof(reused_text_editor_storage))
        throw std::bad_alloc();
    reused_text_editor_in_use = true;
    return reused_text_editor_storage;
}

void ReusedAddressTextEditor::operator delete(void* ptr) noexcept {
    if (ptr == reused_text_editor_storage)
        reused_text_editor_in_use = false;
}

struct FocusGuard {
    FocusGuard() { View::focused_input_ = nullptr; }
    ~FocusGuard() { View::focused_input_ = nullptr; }
};

NSView* find_view_with_class_name(NSView* parent, NSString* class_name) {
    for (NSView* sub in parent.subviews) {
        if ([NSStringFromClass(sub.class) isEqualToString:class_name])
            return sub;
        NSView* nested = find_view_with_class_name(sub, class_name);
        if (nested) return nested;
    }
    return nil;
}

NSView* find_pulp_plugin_view(NSView* parent) {
    return find_view_with_class_name(parent, @"PulpPluginView");
}

}  // namespace

TEST_CASE("PluginViewHost (mac CPU) — the editor takes the keyboard only while a "
          "field is focused; a host grab still ends text input",
          "[plugin-view-host][key-focus][mac][cpu]") {
    @autoreleasepool {
        FocusGuard guard;

        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 800, 600)
                                        styleMask:NSWindowStyleMaskBorderless
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        if (!window || !window.contentView) {
            SUCCEED("No Cocoa window — key-focus contract test skipped.");
            return;
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

        SECTION("claim takes the keyboard; release HANDS IT BACK to the host so DAW "
                "transport / Musical Typing resume") {
            root.claim_input_focus();
            [pulp_view syncKeyFocus];
            REQUIRE(window.firstResponder == pulp_view);
            REQUIRE([pulp_view acceptsFirstResponder]);  // a field is focused

            root.release_input_focus();
            [pulp_view syncKeyFocus];
            // Contract: with no field focused the editor restores the responder
            // that routed the DAW keyboard before the type-in. `nil` is not enough:
            // Logic's Musical Typing remains silent until its responder is restored.
            REQUIRE(window.firstResponder == host_field);
            REQUIRE_FALSE([pulp_view acceptsFirstResponder]);
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
            // type-in there). With no field focused the view no longer accepts
            // first responder, so the DAW keeps the keyboard.
            REQUIRE(View::focused_input_ == nullptr);
            REQUIRE(root.lost_count == 1);
            REQUIRE_FALSE([pulp_view acceptsFirstResponder]);  // no field focused
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

// The event-independent focus-sync contract. syncKeyFocus is invoked on every
// discrete key/mouse event AND — the fix this pins — on the host's frame tick
// (the CVDisplayLink main-thread block in MacPluginViewHost / MacGpuPluginViewHost).
// That extra cadence matters when the focus slot clears WITHOUT a following
// event: a generation that FAILS (the JS pump releases the prompt field but no
// key/mouse event follows) or any programmatic blur. Without a tick-driven sync
// the editor stays first responder after such a clear and swallows the DAW's
// Musical Typing until close/reopen. Here we clear the slot PROGRAMMATICALLY (no
// NSEvent) and prove the actual CPU host frame tick resigns first responder and
// hands the keyboard back. A no-op idle callback deliberately holds the
// display-link gate open; pumping the main run loop lets its queued tick run.
// This makes the regression test discriminate the call-site wiring: deleting
// the frame-tick sync leaves the editor stuck and fails the assertion below.
// The GPU host mirrors the identical override and call site.
TEST_CASE("PluginViewHost (mac CPU) — a programmatic focus clear (no event) "
          "hands the keyboard back on the next syncKeyFocus tick",
          "[plugin-view-host][key-focus][mac][cpu][frame-tick]") {
    @autoreleasepool {
        FocusGuard guard;

        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 400, 200)
                                        styleMask:NSWindowStyleMaskBorderless
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        if (!window || !window.contentView) {
            SUCCEED("No Cocoa window — frame-tick focus-sync test skipped.");
            return;
        }

        FocusRecordingView root;
        PluginViewHost::Options opts;
        opts.size = {400u, 200u};
        opts.use_gpu = false;
        auto host = PluginViewHost::create(root, opts);
        REQUIRE(host != nullptr);
        host->attach_to_parent((__bridge void*)window.contentView);
        std::atomic<int> frame_ticks{0};
        host->set_idle_callback([&] {
            frame_ticks.fetch_add(1, std::memory_order_relaxed);
        });

        NSView* pulp_view = find_pulp_plugin_view(window.contentView);
        REQUIRE(pulp_view != nil);

        PulpTestHostField* host_field =
            [[PulpTestHostField alloc] initWithFrame:NSMakeRect(0, 0, 10, 10)];
        [window.contentView addSubview:host_field];
        REQUIRE([window makeFirstResponder:host_field]);

        // A text field is focused and the editor holds first responder — the
        // mid-type state right before a doomed generation resolves.
        root.claim_input_focus();
        [pulp_view syncKeyFocus];
        REQUIRE(window.firstResponder == pulp_view);  // positive control: keyboard held
        REQUIRE([pulp_view acceptsFirstResponder]);

        // The generation FAILS: the slot is released programmatically, with NO
        // key/mouse event delivered. First responder is UNCHANGED at this instant
        // (the editor still holds it) — exactly the stuck state the old,
        // event-only sync left behind.
        root.release_input_focus();
        REQUIRE(View::focused_input_ == nullptr);
        REQUIRE(window.firstResponder == pulp_view);   // still stuck pre-tick

        // Let the display-link callback queue and execute a main-thread frame
        // tick. No key/mouse event and no direct syncKeyFocus call occurs here.
        const int ticks_before_release =
            frame_ticks.load(std::memory_order_relaxed);
        NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:1.0];
        while (window.firstResponder == pulp_view &&
               deadline.timeIntervalSinceNow > 0.0) {
            [[NSRunLoop mainRunLoop]
                runMode:NSDefaultRunLoopMode
                beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        }
        if (frame_ticks.load(std::memory_order_relaxed) == ticks_before_release) {
            SUCCEED("No active CVDisplayLink — frame-tick focus-sync test skipped.");
            host->detach();
            host.reset();
            [window close];
            return;
        }
        REQUIRE(window.firstResponder != pulp_view);
        REQUIRE_FALSE([pulp_view acceptsFirstResponder]);

        host->detach();
        host.reset();
        [window close];
    }
}

// A focused NON-text widget (a ComboBox dropdown, a focusable container/root)
// must NOT make the editor grab the DAW keyboard — only a real text type-in
// does. Regression: clicking the sampler's Direction/Loop dropdown focused a
// focusable ComboBox, which left the editor first responder and swallowed every
// later key, so Logic transport (Space/R) AND Musical Typing (QWERTY → notes)
// died until the user clicked away. Pins acceptsFirstResponder/syncKeyFocus to
// `accepts_text_input()`, not mere focusability.
TEST_CASE("PluginViewHost (mac CPU) — a focused non-text widget does NOT steal "
          "the DAW keyboard",
          "[plugin-view-host][key-focus][mac][cpu][non-text-focus]") {
    @autoreleasepool {
        FocusGuard guard;
        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 800, 600)
                                        styleMask:NSWindowStyleMaskBorderless
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        if (!window || !window.contentView) {
            SUCCEED("No Cocoa window — non-text-focus contract test skipped.");
            return;
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

        PulpTestHostField* host_field =
            [[PulpTestHostField alloc] initWithFrame:NSMakeRect(0, 0, 10, 10)];
        [window.contentView addSubview:host_field];
        REQUIRE([window makeFirstResponder:host_field]);

        // Model clicking a focusable NON-text widget (e.g. the Direction dropdown):
        // it claims the focus slot, but accepts_text_input() is false.
        root.text_input = false;
        root.claim_input_focus();
        [pulp_view syncKeyFocus];
        // The editor must NOT take the keyboard — the host keeps it (transport keys
        // + Musical Typing keep working).
        REQUIRE_FALSE([pulp_view acceptsFirstResponder]);
        REQUIRE(window.firstResponder == host_field);

        // Flipping the same focused widget to a text field DOES grab — proving the
        // gate is the text-input capability, not the focus slot alone.
        root.text_input = true;
        [pulp_view syncKeyFocus];
        REQUIRE([pulp_view acceptsFirstResponder]);
        REQUIRE(window.firstResponder == pulp_view);

        root.release_input_focus();
        [pulp_view syncKeyFocus];
        REQUIRE(window.firstResponder == host_field);
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
            SUCCEED("No Cocoa window — multi-editor focus test skipped.");
            return;
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

        // An editor accepts first responder ONLY when its OWN tree holds the focused
        // text-input slot. focused_input_ is under editor A, so only A accepts it;
        // editor B (no focused field) leaves the keyboard with the DAW.
        REQUIRE([viewA acceptsFirstResponder]);
        REQUIRE_FALSE([viewB acceptsFirstResponder]);

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
constexpr unichar kNSF1 = 0xF704;

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
            SUCCEED("No Cocoa window — hosted key-routing test skipped.");
            return;
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
        auto editor_owned = std::make_unique<ReusedAddressTextEditor>();
        ReusedAddressTextEditor* editor = editor_owned.get();
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

        SECTION("Tab moves focus without inserting a tab character") {
            editor->set_caret_pos(3);
            [pulp_view keyDown:make_key_event(48, 0, @"\t")];  // 48 = Tab
            REQUIRE(editor->text() == "abc");
            REQUIRE(View::focused_input_ == nullptr);
            REQUIRE(editor->lost_count == 1);
        }

        SECTION("unhandled function key inserts no private-use character") {
            editor->set_caret_pos(3);
            [pulp_view keyDown:make_key_event(
                122, 0,
                [NSString stringWithCharacters:&kNSF1 length:1])];
            REQUIRE(editor->text() == "abc");
            REQUIRE(editor->caret_pos() == 3);
        }

        SECTION("printable key revalidates focus after synchronous text teardown") {
            bool removed = false;
            editor->on_change = [&](const std::string&) {
                if (removed) return;
                removed = true;
                auto removed_child = root.remove_child(editor);
                removed_child.reset();
            };
            editor->set_caret_pos(3);
            [pulp_view keyDown:make_key_event(7, 0, @"x")];  // keyCode 7 = 'x'
            REQUIRE(removed);
            REQUIRE(root.child_count() == 0);
            REQUIRE(View::focused_input_ == nullptr);
        }

        SECTION("Return revalidates focus after synchronous command teardown") {
            std::unique_ptr<View> detached;
            editor->on_return = [&](const std::string&) {
                View::focused_input_ = nullptr;
                detached = root.remove_child(editor);
            };
            [pulp_view keyDown:make_key_event(36, 0, @"\r")];  // 36 = Return
            REQUIRE(detached != nullptr);
            REQUIRE(editor->lost_count == 0);
            REQUIRE(View::focused_input_ == nullptr);
        }

        SECTION("Return callback can transfer focus without blurring replacement") {
            auto next_owned = std::make_unique<FocusRecordingTextEditor>();
            FocusRecordingTextEditor* next = next_owned.get();
            root.add_child(std::move(next_owned));
            editor->on_return = [&](const std::string&) {
                editor->on_focus_changed(false);
                editor->release_input_focus();
                next->on_focus_changed(true);
                next->claim_input_focus();
            };
            [pulp_view keyDown:make_key_event(36, 0, @"\r")];  // 36 = Return
            REQUIRE(View::focused_input_ == next);
            REQUIRE(next->lost_count == 0);
            REQUIRE(next->has_focus());
        }

        SECTION("Return teardown plus same-address replacement keeps replacement focused") {
            ReusedAddressTextEditor* replacement = nullptr;
            bool same_address = false;
            ReusedAddressTextEditor::key_event_hook() = [&] {
                auto removed = root.remove_child(editor);
                removed.reset();
                auto replacement_owned = std::make_unique<ReusedAddressTextEditor>();
                replacement = replacement_owned.get();
                same_address = replacement == editor;
                root.add_child(std::move(replacement_owned));
                replacement->on_focus_changed(true);
                replacement->claim_input_focus();
            };
            [pulp_view keyDown:make_key_event(36, 0, @"\r")];  // 36 = Return
            ReusedAddressTextEditor::key_event_hook() = {};
            REQUIRE(same_address);
            REQUIRE(View::focused_input_ == replacement);
            REQUIRE(replacement->lost_count == 0);
            REQUIRE(replacement->has_focus());
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

// A View whose on_key_event records every key it is asked to handle — a stand-in
// for an editor that might wire musical typing on on_key_event.
namespace {
struct NoteCaptureRoot : public pulp::view::View {
    std::vector<pulp::view::KeyCode> seen;
    bool on_key_event(const pulp::view::KeyEvent& e) override {
        if (e.is_down) seen.push_back(e.key);
        return e.is_down && e.key == pulp::view::KeyCode::a;
    }
};
}  // namespace

// Plugin contract: with NO focused input widget, a keystroke must NOT reach the
// editor root's on_key_event — the editor isn't first responder, so the DAW keeps
// the keyboard (transport keys + Musical Typing). A plugin must never route the
// bare computer keyboard into its own handling. (The standalone drives QWERTY
// musical typing through a different window host.)
TEST_CASE("PluginViewHost (mac CPU) — unfocused keys stay with the DAW, not the "
          "editor root",
          "[plugin-view-host][key-routing][mac][cpu]") {
    @autoreleasepool {
        FocusGuard guard;
        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 400, 200)
                                        styleMask:NSWindowStyleMaskBorderless
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        if (!window || !window.contentView) {
            SUCCEED("No Cocoa window — hosted root-key test skipped.");
            return;
        }
        NoteCaptureRoot root;
        PluginViewHost::Options opts;
        opts.size = {400u, 200u};
        opts.use_gpu = false;
        auto host = PluginViewHost::create(root, opts);
        REQUIRE(host != nullptr);
        host->attach_to_parent((__bridge void*)window.contentView);
        NSView* pulp_view = find_pulp_plugin_view(window.contentView);
        REQUIRE(pulp_view != nil);
        REQUIRE(View::focused_input_ == nullptr);          // nothing focused
        REQUIRE_FALSE([pulp_view acceptsFirstResponder]);  // ...so we don't take keys

        // Even if a key is delivered, with nothing focused pulp_plugin_key_down must
        // NOT dispatch it to the editor root — it returns it to the host.
        [pulp_view keyDown:make_key_event(0, 0, @"a")];
        REQUIRE(root.seen.empty());

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
            SUCCEED("No Cocoa window — focus-affordance test skipped.");
            return;
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
        auto editor_owned = std::make_unique<BlurMutatingTextEditor>();
        BlurMutatingTextEditor* editor = editor_owned.get();
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

        SECTION("#3 Escape clears focus before a throwing blur callback") {
            editor->on_focus_changed(true);
            editor->claim_input_focus();
            editor->throw_on_blur = true;
            [pulp_view keyDown:make_key_event(53, 0, @"\x1b")];  // 53 = Escape
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

        SECTION("#3 click focus transfer revalidates target after blur callback") {
            editor->set_bounds({0, 0, 180, 200});
            auto target_owned = std::make_unique<FocusRecordingTextEditor>();
            FocusRecordingTextEditor* target = target_owned.get();
            target->set_bounds({200, 0, 200, 200});
            root.add_child(std::move(target_owned));
            editor->root = &root;
            editor->remove_on_blur = target;
            editor->on_focus_changed(true);
            editor->claim_input_focus();
            [pulp_view mouseDown:make_left_mouse_down(NSMakePoint(300, 100))];
            REQUIRE(editor->removed != nullptr);
            REQUIRE_FALSE(target->has_focus());
            REQUIRE(View::focused_input_ == nullptr);
        }

        host->detach();
        host.reset();
        [window close];
    }
}

#endif  // __APPLE__
