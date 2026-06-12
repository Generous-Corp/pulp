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
    for (NSView* sub in parent.subviews)
        if ([NSStringFromClass(sub.class) isEqualToString:@"PulpPluginView"])
            return sub;
    return nil;
}

}  // namespace

TEST_CASE("PluginViewHost (mac CPU) — releasing text-input focus restores the "
          "host's prior first responder, and a host grab ends text input",
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

        SECTION("claim takes the keyboard; release hands it BACK to the host "
                "view, not nil") {
            root.claim_input_focus();
            [pulp_view syncKeyFocus];
            REQUIRE(window.firstResponder == pulp_view);

            root.release_input_focus();
            [pulp_view syncKeyFocus];
            // The pre-claim responder is restored — this is what keeps
            // Logic's Musical Typing alive after a type-in commit.
            REQUIRE(window.firstResponder == host_field);
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
            // type-in there), and the editor no longer wants the keyboard.
            REQUIRE(View::focused_input_ == nullptr);
            REQUIRE(root.lost_count == 1);
            REQUIRE_FALSE([pulp_view acceptsFirstResponder]);
        }

        SECTION("a prior responder that left the window degrades to nil — "
                "no dangling restore") {
            root.claim_input_focus();
            [pulp_view syncKeyFocus];
            REQUIRE(window.firstResponder == pulp_view);

            // The saved prior responder disappears from the window's view
            // tree while the widget holds the keyboard.
            [host_field removeFromSuperview];

            root.release_input_focus();
            [pulp_view syncKeyFocus];
            // Identity validation fails → restore falls back to nil (the
            // window itself). The essential assertions: no crash, and the
            // departed view did NOT become first responder.
            REQUIRE(window.firstResponder != host_field);
            REQUIRE(window.firstResponder != pulp_view);
        }

        host->detach();
        host.reset();
        [window close];
    }
}

#endif  // __APPLE__
