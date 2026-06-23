// pulp #2128 follow-up — pin the macOS performKeyEquivalent: route.
//
// Without the override added in window_host_mac.mm, every Cmd-modified
// shortcut chord (Cmd+,, Cmd+S, Cmd+?) is consumed by NSResponder's
// default first-responder chain (menu items) and never reaches keyDown:
// or the WidgetBridge's shortcut + __dispatch__('__global__','keydown')
// path. The override routes the chord into rootView->on_global_key,
// which the design-tool main installs as a hook into
// WidgetBridge::forward_key_event.
//
// This test:
//  1. Creates a PulpView with a rootView whose on_global_key counts hits.
//  2. Synthesizes a Cmd+, NSEvent and calls -performKeyEquivalent: on
//     the view directly — exactly what AppKit does when the user presses
//     Cmd+, in the live window.
//  3. Asserts the on_global_key callback fired AND received the
//     (key=',', mods=kModCmd) event the lambda forwards into the bridge.
//
// The override now
// HONORS consumption. A chord the root hook claims (on_global_key returns
// true — e.g. CommandRegistry::dispatch_key_event found a handler) returns
// YES and is NOT also fanned out to the script global-key dispatcher (no
// double-fire). An unconsumed chord keeps the original additive behavior:
// script fan-out runs and the override returns NO so AppKit menu shortcuts
// (Cmd+W, Cmd+Q) still work. The cases below pin both sides.

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/clipboard.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/script_event_dispatch.hpp>
#include <pulp/view/text_editor.hpp>

#include <memory>

// PulpView interface — the Obj-C class window_host_mac.mm declares.
// We don't link against it directly (it's bundled into pulp::view); the
// runtime lookup via NSClassFromString is sufficient for this test.
@interface PulpView : NSView
@property (nonatomic, assign) pulp::view::View* rootView;
@end

namespace {

class TestRoot : public pulp::view::View {
public:
    void paint(pulp::canvas::Canvas&) override {}
};

// GlobalKeyDispatcher is a plain function pointer, so the script-dispatch
// spy needs file statics. Each test that installs it resets the slot to
// nullptr afterwards — the pre-test state in this binary (no WidgetBridge
// is ever created here, so nothing else writes the slot).
int g_script_hits = 0;
void counting_script_dispatcher(int, uint16_t, bool) { ++g_script_hits; }

NSEvent* make_cmd_comma_event() {
    // NSEvent.keyCode 43 == comma on US keyboards; characters "," so the
    // bridge sees the right key string if it routes through
    // forward_key_event.
    return [NSEvent keyEventWithType:NSEventTypeKeyDown
                            location:NSZeroPoint
                       modifierFlags:NSEventModifierFlagCommand
                           timestamp:0
                        windowNumber:0
                             context:nil
                          characters:@","
         charactersIgnoringModifiers:@","
                           isARepeat:NO
                             keyCode:43];
}

NSEvent* make_tab_event(NSUInteger modifier_flags = 0) {
    return [NSEvent keyEventWithType:NSEventTypeKeyDown
                            location:NSZeroPoint
                       modifierFlags:modifier_flags
                           timestamp:0
                        windowNumber:0
                             context:nil
                          characters:@"\t"
         charactersIgnoringModifiers:@"\t"
                           isARepeat:NO
                             keyCode:48];
}

NSEvent* make_cmd_shift_option_v_event() {
    return [NSEvent keyEventWithType:NSEventTypeKeyDown
                            location:NSZeroPoint
                       modifierFlags:(NSEventModifierFlagCommand
                                      | NSEventModifierFlagShift
                                      | NSEventModifierFlagOption)
                           timestamp:0
                        windowNumber:0
                             context:nil
                          characters:@"V"
         charactersIgnoringModifiers:@"v"
                           isARepeat:NO
                             keyCode:9];
}

PulpView* make_pulp_view(pulp::view::View* root) {
    // Instantiate PulpView at runtime (class is registered by the static
    // initializer in window_host_mac.mm — pulp::view static linkage pulls
    // it in via the link line). nil when the class isn't registered, in
    // which case callers skip rather than false-fail.
    Class cls = NSClassFromString(@"PulpView");
    if (cls == nil) return nil;
    PulpView* view =
        [[(PulpView*)[cls alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)] autorelease];
    view.rootView = root;
    return view;
}

}  // namespace

TEST_CASE("performKeyEquivalent: routes Cmd-modified chord to rootView->on_global_key",
          "[mac][platform][keyboard][wireup][2128]") {
    using namespace pulp::view;

    // 1. Build the same arrangement design-tool's main builds.
    TestRoot root;
    int hits = 0;
    KeyCode last_key = KeyCode::unknown;
    uint16_t last_mods = 0;
    bool last_is_down = false;
    root.on_global_key = [&](const KeyEvent& e) -> bool {
        ++hits;
        last_key = e.key;
        last_mods = e.modifiers;
        last_is_down = e.is_down;
        return false;
    };

    PulpView* view = make_pulp_view(&root);
    if (view == nil) {
        WARN("PulpView class not registered — skipping platform test "
             "(expected when pulp::view isn't linked in)");
        return;
    }

    // The whole point: AppKit calls -performKeyEquivalent: for any chord
    // with Cmd held. Pre-fix, PulpView had no override and the chord was
    // consumed by the responder chain. Post-fix, the override routes it
    // into rootView->on_global_key. Spy on the script fan-out too: an
    // UNCONSUMED chord must keep the additive behavior — script dispatch
    // fires AND the override returns NO so menu shortcuts still work.
    g_script_hits = 0;
    script_events::set_global_key_dispatcher(&counting_script_dispatcher);
    BOOL handled = [view performKeyEquivalent:make_cmd_comma_event()];
    script_events::set_global_key_dispatcher(nullptr);

    REQUIRE(handled == NO);
    REQUIRE(hits == 1);
    REQUIRE(g_script_hits == 1);
    REQUIRE(last_is_down == true);
    REQUIRE((last_mods & kModCmd) != 0);
    // Comma's W3C key form. The bridge translates keyCode==',' (ASCII 44)
    // by the same path, but here we only assert the rootView callback
    // received the chord — the forward_key_event → JS dispatch chain is
    // covered by test_platform_key_wireup.cpp.
}

TEST_CASE("performKeyEquivalent: consumed chord returns YES and does not double-fire",
          "[mac][platform][keyboard][wireup][commands]") {
    using namespace pulp::view;

    // The shell-owned CommandRegistry path installs on_global_key via
    // route_global_keys(); a claimed chord returns true. Model that with
    // a consuming lambda — the contract under test is the override's
    // response to consumption, not the registry lookup itself (pinned by
    // test_command_registry.cpp).
    TestRoot root;
    int hits = 0;
    root.on_global_key = [&](const KeyEvent&) -> bool {
        ++hits;
        return true;
    };

    PulpView* view = make_pulp_view(&root);
    if (view == nil) return;

    g_script_hits = 0;
    script_events::set_global_key_dispatcher(&counting_script_dispatcher);
    BOOL handled = [view performKeyEquivalent:make_cmd_comma_event()];
    script_events::set_global_key_dispatcher(nullptr);

    // Consumed: YES stops AppKit's menu fallthrough; the script fan-out
    // is skipped so the same chord can't fire a JS 'keydown' too.
    REQUIRE(handled == YES);
    REQUIRE(hits == 1);
    REQUIRE(g_script_hits == 0);
}

TEST_CASE("performKeyEquivalent: no-op when rootView->on_global_key is null",
          "[mac][platform][keyboard][wireup][2128]") {
    // Defensive: the override must early-return when no hook is
    // installed (e.g., embedding contexts that drive their own key
    // routing). Otherwise we'd crash on null deref.
    using namespace pulp::view;

    TestRoot root;
    // root.on_global_key intentionally left null.

    PulpView* view = make_pulp_view(&root);
    if (view == nil) return;

    // Should not crash; should return NO so AppKit's default chain runs,
    // and the additive script fan-out must still happen.
    g_script_hits = 0;
    script_events::set_global_key_dispatcher(&counting_script_dispatcher);
    BOOL handled = [view performKeyEquivalent:make_cmd_comma_event()];
    script_events::set_global_key_dispatcher(nullptr);
    REQUIRE(handled == NO);
    REQUIRE(g_script_hits == 1);
}

TEST_CASE("PulpView advertises NSTextInputClient protocol conformance",
          "[mac][platform][keyboard][text_input]") {
    using namespace pulp::view;

    TestRoot root;
    PulpView* view = make_pulp_view(&root);
    if (view == nil) return;

    REQUIRE([view conformsToProtocol:@protocol(NSTextInputClient)] == YES);
}

TEST_CASE("performKeyEquivalent: focused TextEditor handles paste-and-match-style before globals",
          "[mac][platform][keyboard][text_editor][clipboard]") {
    using namespace pulp::view;

    TestRoot root;
    root.set_bounds({0, 0, 320, 120});
    int global_hits = 0;
    root.on_global_key = [&](const KeyEvent&) {
        ++global_hits;
        return false;
    };

    auto editor_owned = std::make_unique<TextEditor>();
    auto* editor = editor_owned.get();
    editor->set_bounds({0, 0, 160, 32});
    editor->set_text("a");
    editor->set_caret_pos(static_cast<int>(editor->text().size()));
    root.add_child(std::move(editor_owned));

    editor->on_focus_changed(true);
    editor->claim_input_focus();
    pulp::platform::Clipboard::set_text("b");

    PulpView* view = make_pulp_view(&root);
    if (view == nil) return;

    g_script_hits = 0;
    script_events::set_global_key_dispatcher(&counting_script_dispatcher);
    BOOL handled = [view performKeyEquivalent:make_cmd_shift_option_v_event()];
    script_events::set_global_key_dispatcher(nullptr);

    REQUIRE(handled == YES);
    REQUIRE(editor->text() == "ab");
    REQUIRE(global_hits == 0);
    REQUIRE(g_script_hits == 0);
}

TEST_CASE("PulpView keyDown offers Tab to focused TextEditor before focus traversal",
          "[mac][platform][keyboard][text_editor][tab]") {
    using namespace pulp::view;

    TestRoot root;
    root.set_bounds({0, 0, 320, 120});

    auto editor_owned = std::make_unique<TextEditor>();
    auto* editor = editor_owned.get();
    editor->set_bounds({0, 0, 160, 32});
    editor->set_text("a");
    editor->set_caret_pos(static_cast<int>(editor->text().size()));
    editor->tab_behavior = TextEditor::TabBehavior::insert_tab;
    root.add_child(std::move(editor_owned));

    editor->on_focus_changed(true);
    editor->claim_input_focus();

    PulpView* view = make_pulp_view(&root);
    if (view == nil) return;

    [view keyDown:make_tab_event()];
    REQUIRE(editor->text() == "a\t");
    REQUIRE(editor->has_focus());
}

TEST_CASE("PulpView keyDown still traverses focus when TextEditor leaves Tab unhandled",
          "[mac][platform][keyboard][text_editor][tab]") {
    using namespace pulp::view;

    TestRoot root;
    root.set_bounds({0, 0, 320, 120});

    auto first_owned = std::make_unique<TextEditor>();
    auto* first = first_owned.get();
    first->set_bounds({0, 0, 120, 32});
    first->set_text("first");

    auto second_owned = std::make_unique<TextEditor>();
    auto* second = second_owned.get();
    second->set_bounds({0, 40, 120, 32});
    second->set_text("second");

    root.add_child(std::move(first_owned));
    root.add_child(std::move(second_owned));

    first->on_focus_changed(true);
    first->claim_input_focus();

    PulpView* view = make_pulp_view(&root);
    if (view == nil) return;

    [view keyDown:make_tab_event()];
    REQUIRE_FALSE(first->has_focus());
    REQUIRE(second->has_focus());
    REQUIRE(pulp::view::View::focused_input_ == second);
}
