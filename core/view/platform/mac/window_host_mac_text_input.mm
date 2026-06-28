// Per-binary-unique ObjC class names (see header).
#include "pulp_mac_objc_names.h"
#include "window_host_mac_view.h"

#include <TargetConditionals.h>
#if TARGET_OS_OSX

#include <pulp/canvas/text_utf8.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/view.hpp>

#include <algorithm>
#include <limits>
#include <string>

extern "C" void pulp_mac_text_input_client_category_anchor() {}

static std::size_t nsrange_location_or_zero(NSRange range) noexcept {
    return range.location == NSNotFound ? 0 : static_cast<std::size_t>(range.location);
}

static std::size_t nsrange_end_or_zero(NSRange range) noexcept {
    if (range.location == NSNotFound) return 0;
    const auto start = static_cast<std::size_t>(range.location);
    const auto length = static_cast<std::size_t>(range.length);
    const auto max = std::numeric_limits<std::size_t>::max();
    return length > max - start ? max : start + length;
}

@interface PulpView (TextInputClient) <NSTextInputClient>
@end

@implementation PulpView (TextInputClient)

- (pulp::view::TextEditor*)focusedTextEditor {
    // pulp #1708 — use the auto-clearing static slot. Without this, the
    // raw focused-view ivar dangles after the focused widget is freed
    // (React unmount of an open modal) and dynamic_cast on freed memory
    // segfaults inside libc++abi during -[NSTextInputContext
    // hasMarkedTextWithCompletionHandler:] on the next keypress.
    auto* fv = pulp::view::View::focused_input_;
    auto* te = dynamic_cast<pulp::view::TextEditor*>(fv);
    return te;
}

- (void)insertText:(id)string replacementRange:(NSRange)range {
    (void)range;
    NSString* in_str = [string isKindOfClass:[NSAttributedString class]]
        ? [(NSAttributedString*)string string] : (NSString*)string;

    // Inspector inline text-tool edits consume character input before it
    // reaches the focused widget, scoped to this host's root view.
    {
        pulp::view::TextInputEvent ite;
        ite.text = [in_str UTF8String];
        if (pulp::view::View::call_inspector_text_hook(ite, self.rootView)) {
            [self setNeedsDisplay:YES];
            return;
        }
    }

    auto* fv = pulp::view::View::focused_input_;
    if (fv) {
        NSString* str = [string isKindOfClass:[NSAttributedString class]]
            ? [(NSAttributedString*)string string] : (NSString*)string;
        pulp::view::TextInputEvent te;
        te.text = [str UTF8String];
        fv->on_text_input(te);
        [self setNeedsDisplay:YES];
    }
}

- (BOOL)hasMarkedText {
    auto* te = [self focusedTextEditor];
    return te ? te->has_marked_text() : NO;
}

- (NSRange)markedRange {
    auto* te = [self focusedTextEditor];
    if (!te || !te->has_marked_text()) return NSMakeRange(NSNotFound, 0);
    auto [start, len] = te->marked_range();
    const auto start16 = pulp::canvas::utf16_offset_for_utf8_offset(
        te->text(), static_cast<std::size_t>(start));
    const auto end16 = pulp::canvas::utf16_offset_for_utf8_offset(
        te->text(), static_cast<std::size_t>(start + len));
    return NSMakeRange(static_cast<NSUInteger>(start16),
                       static_cast<NSUInteger>(end16 - start16));
}

- (NSRange)selectedRange {
    auto* te = [self focusedTextEditor];
    if (!te) return NSMakeRange(0, 0);
    auto [start, end] = te->selection_range();
    const auto start16 = pulp::canvas::utf16_offset_for_utf8_offset(
        te->text(), static_cast<std::size_t>(start));
    const auto end16 = pulp::canvas::utf16_offset_for_utf8_offset(
        te->text(), static_cast<std::size_t>(end));
    return NSMakeRange(static_cast<NSUInteger>(start16),
                       static_cast<NSUInteger>(end16 - start16));
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)sel replacementRange:(NSRange)rep {
    (void)rep;
    auto* te = [self focusedTextEditor];
    if (!te) return;
    NSString* str = [string isKindOfClass:[NSAttributedString class]]
        ? [(NSAttributedString*)string string] : (NSString*)string;
    const char* utf8 = [str UTF8String];
    std::string marked = utf8 ? utf8 : "";
    const auto selected_start16 = nsrange_location_or_zero(sel);
    const auto selected_end16 = nsrange_end_or_zero(sel);
    const auto selected_start8 = pulp::canvas::utf8_offset_for_utf16_offset(marked, selected_start16);
    const auto selected_end8 = pulp::canvas::utf8_offset_for_utf16_offset(marked, selected_end16);
    te->set_marked_text(marked,
                        static_cast<int>(std::min(selected_start8, selected_end8)),
                        static_cast<int>(selected_end8 > selected_start8
                            ? selected_end8 - selected_start8
                            : selected_start8 - selected_end8));
    [self setNeedsDisplay:YES];
}

- (void)unmarkText {
    auto* te = [self focusedTextEditor];
    if (te) te->unmark_text();
}

- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText {
    return @[];
}

- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)r
                                               actualRange:(NSRangePointer)a {
    auto* te = [self focusedTextEditor];
    if (!te) return nil;
    auto& text = te->text();
    if (r.location == NSNotFound) return nil;
    const auto start8 = pulp::canvas::utf8_offset_for_utf16_offset(
        text, nsrange_location_or_zero(r));
    const auto end8 = pulp::canvas::utf8_offset_for_utf16_offset(
        text, nsrange_end_or_zero(r));
    if (start8 >= text.size()) return nil;
    const auto clamped_end8 = std::min(end8, text.size());
    auto sub = text.substr(start8, clamped_end8 - start8);
    if (a) {
        const auto actual_start16 = pulp::canvas::utf16_offset_for_utf8_offset(text, start8);
        const auto actual_end16 = pulp::canvas::utf16_offset_for_utf8_offset(text, clamped_end8);
        *a = NSMakeRange(static_cast<NSUInteger>(actual_start16),
                         static_cast<NSUInteger>(actual_end16 - actual_start16));
    }
    return [[NSAttributedString alloc] initWithString:[NSString stringWithUTF8String:sub.c_str()]];
}

- (NSUInteger)characterIndexForPoint:(NSPoint)p {
    (void)p;
    return 0;
}

- (NSRect)firstRectForCharacterRange:(NSRange)r actualRange:(NSRangePointer)a {
    (void)r; (void)a;
    // IME candidate window positioning. `TextEditor::caret_rect()` is
    // the post-paint, layout-aware caret geometry — single source of
    // truth for both the visible caret and the IME hook so the
    // candidate window aligns with the rendered position even when
    // the text is wrapped or proportional-width.
    auto* te = [self focusedTextEditor];
    if (!te) return NSZeroRect;

    pulp::view::Rect caret = te->caret_rect();
    float local_x = caret.x;
    float local_y = caret.y;
    float caret_w = caret.width > 0.f ? caret.width : 1.0f;
    float caret_h = caret.height > 0.f ? caret.height : te->font_size();

    float rx = local_x, ry = local_y;
    for (auto* v = static_cast<pulp::view::View*>(te); v; v = v->parent()) {
        rx += v->bounds().x;
        ry += v->bounds().y;
    }

    float viewHeight = static_cast<float>(self.bounds.size.height);
    NSRect viewRect = NSMakeRect(rx, viewHeight - ry - caret_h, caret_w, caret_h);
    NSRect windowRect = [self convertRect:viewRect toView:nil];
    return [self.window convertRectToScreen:windowRect];
}

- (void)doCommandBySelector:(SEL)sel {
    (void)sel;
}

@end

#endif
