#include <catch2/catch_test_macros.hpp>

#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>

#include <stdexcept>

#if defined(__APPLE__)

#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

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

int g_discard_marked_text_count = 0;
IMP g_original_discard_marked_text = nullptr;

void spy_discard_marked_text(id self, SEL cmd) {
    ++g_discard_marked_text_count;
    if (g_original_discard_marked_text) {
        using Fn = void (*)(id, SEL);
        reinterpret_cast<Fn>(g_original_discard_marked_text)(self, cmd);
    }
}

struct DiscardMarkedTextSpy {
    Method method = nullptr;
    IMP original = nullptr;
    bool active = false;

    DiscardMarkedTextSpy() {
        g_discard_marked_text_count = 0;
        method = class_getInstanceMethod(NSClassFromString(@"NSTextInputContext"),
                                         @selector(discardMarkedText));
        if (!method) return;
        original = method_getImplementation(method);
        g_original_discard_marked_text = original;
        method_setImplementation(method, reinterpret_cast<IMP>(&spy_discard_marked_text));
        active = true;
    }

    ~DiscardMarkedTextSpy() {
        if (active && method && original)
            method_setImplementation(method, original);
        if (g_original_discard_marked_text == original)
            g_original_discard_marked_text = nullptr;
    }

    int count() const { return g_discard_marked_text_count; }
};

class RecordingMouseView : public View {
public:
    int mouse_events = 0;
    int mouse_downs = 0;

    void on_mouse_event(const MouseEvent& event) override {
        (void)event;
        ++mouse_events;
    }

    void on_mouse_down(pulp::view::Point pos) override {
        (void)pos;
        ++mouse_downs;
    }
};

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

TEST_CASE("PluginViewHost (mac CPU) — NSTextInputClient contains throwing text "
          "callbacks",
          "[plugin-view-host][text-input][ime][exceptions][mac][cpu]") {
    @autoreleasepool {
        FocusGuard guard;

        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 400, 200)
                                        styleMask:NSWindowStyleMaskBorderless
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        if (!window || !window.contentView) {
            SUCCEED("No Cocoa window — hosted IME exception-containment test skipped.");
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

        auto editor_owned = std::make_unique<TextEditor>();
        TextEditor* editor = editor_owned.get();
        editor->set_bounds({16, 20, 200, 40});
        editor->set_text("abc");
        root.add_child(std::move(editor_owned));
        editor->on_focus_changed(true);
        editor->claim_input_focus();
        editor->set_caret_pos(3);
        [pulp_view syncKeyFocus];
        REQUIRE(window.firstResponder == pulp_view);

        int throws = 0;
        editor->on_change = [&](const std::string&) {
            ++throws;
            throw std::runtime_error("text callback failed");
        };

        constexpr const char kNi[] = "\xE3\x81\xAB";
        NSString* ni = [NSString stringWithUTF8String:kNi];
        constexpr const char kNichi[] = "\xE6\x97\xA5";
        NSString* nichi = [NSString stringWithUTF8String:kNichi];
        REQUIRE_NOTHROW(([&] {
            [pulp_view setMarkedText:ni
                       selectedRange:NSMakeRange(1, 0)
                    replacementRange:NSMakeRange(NSNotFound, 0)];
        }()));
        REQUIRE(throws == 1);
        REQUIRE(editor->has_marked_text());

        REQUIRE_NOTHROW(([&] {
            [pulp_view insertText:nichi replacementRange:[pulp_view markedRange]];
        }()));
        REQUIRE(throws == 2);
        REQUIRE_FALSE(editor->has_marked_text());

        REQUIRE_NOTHROW(([&] {
            [pulp_view setMarkedText:ni
                       selectedRange:NSMakeRange(1, 0)
                    replacementRange:NSMakeRange(NSNotFound, 0)];
        }()));
        REQUIRE(throws == 3);
        REQUIRE(editor->has_marked_text());

        REQUIRE_NOTHROW(([&] {
            [pulp_view resignFirstResponder];
        }()));
        REQUIRE(throws == 4);
        REQUIRE(View::focused_input_ == nullptr);
        REQUIRE_FALSE(editor->has_focus());
        REQUIRE_FALSE(editor->has_marked_text());

        host->detach();
        host.reset();
        [window close];
    }
}

TEST_CASE("PluginViewHost (mac CPU) — host focus loss cancels active IME marked "
          "text before releasing focus",
          "[plugin-view-host][text-input][ime][focus][mac][cpu]") {
    @autoreleasepool {
        FocusGuard guard;

        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 400, 200)
                                        styleMask:NSWindowStyleMaskBorderless
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        if (!window || !window.contentView) {
            SUCCEED("No Cocoa window — hosted IME focus-loss test skipped.");
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
        NSString* ni = [NSString stringWithUTF8String:kNi];
        [pulp_view setMarkedText:ni
                   selectedRange:NSMakeRange(1, 0)
                replacementRange:NSMakeRange(NSNotFound, 0)];
        REQUIRE(editor->text() == std::string("abc") + kNi);
        REQUIRE(editor->has_marked_text());

        DiscardMarkedTextSpy discard_spy;
        REQUIRE(discard_spy.active);
        [pulp_view resignFirstResponder];

        REQUIRE(discard_spy.count() >= 1);
        REQUIRE(View::focused_input_ == nullptr);
        REQUIRE_FALSE(editor->has_focus());
        REQUIRE_FALSE(editor->has_marked_text());
        REQUIRE(editor->text() == "abc");
        REQUIRE([pulp_view hasMarkedText] == NO);

        host->detach();
        host.reset();
        [window close];
    }
}

TEST_CASE("PluginViewHost (mac CPU) — host focus loss survives IME cancellation "
          "that unmounts the editor",
          "[plugin-view-host][text-input][ime][focus][lifetime][mac][cpu]") {
    @autoreleasepool {
        FocusGuard guard;

        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 400, 200)
                                        styleMask:NSWindowStyleMaskBorderless
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        if (!window || !window.contentView) {
            SUCCEED("No Cocoa window — hosted IME focus-loss teardown test skipped.");
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

        auto editor_owned = std::make_unique<TextEditor>();
        TextEditor* editor = editor_owned.get();
        editor->set_bounds({16, 20, 200, 40});
        editor->set_text("abc");
        root.add_child(std::move(editor_owned));
        editor->on_focus_changed(true);
        editor->claim_input_focus();
        editor->set_caret_pos(3);
        [pulp_view syncKeyFocus];
        REQUIRE(window.firstResponder == pulp_view);

        constexpr const char kNi[] = "\xE3\x81\xAB";
        NSString* ni = [NSString stringWithUTF8String:kNi];
        [pulp_view setMarkedText:ni
                   selectedRange:NSMakeRange(1, 0)
                replacementRange:NSMakeRange(NSNotFound, 0)];
        REQUIRE(editor->has_marked_text());

        bool removed = false;
        editor->on_change = [&](const std::string&) {
            if (removed) return;
            removed = true;
            auto removed_child = root.remove_child(editor);
            removed_child.reset();
        };

        [pulp_view resignFirstResponder];

        REQUIRE(removed);
        REQUIRE(root.child_count() == 0);
        REQUIRE(View::focused_input_ == nullptr);
        REQUIRE([pulp_view hasMarkedText] == NO);

        host->detach();
        host.reset();
        [window close];
    }
}

TEST_CASE("PluginViewHost (mac CPU) — host focus loss clears a replacement "
          "editor focused during IME cancellation",
          "[plugin-view-host][text-input][ime][focus][lifetime][mac][cpu]") {
    @autoreleasepool {
        FocusGuard guard;

        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 400, 200)
                                        styleMask:NSWindowStyleMaskBorderless
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        if (!window || !window.contentView) {
            SUCCEED("No Cocoa window — hosted IME replacement-focus test skipped.");
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

        auto editor_owned = std::make_unique<TextEditor>();
        TextEditor* editor = editor_owned.get();
        editor->set_bounds({16, 20, 200, 40});
        editor->set_text("abc");
        root.add_child(std::move(editor_owned));
        editor->on_focus_changed(true);
        editor->claim_input_focus();
        editor->set_caret_pos(3);
        [pulp_view syncKeyFocus];
        REQUIRE(window.firstResponder == pulp_view);

        constexpr const char kNi[] = "\xE3\x81\xAB";
        NSString* ni = [NSString stringWithUTF8String:kNi];
        [pulp_view setMarkedText:ni
                   selectedRange:NSMakeRange(1, 0)
                replacementRange:NSMakeRange(NSNotFound, 0)];
        REQUIRE(editor->has_marked_text());

        TextEditor* replacement = nullptr;
        editor->on_change = [&](const std::string&) {
            auto replacement_owned = std::make_unique<TextEditor>();
            replacement = replacement_owned.get();
            replacement->set_bounds({16, 20, 200, 40});
            replacement->set_text("replacement");
            root.add_child(std::move(replacement_owned));
            replacement->on_focus_changed(true);
            replacement->claim_input_focus();
            auto removed_child = root.remove_child(editor);
            removed_child.reset();
        };

        [pulp_view resignFirstResponder];

        REQUIRE(replacement != nullptr);
        REQUIRE(View::focused_input_ == nullptr);
        REQUIRE_FALSE(replacement->has_focus());
        REQUIRE([pulp_view hasMarkedText] == NO);

        host->detach();
        host.reset();
        [window close];
    }
}

TEST_CASE("PluginViewHost (mac CPU) — focus transfer cancels old active IME "
          "composition",
          "[plugin-view-host][text-input][ime][focus][mac][cpu]") {
    @autoreleasepool {
        FocusGuard guard;

        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 400, 200)
                                        styleMask:NSWindowStyleMaskBorderless
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        if (!window || !window.contentView) {
            SUCCEED("No Cocoa window — hosted IME focus-transfer test skipped.");
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

        auto first_owned = std::make_unique<TextEditor>();
        TextEditor* first = first_owned.get();
        first->set_bounds({16, 20, 200, 40});
        first->set_text("abc");
        root.add_child(std::move(first_owned));

        auto second_owned = std::make_unique<TextEditor>();
        TextEditor* second = second_owned.get();
        second->set_bounds({16, 90, 200, 40});
        second->set_text("target");
        root.add_child(std::move(second_owned));

        first->on_focus_changed(true);
        first->claim_input_focus();
        first->set_caret_pos(3);
        [pulp_view syncKeyFocus];
        REQUIRE(window.firstResponder == pulp_view);

        constexpr const char kNi[] = "\xE3\x81\xAB";
        NSString* ni = [NSString stringWithUTF8String:kNi];
        [pulp_view setMarkedText:ni
                   selectedRange:NSMakeRange(1, 0)
                replacementRange:NSMakeRange(NSNotFound, 0)];
        REQUIRE(first->text() == std::string("abc") + kNi);
        REQUIRE(first->has_marked_text());

        DiscardMarkedTextSpy discard_spy;
        REQUIRE(discard_spy.active);
        [pulp_view mouseDown:make_left_mouse_down(NSMakePoint(30, 110))];

        REQUIRE(discard_spy.count() >= 1);
        REQUIRE(View::focused_input_ == second);
        REQUIRE_FALSE(first->has_focus());
        REQUIRE_FALSE(first->has_marked_text());
        REQUIRE(first->text() == "abc");
        REQUIRE(second->has_focus());
        REQUIRE([pulp_view hasMarkedText] == NO);

        host->detach();
        host.reset();
        [window close];
    }
}

TEST_CASE("PluginViewHost (mac CPU) — mouse focus changes clear replacement "
          "focus from IME cancellation callbacks",
          "[plugin-view-host][text-input][ime][focus][lifetime][mac][cpu]") {
    @autoreleasepool {
        FocusGuard guard;

        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 400, 200)
                                        styleMask:NSWindowStyleMaskBorderless
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        if (!window || !window.contentView) {
            SUCCEED("No Cocoa window — hosted IME mouse replacement-focus test skipped.");
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

        auto first_owned = std::make_unique<TextEditor>();
        TextEditor* first = first_owned.get();
        first->set_bounds({16, 20, 200, 40});
        first->set_text("abc");
        root.add_child(std::move(first_owned));

        first->on_focus_changed(true);
        first->claim_input_focus();
        first->set_caret_pos(3);
        [pulp_view syncKeyFocus];
        REQUIRE(window.firstResponder == pulp_view);

        constexpr const char kNi[] = "\xE3\x81\xAB";
        NSString* ni = [NSString stringWithUTF8String:kNi];
        [pulp_view setMarkedText:ni
                   selectedRange:NSMakeRange(1, 0)
                replacementRange:NSMakeRange(NSNotFound, 0)];
        REQUIRE(first->has_marked_text());

        TextEditor* replacement = nullptr;
        first->on_change = [&](const std::string&) {
            auto replacement_owned = std::make_unique<TextEditor>();
            replacement = replacement_owned.get();
            replacement->set_bounds({16, 20, 200, 40});
            replacement->set_text("replacement");
            root.add_child(std::move(replacement_owned));
            replacement->on_focus_changed(true);
            replacement->claim_input_focus();
            auto removed_child = root.remove_child(first);
            removed_child.reset();
        };

        SECTION("clicking another focused field clears the replacement before "
                "focusing the clicked field") {
            auto second_owned = std::make_unique<TextEditor>();
            TextEditor* second = second_owned.get();
            second->set_bounds({16, 90, 200, 40});
            second->set_text("target");
            root.add_child(std::move(second_owned));

            [pulp_view mouseDown:make_left_mouse_down(NSMakePoint(30, 110))];

            REQUIRE(replacement != nullptr);
            REQUIRE(View::focused_input_ == second);
            REQUIRE_FALSE(replacement->has_focus());
            REQUIRE(second->has_focus());
        }

        SECTION("clicking a non-focusable view clears the replacement focus") {
            auto target_owned = std::make_unique<View>();
            View* target = target_owned.get();
            target->set_bounds({16, 90, 200, 40});
            root.add_child(std::move(target_owned));

            [pulp_view mouseDown:make_left_mouse_down(NSMakePoint(30, 110))];

            REQUIRE(replacement != nullptr);
            REQUIRE(View::focused_input_ == nullptr);
            REQUIRE_FALSE(replacement->has_focus());
            REQUIRE_FALSE(target->has_focus());
        }

        host->detach();
        host.reset();
        [window close];
    }
}

TEST_CASE("PluginViewHost (mac CPU) — non-focusable click target removed during "
          "IME cancellation is not dispatched",
          "[plugin-view-host][text-input][ime][focus][lifetime][mac][cpu]") {
    @autoreleasepool {
        FocusGuard guard;

        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 400, 200)
                                        styleMask:NSWindowStyleMaskBorderless
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        if (!window || !window.contentView) {
            SUCCEED("No Cocoa window — hosted IME removed-target test skipped.");
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

        auto first_owned = std::make_unique<TextEditor>();
        TextEditor* first = first_owned.get();
        first->set_bounds({16, 20, 200, 40});
        first->set_text("abc");
        root.add_child(std::move(first_owned));

        auto target_owned = std::make_unique<RecordingMouseView>();
        RecordingMouseView* target = target_owned.get();
        target->set_bounds({16, 90, 200, 40});
        root.add_child(std::move(target_owned));

        first->on_focus_changed(true);
        first->claim_input_focus();
        first->set_caret_pos(3);
        [pulp_view syncKeyFocus];
        REQUIRE(window.firstResponder == pulp_view);

        constexpr const char kNi[] = "\xE3\x81\xAB";
        NSString* ni = [NSString stringWithUTF8String:kNi];
        [pulp_view setMarkedText:ni
                   selectedRange:NSMakeRange(1, 0)
                replacementRange:NSMakeRange(NSNotFound, 0)];
        REQUIRE(first->has_marked_text());

        std::unique_ptr<View> removed_target;
        first->on_change = [&](const std::string&) {
            removed_target = root.remove_child(target);
        };

        [pulp_view mouseDown:make_left_mouse_down(NSMakePoint(30, 110))];

        REQUIRE(removed_target != nullptr);
        REQUIRE(target->mouse_events == 0);
        REQUIRE(target->mouse_downs == 0);

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
