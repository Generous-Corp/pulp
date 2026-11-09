// The input half of MetalSurface's view: responder and focus, pointer
// tracking and cursors, keys with the host-first routing rule, text input
// (NSTextInputClient) and file drops. Compiled with ARC.

#include "surface_input_mac.h"

#include <pulp/platform/objc_unique_class.hpp>

#import <objc/runtime.h>

#include <string>
#include <vector>

#if !__has_feature(objc_arc)
#error "surface_input_mac.mm must be compiled with -fobjc-arc"
#endif

namespace pulp::native::detail {

namespace {

template <class Fn> Fn super_fn(SEL sel) {
    return reinterpret_cast<Fn>(pulp::platform::mac::super_implementation(
        (__bridge void*)surface_view_class(), sel_getName(sel)));
}

void super_event(id self, SEL sel, NSEvent* event) {
    if (auto fn = super_fn<void (*)(id, SEL, NSEvent*)>(sel))
        fn(self, sel, event);
}

std::string utf8_of(id string) {
    NSString* s = [string isKindOfClass:[NSAttributedString class]]
                      ? [(NSAttributedString*)string string]
                      : (NSString*)string;
    if (![s isKindOfClass:[NSString class]] || s.length == 0)
        return {};
    const char* c = s.UTF8String;
    return c ? std::string(c) : std::string();
}

void set_cursor_now(Cursor cursor) {
    [(__bridge NSCursor*)ns_cursor_for(cursor) set];
}

// ── Pointer ──────────────────────────────────────────────────────────────────

void imp_pointer(id self, SEL sel, NSEvent* event) {
    auto* input = surface_input_of(self);
    if (!input) {
        super_event(self, sel, event);
        return;
    }
    auto e = translate_pointer((__bridge void*)event, (__bridge void*)self);
    if (!e)
        return;
    if (e->kind == PointerKind::enter) {
        input->pointer_inside = true;
        set_cursor_now(input->cursor);
    } else if (e->kind == PointerKind::leave) {
        input->pointer_inside = false;
        set_cursor_now(Cursor::arrow);
    }
    if (input->handler.pointer) {
        auto callback = input->handler.pointer;
        callback(*e);
    }
}

void imp_scroll_wheel(id self, SEL sel, NSEvent* event) {
    auto* input = surface_input_of(self);
    if (!input || !input->handler.wheel) {
        super_event(self, sel, event);
        return;
    }
    if (auto e = translate_wheel((__bridge void*)event, (__bridge void*)self)) {
        auto callback = input->handler.wheel;
        callback(*e);
    }
}

void imp_cursor_update(id self, SEL sel, NSEvent* event) {
    auto* input = surface_input_of(self);
    if (!input) {
        super_event(self, sel, event);
        return;
    }
    set_cursor_now(input->cursor);
}

void imp_update_tracking_areas(id self, SEL sel) {
    if (auto fn = super_fn<void (*)(id, SEL)>(sel))
        fn(self, sel);
    auto* input = surface_input_of(self);
    if (!input)
        return;
    NSView* view = self;
    if (input->tracking)
        [view removeTrackingArea:input->tracking];
    input->tracking = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved |
                     NSTrackingCursorUpdate | NSTrackingActiveAlways | NSTrackingInVisibleRect
               owner:view
            userInfo:nil];
    [view addTrackingArea:input->tracking];
}

// ── Responder and focus ──────────────────────────────────────────────────────

BOOL imp_accepts_first_responder(id self, SEL) {
    auto* input = surface_input_of(self);
    // Hold the host's keyboard only while the client asked for it.
    return input && input->text_focus;
}

BOOL imp_accepts_first_mouse(id, SEL, NSEvent*) {
    return YES;
}

BOOL imp_become_first_responder(id self, SEL sel) {
    auto fn = super_fn<BOOL (*)(id, SEL)>(sel);
    const BOOL ok = fn ? fn(self, sel) : YES;
    if (ok)
        if (auto* input = surface_input_of(self))
            input->emit_focus(true, FocusReason::responder);
    return ok;
}

BOOL imp_resign_first_responder(id self, SEL sel) {
    auto fn = super_fn<BOOL (*)(id, SEL)>(sel);
    const BOOL ok = fn ? fn(self, sel) : YES;
    if (ok) {
        if (auto* input = surface_input_of(self)) {
            // The host took the keyboard: the hold ends, so the view does not
            // take it back on the next event.
            input->text_focus = false;
            if (!input->marked.empty()) {
                input->marked.clear();
                if (input->handler.marked_text)
                    input->handler.marked_text(std::string(), 0);
            }
            input->emit_focus(false, FocusReason::responder);
        }
    }
    return ok;
}

// ── Keys ─────────────────────────────────────────────────────────────────────

bool offer_key(SurfaceInput& input, const KeyEvent& e) {
    if (!input.handler.key)
        return false;
    auto callback = input.handler.key;
    return callback(e);
}

void imp_key_down(id self, SEL sel, NSEvent* event) {
    auto* input = surface_input_of(self);
    auto e = translate_key((__bridge void*)event);
    if (!input || !e || route_key(*e, input->text_focus) == KeyRoute::host) {
        super_event(self, sel, event);
        return;
    }
    if (offer_key(*input, *e))
        return;
    if (input->text_focus) {
        // Turns the key into committed or marked text through the
        // NSTextInputClient methods below.
        [(NSView*)self interpretKeyEvents:@[ event ]];
        return;
    }
    super_event(self, sel, event);
}

void imp_key_up(id self, SEL sel, NSEvent* event) {
    auto* input = surface_input_of(self);
    auto e = translate_key((__bridge void*)event);
    if (!input || !e || route_key(*e, input->text_focus) == KeyRoute::host ||
        !offer_key(*input, *e))
        super_event(self, sel, event);
}

void imp_flags_changed(id self, SEL sel, NSEvent* event) {
    if (auto* input = surface_input_of(self))
        if (auto e = translate_flags_changed((__bridge void*)event))
            offer_key(*input, *e);
    // Modifier state is the host's as well; always pass it on.
    super_event(self, sel, event);
}

BOOL imp_perform_key_equivalent(id self, SEL sel, NSEvent* event) {
    auto* input = surface_input_of(self);
    auto e = translate_key((__bridge void*)event);
    if (input && e && route_key(*e, input->text_focus) == KeyRoute::client_first &&
        offer_key(*input, *e))
        return YES;
    auto fn = super_fn<BOOL (*)(id, SEL, NSEvent*)>(sel);
    return fn ? fn(self, sel, event) : NO;
}

// ── NSTextInputClient ────────────────────────────────────────────────────────

void imp_insert_text(id self, SEL, id string, NSRange) {
    auto* input = surface_input_of(self);
    if (!input)
        return;
    if (!input->marked.empty()) {
        input->marked.clear();
        if (input->handler.marked_text)
            input->handler.marked_text(std::string(), 0);
    }
    const auto text = utf8_of(string);
    if (!text.empty() && input->handler.text) {
        auto callback = input->handler.text;
        callback(text);
    }
}

void imp_set_marked_text(id self, SEL, id string, NSRange selected, NSRange) {
    auto* input = surface_input_of(self);
    if (!input)
        return;
    input->marked = utf8_of(string);
    if (input->handler.marked_text) {
        auto callback = input->handler.marked_text;
        callback(input->marked, selected.location == NSNotFound ? 0 : selected.location);
    }
}

void imp_unmark_text(id self, SEL) {
    auto* input = surface_input_of(self);
    if (!input || input->marked.empty())
        return;
    input->marked.clear();
    if (input->handler.marked_text)
        input->handler.marked_text(std::string(), 0);
}

BOOL imp_has_marked_text(id self, SEL) {
    auto* input = surface_input_of(self);
    return input && !input->marked.empty();
}

NSRange imp_marked_range(id self, SEL) {
    auto* input = surface_input_of(self);
    if (!input || input->marked.empty())
        return NSMakeRange(NSNotFound, 0);
    return NSMakeRange(0, [NSString stringWithUTF8String:input->marked.c_str()].length);
}

NSRange imp_selected_range(id, SEL) {
    return NSMakeRange(NSNotFound, 0);
}

NSAttributedString* imp_attributed_substring(id, SEL, NSRange, NSRangePointer) {
    return nil;
}

NSArray* imp_valid_attributes(id, SEL) {
    return @[];
}

NSRect imp_first_rect(id self, SEL, NSRange, NSRangePointer) {
    // The candidate window sits at the surface's top-left corner; a client
    // that knows its caret can position it more precisely later.
    NSView* view = self;
    if (!view.window)
        return NSZeroRect;
    NSRect r = [view convertRect:NSMakeRect(0, 0, 1, 20) toView:nil];
    return [view.window convertRectToScreen:r];
}

NSUInteger imp_character_index(id, SEL, NSPoint) {
    return NSNotFound;
}

void imp_do_command(id, SEL, SEL) {
    // A key the client did not consume and that is not text: nothing to do,
    // and no beep.
}

// ── File drops ───────────────────────────────────────────────────────────────

DropEvent drop_event(id self, id<NSDraggingInfo> info, DropPhase phase) {
    DropEvent e;
    e.phase = phase;
    NSView* view = self;
    const NSPoint p = [view convertPoint:[info draggingLocation] fromView:nil];
    e.x = p.x;
    e.y = view.isFlipped ? p.y : view.bounds.size.height - p.y;
    NSArray<NSURL*>* urls =
        [[info draggingPasteboard] readObjectsForClasses:@[ [NSURL class] ]
                                                 options:@{
                                                     NSPasteboardURLReadingFileURLsOnlyKey : @YES
                                                 }];
    for (NSURL* url in urls)
        if (url.path)
            e.paths.emplace_back(url.path.UTF8String);
    return e;
}

NSDragOperation offer_drag(id self, id<NSDraggingInfo> info, DropPhase phase) {
    auto* input = surface_input_of(self);
    if (!input || !input->handler.drop)
        return NSDragOperationNone;
    auto e = drop_event(self, info, phase);
    if (e.paths.empty())
        return NSDragOperationNone;
    auto callback = input->handler.drop;
    input->drop_accepted = callback(e);
    return input->drop_accepted ? NSDragOperationCopy : NSDragOperationNone;
}

NSDragOperation imp_dragging_entered(id self, SEL, id<NSDraggingInfo> info) {
    return offer_drag(self, info, DropPhase::enter);
}

NSDragOperation imp_dragging_updated(id self, SEL, id<NSDraggingInfo> info) {
    return offer_drag(self, info, DropPhase::move);
}

void imp_dragging_exited(id self, SEL, id<NSDraggingInfo> info) {
    auto* input = surface_input_of(self);
    if (!input || !input->handler.drop)
        return;
    input->drop_accepted = false;
    auto e = drop_event(self, info, DropPhase::exit);
    auto callback = input->handler.drop;
    callback(e);
}

BOOL imp_perform_drag(id self, SEL, id<NSDraggingInfo> info) {
    auto* input = surface_input_of(self);
    if (!input || !input->handler.drop || !input->drop_accepted)
        return NO;
    input->drop_accepted = false;
    auto e = drop_event(self, info, DropPhase::drop);
    if (e.paths.empty())
        return NO;
    auto callback = input->handler.drop;
    callback(e);
    return YES;
}

// ── Registration ─────────────────────────────────────────────────────────────

const char* encoding_for(SEL sel) {
    if (Method m = class_getInstanceMethod([NSView class], sel))
        return method_getTypeEncoding(m);
    for (Protocol* protocol :
         { @protocol (NSTextInputClient), @protocol (NSDraggingDestination) }) {
        for (BOOL required : {YES, NO}) {
            auto d = protocol_getMethodDescription(protocol, sel, required, YES);
            if (d.types)
                return d.types;
        }
    }
    return nullptr;
}

void add(Class cls, SEL sel, IMP imp) {
    const char* types = encoding_for(sel);
    if (types)
        class_addMethod(cls, sel, imp, types);
}

} // namespace

void add_input_methods(Class cls) {
    for (SEL sel : {
             @selector(mouseDown:), @selector(mouseUp:), @selector(mouseDragged:),
                 @selector(mouseMoved:), @selector(rightMouseDown:), @selector(rightMouseUp:),
                 @selector(rightMouseDragged:), @selector(otherMouseDown:),
                 @selector(otherMouseUp:), @selector(otherMouseDragged:), @selector(mouseEntered:),
                 @selector(mouseExited:)
         })
        add(cls, sel, (IMP)imp_pointer);
    add(cls, @selector(scrollWheel:), (IMP)imp_scroll_wheel);
    add(cls, @selector(cursorUpdate:), (IMP)imp_cursor_update);
    add(cls, @selector(updateTrackingAreas), (IMP)imp_update_tracking_areas);
    add(cls, @selector(acceptsFirstResponder), (IMP)imp_accepts_first_responder);
    add(cls, @selector(acceptsFirstMouse:), (IMP)imp_accepts_first_mouse);
    add(cls, @selector(becomeFirstResponder), (IMP)imp_become_first_responder);
    add(cls, @selector(resignFirstResponder), (IMP)imp_resign_first_responder);
    add(cls, @selector(keyDown:), (IMP)imp_key_down);
    add(cls, @selector(keyUp:), (IMP)imp_key_up);
    add(cls, @selector(flagsChanged:), (IMP)imp_flags_changed);
    add(cls, @selector(performKeyEquivalent:), (IMP)imp_perform_key_equivalent);

    class_addProtocol(cls, @protocol(NSTextInputClient));
    add(cls, @selector(insertText:replacementRange:), (IMP)imp_insert_text);
    add(cls, @selector(setMarkedText:selectedRange:replacementRange:), (IMP)imp_set_marked_text);
    add(cls, @selector(unmarkText), (IMP)imp_unmark_text);
    add(cls, @selector(hasMarkedText), (IMP)imp_has_marked_text);
    add(cls, @selector(markedRange), (IMP)imp_marked_range);
    add(cls, @selector(selectedRange), (IMP)imp_selected_range);
    add(cls, @selector(attributedSubstringForProposedRange:actualRange:),
        (IMP)imp_attributed_substring);
    add(cls, @selector(validAttributesForMarkedText), (IMP)imp_valid_attributes);
    add(cls, @selector(firstRectForCharacterRange:actualRange:), (IMP)imp_first_rect);
    add(cls, @selector(characterIndexForPoint:), (IMP)imp_character_index);
    add(cls, @selector(doCommandBySelector:), (IMP)imp_do_command);

    add(cls, @selector(draggingEntered:), (IMP)imp_dragging_entered);
    add(cls, @selector(draggingUpdated:), (IMP)imp_dragging_updated);
    add(cls, @selector(draggingExited:), (IMP)imp_dragging_exited);
    add(cls, @selector(performDragOperation:), (IMP)imp_perform_drag);
}

} // namespace pulp::native::detail
