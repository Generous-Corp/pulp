#include <catch2/catch_test_macros.hpp>

#include <pulp/state/store.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>

#import <Cocoa/Cocoa.h>

@interface PulpScriptKeyTestHost : NSView
@property(nonatomic) NSUInteger keyCount;
@end
@implementation PulpScriptKeyTestHost
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent*)event { ++self.keyCount; }
@end

namespace {
using namespace pulp::view;

NSView* plugin_view(NSView* parent) {
    for (NSView* child in parent.subviews) {
        if ([NSStringFromClass(child.class) isEqualToString:@"PulpPluginView"] ||
            [NSStringFromClass(child.class) isEqualToString:@"PulpGpuPluginView"])
            return child;
        if (NSView* nested = plugin_view(child)) return nested;
    }
    return nil;
}

NSEvent* key(unsigned short code, NSString* text, NSEventModifierFlags modifiers = 0) {
    static NSTimeInterval timestamp = 1;
    return [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
                     modifierFlags:modifiers timestamp:timestamp++ windowNumber:0
                           context:nil characters:text charactersIgnoringModifiers:text
                         isARepeat:NO keyCode:code];
}

struct Fixture {
    ScriptEngine engine;
    View root;
    pulp::state::StateStore state;
    WidgetBridge bridge{engine, root, state};
    NSWindow* window = nil;
    PulpScriptKeyTestHost* daw = nil;
    std::unique_ptr<PluginViewHost> host;
    NSView* editor = nil;

    explicit Fixture(bool gpu) {
        View::focused_input_ = nullptr;
        [NSApplication sharedApplication];
        window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 400, 200)
                                             styleMask:NSWindowStyleMaskBorderless
                                               backing:NSBackingStoreBuffered defer:NO];
        REQUIRE(window != nil);
        daw = [[PulpScriptKeyTestHost alloc] initWithFrame:NSMakeRect(0, 0, 400, 200)];
        [window setContentView:daw];
        PluginViewHost::Options options;
        options.size = {400, 200};
        options.use_gpu = gpu;
        host = PluginViewHost::create(root, options);
        REQUIRE(host != nullptr);
        host->attach_to_parent((__bridge void*)daw);
        editor = plugin_view(daw);
        REQUIRE(editor != nil);
        REQUIRE([NSStringFromClass(editor.class) isEqualToString:
                    gpu ? @"PulpGpuPluginView" : @"PulpPluginView"]);
        bridge.load_script(R"JS(
            var presses = [];
            document.addEventListener('keydown', function(e) {
                presses.push(e.key + (e.metaKey ? '+cmd' : ''));
                if (e.key === 's' || (e.key === 'z' && e.metaKey)) e.preventDefault();
            });
        )JS");
    }
    ~Fixture() {
        View::focused_input_ = nullptr;
        host.reset();
        [window setContentView:nil];
        [daw release];
        [window release];
    }
    int count() { return engine.evaluate("presses.length").getWithDefault<int>(-1); }
};
} // namespace

TEST_CASE("Plugin document shortcuts consume only keys claimed by the owning JavaScript",
          "[plugin-view-host][script-keys][mac]") {
    @autoreleasepool {
        bool gpu = false;
        SECTION("CPU editor") { gpu = false; }
#ifdef PULP_HAS_SKIA
        SECTION("GPU editor") { gpu = true; }
#endif
        Fixture fixture(gpu);
        REQUIRE(fixture.count() == 0);
        // Idle editors do not grab the DAW's first responder to receive shortcuts.
        CHECK_FALSE([fixture.editor acceptsFirstResponder]);
        CHECK([fixture.editor performKeyEquivalent:key(1, @"s")]);
        REQUIRE(fixture.count() == 1);
        CHECK(fixture.engine.evaluate("presses[0]").toString() == "s");
        CHECK([fixture.editor performKeyEquivalent:key(6, @"z", NSEventModifierFlagCommand)]);
        REQUIRE(fixture.count() == 2);
        CHECK(fixture.engine.evaluate("presses[1]").toString() == "z+cmd");
        int settings = 0;
        fixture.root.on_global_key = [&](const KeyEvent& event) {
            if (event.key == static_cast<KeyCode>(',') && (event.modifiers & kModCmd)) {
                ++settings;
                return true;
            }
            return false;
        };
        CHECK([fixture.editor performKeyEquivalent:key(43, @",", NSEventModifierFlagCommand)]);
        CHECK(settings == 1);
        CHECK(fixture.count() == 2);
        CHECK_FALSE([fixture.editor performKeyEquivalent:key(12, @"q", NSEventModifierFlagCommand)]);
        CHECK(fixture.count() == 3);
        CHECK(fixture.daw.keyCount == 0);
    }
}

TEST_CASE("Plugin unconsumed document keys still reach the host exactly once",
          "[plugin-view-host][script-keys][mac][host-forward]") {
    @autoreleasepool {
        Fixture fixture(false);
        NSEvent* space = key(49, @" ");
        CHECK_FALSE([fixture.editor performKeyEquivalent:space]);
        REQUIRE(fixture.count() == 1);
        [fixture.editor keyDown:space];
        CHECK(fixture.count() == 1);
        CHECK(fixture.daw.keyCount == 1);
        // A new physical press with the same key must still be delivered.
        [fixture.editor keyDown:key(49, @" ")];
        CHECK(fixture.count() == 2);
        CHECK(fixture.daw.keyCount == 2);
        [fixture.editor keyDown:key(1, @"s")];
        CHECK(fixture.count() == 3);
        CHECK(fixture.daw.keyCount == 2);
    }
}

TEST_CASE("Plugin document shortcut dispatch cannot reach another editor",
          "[plugin-view-host][script-keys][mac][multi-editor]") {
    @autoreleasepool {
        Fixture first(false);
        Fixture second(false);
        REQUIRE(first.count() == 0);
        REQUIRE(second.count() == 0);
        CHECK([first.editor performKeyEquivalent:key(1, @"s")]);
        CHECK(first.count() == 1);
        CHECK(second.count() == 0);
        CHECK([second.editor performKeyEquivalent:key(1, @"s")]);
        CHECK(first.count() == 1);
        CHECK(second.count() == 1);
    }
}

TEST_CASE("Plugin focused text input wins over document shortcuts",
          "[plugin-view-host][script-keys][mac][text-priority]") {
    @autoreleasepool {
        Fixture fixture(false);
        auto text = std::make_unique<TextEditor>();
        auto* editor = text.get();
        editor->set_text("abc");
        fixture.root.add_child(std::move(text));
        editor->claim_input_focus();
        CHECK([fixture.editor performKeyEquivalent:key(0, @"a", NSEventModifierFlagCommand)]);
        CHECK(editor->has_selection());
        CHECK(fixture.count() == 0);
        CHECK([fixture.editor performKeyEquivalent:key(1, @"s")]);
        CHECK(editor->text() == "s");
        CHECK(fixture.count() == 0);
        editor->release_input_focus();
    }
}
