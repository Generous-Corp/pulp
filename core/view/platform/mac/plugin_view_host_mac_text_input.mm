#include "pulp_mac_objc_names.h"

#include <TargetConditionals.h>
#if TARGET_OS_OSX

#include <pulp/canvas/text_utf8.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/window_host.hpp>

#import <Cocoa/Cocoa.h>

#include <algorithm>
#include <limits>
#include <string>

extern "C" void pulp_mac_plugin_text_input_client_category_anchor() {}

@interface PulpPluginView : NSView
@property (nonatomic, assign) pulp::view::View* rootView;
@property (nonatomic, assign) float designW;
@property (nonatomic, assign) float designH;
@property (nonatomic, assign) BOOL designTopAlign;
@end

#ifdef PULP_HAS_SKIA
@interface PulpGpuPluginView : NSView
@property (nonatomic, assign) pulp::view::View* rootView;
@property (nonatomic, assign) float designW;
@property (nonatomic, assign) float designH;
@property (nonatomic, assign) BOOL designTopAlign;
@end
#endif

namespace {

std::size_t nsrange_location_or_zero(NSRange range) noexcept {
    return range.location == NSNotFound ? 0 : static_cast<std::size_t>(range.location);
}

std::size_t nsrange_end_or_zero(NSRange range) noexcept {
    if (range.location == NSNotFound) return 0;
    const auto start = static_cast<std::size_t>(range.location);
    const auto length = static_cast<std::size_t>(range.length);
    const auto max = std::numeric_limits<std::size_t>::max();
    return length > max - start ? max : start + length;
}

NSString* pulp_plugin_string_from_input(id string) {
    if ([string isKindOfClass:[NSAttributedString class]])
        return [(NSAttributedString*)string string];
    if ([string isKindOfClass:[NSString class]])
        return (NSString*)string;
    return @"";
}

pulp::view::View* pulp_plugin_focus_under_root(pulp::view::View* root) {
    auto* fv = pulp::view::View::focused_input_;
    if (!fv || !root) return nullptr;
    for (pulp::view::View* v = fv; v != nullptr; v = v->parent()) {
        if (v == root) return fv;
    }
    return nullptr;
}

pulp::view::TextEditor* pulp_plugin_focused_text_editor(pulp::view::View* root) {
    return dynamic_cast<pulp::view::TextEditor*>(pulp_plugin_focus_under_root(root));
}

bool pulp_plugin_apply_replacement_range(pulp::view::TextEditor* te,
                                         NSRange replacement_range) {
    if (!te || replacement_range.location == NSNotFound) return false;
    const auto& text = te->text();
    const auto start8 = pulp::canvas::utf8_offset_for_utf16_offset(
        text, nsrange_location_or_zero(replacement_range));
    const auto end8 = pulp::canvas::utf8_offset_for_utf16_offset(
        text, nsrange_end_or_zero(replacement_range));
    if (te->has_marked_text())
        te->unmark_text();
    te->set_selection(static_cast<int>(std::min(start8, end8)),
                      static_cast<int>(std::max(start8, end8)));
    return true;
}

void pulp_plugin_request_text_redraw(NSView* host, pulp::view::View* root) {
    [host setNeedsDisplay:YES];
    if (root) root->request_repaint();
}

void pulp_plugin_insert_text(NSView* host,
                             pulp::view::View* root,
                             id string,
                             NSRange replacement_range) {
    NSString* in_str = pulp_plugin_string_from_input(string);
    pulp::view::TextInputEvent inspector_event;
    inspector_event.text = in_str.UTF8String ? in_str.UTF8String : "";
    if (pulp::view::View::call_inspector_text_hook(inspector_event, root)) {
        pulp_plugin_request_text_redraw(host, root);
        return;
    }

    auto* fv = pulp_plugin_focus_under_root(root);
    if (!fv) return;
    if (auto* te = dynamic_cast<pulp::view::TextEditor*>(fv))
        pulp_plugin_apply_replacement_range(te, replacement_range);
    pulp::view::TextInputEvent event;
    event.text = inspector_event.text;
    fv->on_text_input(event);
    pulp_plugin_request_text_redraw(host, root);
}

BOOL pulp_plugin_has_marked_text(pulp::view::View* root) {
    auto* te = pulp_plugin_focused_text_editor(root);
    return te ? te->has_marked_text() : NO;
}

NSRange pulp_plugin_marked_range(pulp::view::View* root) {
    auto* te = pulp_plugin_focused_text_editor(root);
    if (!te || !te->has_marked_text()) return NSMakeRange(NSNotFound, 0);
    auto [start, len] = te->marked_range();
    const auto start16 = pulp::canvas::utf16_offset_for_utf8_offset(
        te->text(), static_cast<std::size_t>(start));
    const auto end16 = pulp::canvas::utf16_offset_for_utf8_offset(
        te->text(), static_cast<std::size_t>(start + len));
    return NSMakeRange(static_cast<NSUInteger>(start16),
                       static_cast<NSUInteger>(end16 - start16));
}

NSRange pulp_plugin_selected_range(pulp::view::View* root) {
    auto* te = pulp_plugin_focused_text_editor(root);
    if (!te) return NSMakeRange(0, 0);
    auto [start, end] = te->selection_range();
    const auto start16 = pulp::canvas::utf16_offset_for_utf8_offset(
        te->text(), static_cast<std::size_t>(start));
    const auto end16 = pulp::canvas::utf16_offset_for_utf8_offset(
        te->text(), static_cast<std::size_t>(end));
    return NSMakeRange(static_cast<NSUInteger>(start16),
                       static_cast<NSUInteger>(end16 - start16));
}

void pulp_plugin_set_marked_text(NSView* host,
                                 pulp::view::View* root,
                                 id string,
                                 NSRange selected_range,
                                 NSRange replacement_range) {
    auto* te = pulp_plugin_focused_text_editor(root);
    if (!te) return;
    pulp_plugin_apply_replacement_range(te, replacement_range);
    NSString* str = pulp_plugin_string_from_input(string);
    const char* utf8 = str.UTF8String;
    std::string marked = utf8 ? utf8 : "";
    const auto selected_start16 = nsrange_location_or_zero(selected_range);
    const auto selected_end16 = nsrange_end_or_zero(selected_range);
    const auto selected_start8 = pulp::canvas::utf8_offset_for_utf16_offset(
        marked, selected_start16);
    const auto selected_end8 = pulp::canvas::utf8_offset_for_utf16_offset(
        marked, selected_end16);
    te->set_marked_text(marked,
                        static_cast<int>(std::min(selected_start8, selected_end8)),
                        static_cast<int>(selected_start8 < selected_end8
                            ? selected_end8 - selected_start8
                            : selected_start8 - selected_end8));
    pulp_plugin_request_text_redraw(host, root);
}

void pulp_plugin_unmark_text(NSView* host, pulp::view::View* root) {
    auto* te = pulp_plugin_focused_text_editor(root);
    if (!te) return;
    te->unmark_text();
    pulp_plugin_request_text_redraw(host, root);
}

NSAttributedString* pulp_plugin_attributed_substring(pulp::view::View* root,
                                                    NSRange range,
                                                    NSRangePointer actual_range) {
    auto* te = pulp_plugin_focused_text_editor(root);
    if (!te || range.location == NSNotFound) return nil;
    const auto& text = te->text();
    const auto start8 = pulp::canvas::utf8_offset_for_utf16_offset(
        text, nsrange_location_or_zero(range));
    const auto end8 = pulp::canvas::utf8_offset_for_utf16_offset(
        text, nsrange_end_or_zero(range));
    if (start8 >= text.size()) return nil;
    const auto clamped_end8 = std::min(end8, text.size());
    const auto sub = text.substr(start8, clamped_end8 - start8);
    if (actual_range) {
        const auto actual_start16 = pulp::canvas::utf16_offset_for_utf8_offset(text, start8);
        const auto actual_end16 = pulp::canvas::utf16_offset_for_utf8_offset(text, clamped_end8);
        *actual_range = NSMakeRange(static_cast<NSUInteger>(actual_start16),
                                    static_cast<NSUInteger>(actual_end16 - actual_start16));
    }
    return [[NSAttributedString alloc]
        initWithString:[NSString stringWithUTF8String:sub.c_str()]];
}

NSRect pulp_plugin_first_rect_for_character_range(NSView* host,
                                                  pulp::view::View* root,
                                                  float design_w,
                                                  float design_h,
                                                  BOOL design_top_align,
                                                  NSRange range,
                                                  NSRangePointer actual_range) {
    (void)range;
    if (actual_range) *actual_range = pulp_plugin_selected_range(root);
    auto* te = pulp_plugin_focused_text_editor(root);
    if (!te) return NSZeroRect;

    pulp::view::Rect caret = te->caret_rect();
    float root_x = caret.x;
    float root_y = caret.y;
    float caret_w = caret.width > 0.0f ? caret.width : 1.0f;
    float caret_h = caret.height > 0.0f ? caret.height : te->font_size();

    for (auto* v = static_cast<pulp::view::View*>(te); v; v = v->parent()) {
        root_x += v->bounds().x;
        root_y += v->bounds().y;
        if (auto* scroll = dynamic_cast<pulp::view::ScrollView*>(v->parent())) {
            root_x -= scroll->scroll_x();
            root_y -= scroll->scroll_y();
        }
    }

    float host_x = root_x;
    float host_y = root_y;
    float host_w = caret_w;
    float host_h = caret_h;
    float sx = 1.0f, sy = 1.0f, tx = 0.0f, ty = 0.0f;
    if (pulp::view::WindowHost::compute_design_viewport_transform(
            static_cast<float>(host.bounds.size.width),
            static_cast<float>(host.bounds.size.height),
            design_w, design_h, sx, sy, tx, ty, design_top_align)) {
        host_x = root_x * sx + tx;
        host_y = root_y * sy + ty;
        host_w = caret_w * sx;
        host_h = caret_h * sy;
    }

    const float view_height = static_cast<float>(host.bounds.size.height);
    NSRect view_rect = NSMakeRect(host_x, view_height - host_y - host_h, host_w, host_h);
    NSRect window_rect = [host convertRect:view_rect toView:nil];
    return host.window ? [host.window convertRectToScreen:window_rect] : window_rect;
}

}  // namespace

@interface PulpPluginView (TextInputClient) <NSTextInputClient>
@end

@implementation PulpPluginView (TextInputClient)

- (void)insertText:(id)string replacementRange:(NSRange)range {
    pulp_plugin_insert_text(self, self.rootView, string, range);
}

- (BOOL)hasMarkedText {
    return pulp_plugin_has_marked_text(self.rootView);
}

- (NSRange)markedRange {
    return pulp_plugin_marked_range(self.rootView);
}

- (NSRange)selectedRange {
    return pulp_plugin_selected_range(self.rootView);
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)sel replacementRange:(NSRange)rep {
    pulp_plugin_set_marked_text(self, self.rootView, string, sel, rep);
}

- (void)unmarkText {
    pulp_plugin_unmark_text(self, self.rootView);
}

- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText {
    return @[];
}

- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range
                                               actualRange:(NSRangePointer)actualRange {
    return pulp_plugin_attributed_substring(self.rootView, range, actualRange);
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point {
    (void)point;
    return 0;
}

- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    return pulp_plugin_first_rect_for_character_range(
        self, self.rootView, self.designW, self.designH, self.designTopAlign, range, actualRange);
}

- (void)doCommandBySelector:(SEL)selector {
    (void)selector;
}

@end

#ifdef PULP_HAS_SKIA

@interface PulpGpuPluginView (TextInputClient) <NSTextInputClient>
@end

@implementation PulpGpuPluginView (TextInputClient)

- (void)insertText:(id)string replacementRange:(NSRange)range {
    pulp_plugin_insert_text(self, self.rootView, string, range);
}

- (BOOL)hasMarkedText {
    return pulp_plugin_has_marked_text(self.rootView);
}

- (NSRange)markedRange {
    return pulp_plugin_marked_range(self.rootView);
}

- (NSRange)selectedRange {
    return pulp_plugin_selected_range(self.rootView);
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)sel replacementRange:(NSRange)rep {
    pulp_plugin_set_marked_text(self, self.rootView, string, sel, rep);
}

- (void)unmarkText {
    pulp_plugin_unmark_text(self, self.rootView);
}

- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText {
    return @[];
}

- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range
                                               actualRange:(NSRangePointer)actualRange {
    return pulp_plugin_attributed_substring(self.rootView, range, actualRange);
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point {
    (void)point;
    return 0;
}

- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    return pulp_plugin_first_rect_for_character_range(
        self, self.rootView, self.designW, self.designH, self.designTopAlign, range, actualRange);
}

- (void)doCommandBySelector:(SEL)selector {
    (void)selector;
}

@end

#endif  // PULP_HAS_SKIA

#endif  // TARGET_OS_OSX
