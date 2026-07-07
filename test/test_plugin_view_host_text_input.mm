#include <catch2/catch_test_macros.hpp>

#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>

#if defined(__APPLE__)

#import <Cocoa/Cocoa.h>

using namespace pulp::view;

@interface NSView (PulpTextInputTestHooks)
- (void)syncKeyFocus;
- (void)insertText:(id)string replacementRange:(NSRange)range;
- (void)setMarkedText:(id)string selectedRange:(NSRange)sel replacementRange:(NSRange)rep;
- (void)unmarkText;
- (BOOL)hasMarkedText;
- (NSRange)markedRange;
- (NSRange)selectedRange;
- (void)doCommandBySelector:(SEL)selector;
- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range
                                               actualRange:(NSRangePointer)actualRange;
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange;
@end

namespace {

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

TEST_CASE("PluginViewHost (mac CPU) — NSTextInputClient routes marked text to "
          "the focused TextEditor",
          "[plugin-view-host][text-input][ime][mac][cpu]") {
    @autoreleasepool {
        FocusGuard guard;

        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 400, 200)
                                        styleMask:NSWindowStyleMaskBorderless
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        if (!window || !window.contentView) {
            SUCCEED("No Cocoa window — hosted IME text-input test skipped.");
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
        REQUIRE([pulp_view conformsToProtocol:@protocol(NSTextInputClient)] == YES);

        auto editor_owned = std::make_unique<TextEditor>();
        TextEditor* editor = editor_owned.get();
        editor->set_bounds({16, 20, 200, 40});
        editor->set_text("abc");
        root.add_child(std::move(editor_owned));
        editor->on_focus_changed(true);
        editor->claim_input_focus();
        editor->set_caret_pos(3);
        [pulp_view syncKeyFocus];
        REQUIRE(View::focused_input_ == editor);
        REQUIRE(window.firstResponder == pulp_view);

        constexpr const char kNi[] = "\xE3\x81\xAB";
        constexpr const char kNichi[] = "\xE6\x97\xA5";
        constexpr const char kGo[] = "\xE8\xAA\x9E";
        NSString* ni = [NSString stringWithUTF8String:kNi];
        NSString* nichi = [NSString stringWithUTF8String:kNichi];
        NSString* go = [NSString stringWithUTF8String:kGo];

        [pulp_view setMarkedText:ni
                   selectedRange:NSMakeRange(1, 0)
                replacementRange:NSMakeRange(NSNotFound, 0)];
        REQUIRE(editor->text() == std::string("abc") + kNi);
        REQUIRE(editor->has_marked_text());
        REQUIRE([pulp_view hasMarkedText] == YES);

        NSRange marked = [pulp_view markedRange];
        REQUIRE(marked.location == static_cast<NSUInteger>(3));
        REQUIRE(marked.length == static_cast<NSUInteger>(1));

        NSRange selected = [pulp_view selectedRange];
        REQUIRE(selected.location == static_cast<NSUInteger>(4));
        REQUIRE(selected.length == static_cast<NSUInteger>(0));

        [pulp_view doCommandBySelector:@selector(cancelOperation:)];
        REQUIRE(editor->text() == "abc");
        REQUIRE_FALSE(editor->has_marked_text());
        REQUIRE([pulp_view hasMarkedText] == NO);

        [pulp_view setMarkedText:ni
                   selectedRange:NSMakeRange(1, 0)
                replacementRange:NSMakeRange(NSNotFound, 0)];
        REQUIRE(editor->text() == std::string("abc") + kNi);
        REQUIRE(editor->has_marked_text());
        marked = [pulp_view markedRange];

        NSRange actual = NSMakeRange(NSNotFound, 0);
        NSAttributedString* substring =
            [pulp_view attributedSubstringForProposedRange:NSMakeRange(3, 1)
                                               actualRange:&actual];
        REQUIRE(substring != nil);
        REQUIRE([[substring string] isEqualToString:ni]);
        REQUIRE(actual.location == static_cast<NSUInteger>(3));
        REQUIRE(actual.length == static_cast<NSUInteger>(1));

        editor->password_mode = true;
        actual = NSMakeRange(0, 0);
        NSAttributedString* hidden =
            [pulp_view attributedSubstringForProposedRange:NSMakeRange(0, 3)
                                               actualRange:&actual];
        REQUIRE(hidden == nil);
        REQUIRE(actual.location == NSNotFound);
        REQUIRE(actual.length == static_cast<NSUInteger>(0));

        editor->clipboard_policy = TextEditor::ClipboardPolicy::allow_password_contents;
        actual = NSMakeRange(NSNotFound, 0);
        NSAttributedString* still_hidden =
            [pulp_view attributedSubstringForProposedRange:NSMakeRange(0, 3)
                                               actualRange:&actual];
        REQUIRE(still_hidden == nil);
        REQUIRE(actual.location == NSNotFound);
        REQUIRE(actual.length == static_cast<NSUInteger>(0));
        editor->password_mode = false;
        editor->clipboard_policy = TextEditor::ClipboardPolicy::standard;

        editor->input_filter = [&](std::string_view text) {
            if (text == std::string_view{kNichi, sizeof(kNichi) - 1})
                return std::string{};
            return std::string{text};
        };
        [pulp_view insertText:nichi replacementRange:marked];
        REQUIRE(editor->text() == std::string("abc") + kNi);
        REQUIRE(editor->has_marked_text());
        editor->input_filter = nullptr;

        [pulp_view setMarkedText:@""
                   selectedRange:NSMakeRange(0, 0)
                replacementRange:marked];
        REQUIRE(editor->text() == "abc");
        REQUIRE_FALSE(editor->has_marked_text());

        editor->set_text("abcd");
        editor->set_caret_pos(4);
        [pulp_view setMarkedText:@"X"
                   selectedRange:NSMakeRange(1, 0)
                replacementRange:NSMakeRange(NSNotFound, 0)];
        REQUIRE(editor->text() == "abcdX");
        REQUIRE(editor->has_marked_text());

        [pulp_view setMarkedText:@"Y"
                   selectedRange:NSMakeRange(1, 0)
                replacementRange:NSMakeRange(1, 1)];
        REQUIRE(editor->text() == "aYcdX");
        NSRange retargeted = [pulp_view markedRange];
        REQUIRE(retargeted.location == static_cast<NSUInteger>(1));
        REQUIRE(retargeted.length == static_cast<NSUInteger>(1));
        [pulp_view unmarkText];

        editor->set_text("abc");
        editor->set_caret_pos(3);
        [pulp_view setMarkedText:ni
                   selectedRange:NSMakeRange(1, 0)
                replacementRange:NSMakeRange(NSNotFound, 0)];
        REQUIRE(editor->text() == std::string("abc") + kNi);
        REQUIRE(editor->has_marked_text());

        NSRect first_rect =
            [pulp_view firstRectForCharacterRange:NSMakeRange(0, 0)
                                      actualRange:&actual];
        REQUIRE(first_rect.size.width > 0.0);
        REQUIRE(first_rect.size.height > 0.0);

        [pulp_view insertText:nichi replacementRange:NSMakeRange(NSNotFound, 0)];
        REQUIRE(editor->text() == std::string("abc") + kNichi);
        REQUIRE_FALSE(editor->has_marked_text());
        REQUIRE([pulp_view hasMarkedText] == NO);

        [pulp_view insertText:go replacementRange:NSMakeRange(3, 1)];
        REQUIRE(editor->text() == std::string("abc") + kGo);

        [pulp_view setMarkedText:ni
                   selectedRange:NSMakeRange(0, 1)
                replacementRange:NSMakeRange(3, 1)];
        REQUIRE(editor->text() == std::string("abc") + kNi);
        REQUIRE(editor->has_marked_text());
        [pulp_view unmarkText];
        REQUIRE_FALSE(editor->has_marked_text());
        REQUIRE(editor->text() == std::string("abc") + kNi);

        host->detach();
        host.reset();
        [window close];
    }
}

TEST_CASE("PluginViewHost (mac CPU) — IME candidate rect follows ScrollView "
          "offset",
          "[plugin-view-host][text-input][ime][scroll][mac][cpu]") {
    @autoreleasepool {
        FocusGuard guard;

        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 400, 200)
                                        styleMask:NSWindowStyleMaskBorderless
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        if (!window || !window.contentView) {
            SUCCEED("No Cocoa window — hosted IME scroll rect test skipped.");
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

        auto scroll_owned = std::make_unique<ScrollView>();
        ScrollView* scroll = scroll_owned.get();
        scroll->set_bounds({0, 0, 400, 200});
        scroll->set_content_size({400, 600});

        auto editor_owned = std::make_unique<TextEditor>();
        TextEditor* editor = editor_owned.get();
        editor->set_bounds({20, 160, 200, 40});
        editor->set_text("abc");
        scroll->add_child(std::move(editor_owned));
        root.add_child(std::move(scroll_owned));

        editor->on_focus_changed(true);
        editor->claim_input_focus();
        editor->set_caret_pos(3);
        [pulp_view syncKeyFocus];
        REQUIRE(View::focused_input_ == editor);

        NSRange actual = NSMakeRange(NSNotFound, 0);
        scroll->set_scroll(0, 20);
        NSRect less_scrolled =
            [pulp_view firstRectForCharacterRange:NSMakeRange(0, 0)
                                      actualRange:&actual];

        scroll->set_scroll(0, 80);
        NSRect more_scrolled =
            [pulp_view firstRectForCharacterRange:NSMakeRange(0, 0)
                                      actualRange:&actual];

        REQUIRE(more_scrolled.origin.y > less_scrolled.origin.y + 50.0);
        REQUIRE(more_scrolled.size.height == less_scrolled.size.height);

        host->detach();
        host.reset();
        [window close];
    }
}

#ifdef PULP_HAS_SKIA
TEST_CASE("PluginViewHost (mac GPU) — hosted view adopts NSTextInputClient when "
          "GPU rendering is available",
          "[plugin-view-host][text-input][ime][mac][gpu]") {
    @autoreleasepool {
        FocusGuard guard;

        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 400, 200)
                                        styleMask:NSWindowStyleMaskBorderless
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        if (!window || !window.contentView) {
            SUCCEED("No Cocoa window — hosted GPU IME conformance test skipped.");
            return;
        }

        View root;
        PluginViewHost::Options opts;
        opts.size = {400u, 200u};
        opts.use_gpu = true;
        auto host = PluginViewHost::create(root, opts);
        REQUIRE(host != nullptr);
        if (!host->is_gpu_backed()) {
            SUCCEED("No GPU-backed plugin host in this process — conformance test skipped.");
            [window close];
            return;
        }
        host->attach_to_parent((__bridge void*)window.contentView);

        NSView* pulp_view = find_view_with_class_name(window.contentView, @"PulpGpuPluginView");
        REQUIRE(pulp_view != nil);
        REQUIRE([pulp_view conformsToProtocol:@protocol(NSTextInputClient)] == YES);

        host->detach();
        host.reset();
        [window close];
    }
}
#endif

#endif  // __APPLE__
