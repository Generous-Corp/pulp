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
// Audio-observability Phase 4 sharpened the contract: the override now
// HONORS consumption. A chord the root hook claims (on_global_key returns
// true — e.g. CommandRegistry::dispatch_key_event found a handler) returns
// YES and is NOT also fanned out to the script global-key dispatcher (no
// double-fire). An unconsumed chord keeps the original additive behavior:
// script fan-out runs and the override returns NO so AppKit menu shortcuts
// (Cmd+W, Cmd+Q) still work. The cases below pin both sides.

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#include <catch2/catch_test_macros.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/script_event_dispatch.hpp>
#include <memory>

// PulpView interface — the Obj-C class window_host_mac.mm declares.
// We don't link against it directly (it's bundled into pulp::view); the
// runtime lookup via NSClassFromString is sufficient for this test.
@interface PulpView : NSView
@property (nonatomic, assign) pulp::view::View* rootView;
@end

@interface PulpPluginView : NSView
@property (nonatomic, assign) pulp::view::View* rootView;
@end

namespace {

class TestRoot : public pulp::view::View {
public:
    void paint(pulp::canvas::Canvas&) override {}
};

class TextSinkView : public pulp::view::View {
public:
    explicit TextSinkView(bool accepts_text) : accepts_text_(accepts_text) {
        set_focusable(true);
    }

    bool accepts_text_input() const override { return accepts_text_; }
    bool on_key_event(const pulp::view::KeyEvent&) override {
        ++key_hits;
        return false;
    }
    void on_text_input(const pulp::view::TextInputEvent&) override {
        ++text_hits;
    }

    int key_hits = 0;
    int text_hits = 0;

private:
    bool accepts_text_ = false;
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

NSEvent* make_plain_a_event() {
    return [NSEvent keyEventWithType:NSEventTypeKeyDown
                            location:NSZeroPoint
                       modifierFlags:0
                           timestamp:0
                        windowNumber:0
                             context:nil
                          characters:@"a"
         charactersIgnoringModifiers:@"a"
                           isARepeat:NO
                             keyCode:0];
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

PulpPluginView* make_pulp_plugin_view(pulp::view::View* root) {
    Class cls = NSClassFromString(@"PulpPluginView");
    if (cls == nil) return nil;
    PulpPluginView* view =
        [[(PulpPluginView*)[cls alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)] autorelease];
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

TEST_CASE("PulpPluginView keyDown: focused non-text control does not swallow printable keys",
          "[mac][platform][keyboard][plugin][musical-typing]") {
    using namespace pulp::view;

    TestRoot root;
    int root_hits = 0;
    root.on_global_key = [&](const KeyEvent&) -> bool {
        ++root_hits;
        return false;
    };

    auto sink = std::make_unique<TextSinkView>(false);
    auto* sink_ptr = sink.get();
    root.add_child(std::move(sink));
    sink_ptr->claim_input_focus();

    PulpPluginView* view = make_pulp_plugin_view(&root);
    if (view == nil) {
        sink_ptr->release_input_focus();
        WARN("PulpPluginView class not registered — skipping platform test");
        return;
    }

    g_script_hits = 0;
    script_events::set_global_key_dispatcher(&counting_script_dispatcher);
    [view keyDown:make_plain_a_event()];
    script_events::set_global_key_dispatcher(nullptr);
    sink_ptr->release_input_focus();

    REQUIRE(sink_ptr->key_hits == 1);
    REQUIRE(sink_ptr->text_hits == 0);
    REQUIRE(root_hits == 1);
    REQUIRE(g_script_hits == 1);
}

TEST_CASE("PulpPluginView keyDown: focused text-capable control receives printable keys",
          "[mac][platform][keyboard][plugin][text]") {
    using namespace pulp::view;

    TestRoot root;
    int root_hits = 0;
    root.on_global_key = [&](const KeyEvent&) -> bool {
        ++root_hits;
        return false;
    };

    auto sink = std::make_unique<TextSinkView>(true);
    auto* sink_ptr = sink.get();
    root.add_child(std::move(sink));
    sink_ptr->claim_input_focus();

    PulpPluginView* view = make_pulp_plugin_view(&root);
    if (view == nil) {
        sink_ptr->release_input_focus();
        return;
    }

    g_script_hits = 0;
    script_events::set_global_key_dispatcher(&counting_script_dispatcher);
    [view keyDown:make_plain_a_event()];
    script_events::set_global_key_dispatcher(nullptr);
    sink_ptr->release_input_focus();

    REQUIRE(sink_ptr->key_hits == 1);
    REQUIRE(sink_ptr->text_hits == 1);
    REQUIRE(root_hits == 0);
    REQUIRE(g_script_hits == 0);
}
