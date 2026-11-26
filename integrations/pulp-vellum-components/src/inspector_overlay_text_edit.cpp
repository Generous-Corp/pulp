#include <pulp/vellum_components/inspector_overlay.hpp>

#include <pulp/vellum_components/overlay_palette.hpp>

#include <vellum/view/input_events.hpp>
#include <vellum/view/text_editor.hpp>
#include <vellum/view/widgets.hpp>

#include <algorithm>
#include <utility>

namespace pulp::vellum_components {

using ::vellum::view::KeyCode;
using ::vellum::view::KeyEvent;
using ::vellum::view::TextInputEvent;

void InspectorOverlay::set_tool(Tool tool) {
    if (tool_ == tool)
        return;
    // Switching away with an edit open commits it, the way a click away does.
    if (tool_ == Tool::text && tool != Tool::text && text_editing())
        commit_text_edit();
    tool_ = tool;
}

void InspectorOverlay::set_text_clipboard(overlay::TextClipboard clipboard) {
    clipboard_ = std::move(clipboard);
}

bool InspectorOverlay::begin_text_edit(View* view) {
    if (!overlay::view_has_editable_text(view))
        return false;
    commit_field_edit();
    // The edit target becomes the selection, so the selection box follows it.
    selected_ = view;
    text_target_ = view;
    text_original_ = overlay::editable_text_of(view);
    text_edit_.begin(text_original_);
    return true;
}

void InspectorOverlay::clear_text_edit() {
    text_target_ = nullptr;
    text_original_.clear();
    text_edit_.clear();
}

bool InspectorOverlay::text_target_reachable() const {
    return text_target_ && overlay::tree_contains(root_, text_target_);
}

void InspectorOverlay::push_text_to_target() {
    overlay::set_editable_text_of(text_target_, text_edit_.text());
}

bool InspectorOverlay::commit_text_edit() {
    if (!text_editing())
        return false;
    // A target a tree rebuild freed has nothing left to commit.
    if (!text_target_reachable()) {
        clear_text_edit();
        return false;
    }
    View* target = text_target_;
    const std::string new_text = text_edit_.text();
    const bool changed = new_text != text_original_;
    clear_text_edit();
    overlay::set_editable_text_of(target, new_text);
    if (!changed)
        return false;
    return emit_edit(*target, text_tweak_path_, EditValue{new_text}, "inspector-text-edit");
}

void InspectorOverlay::cancel_text_edit() {
    if (!text_editing())
        return;
    if (text_target_reachable())
        overlay::set_editable_text_of(text_target_, text_original_);
    clear_text_edit();
}

void InspectorOverlay::text_move_caret(int delta, bool extend) {
    if (text_editing())
        text_edit_.move_caret(delta, extend);
}

void InspectorOverlay::text_move_word(int direction, bool extend) {
    if (text_editing())
        text_edit_.move_word(direction, extend);
}

void InspectorOverlay::text_move_home(bool extend) {
    if (text_editing())
        text_edit_.move_home(extend);
}

void InspectorOverlay::text_move_end(bool extend) {
    if (text_editing())
        text_edit_.move_end(extend);
}

void InspectorOverlay::text_select_all() {
    if (text_editing())
        text_edit_.select_all();
}

bool InspectorOverlay::text_copy() {
    if (!text_editing() || !text_has_selection() || !clipboard_.set)
        return false;
    return clipboard_.set(text_edit_.selected_text());
}

bool InspectorOverlay::text_cut() {
    if (!text_editing() || !text_has_selection())
        return false;
    const bool copied = clipboard_.set && clipboard_.set(text_edit_.selected_text());
    // Cut removes the selection even when the clipboard write failed: the
    // delete is the visible half of the action.
    text_edit_.erase_selection();
    push_text_to_target();
    return copied;
}

bool InspectorOverlay::text_paste() {
    if (!text_editing() || !clipboard_.get)
        return false;
    const auto clip = clipboard_.get();
    if (!clip || clip->empty())
        return false;
    text_insert(*clip);
    return true;
}

void InspectorOverlay::text_insert(const std::string& utf8) {
    if (!text_editing())
        return;
    if (!text_target_reachable()) {
        clear_text_edit();
        return;
    }
    text_edit_.insert(utf8);
    push_text_to_target();
}

void InspectorOverlay::text_delete_backward() {
    if (!text_editing())
        return;
    if (!text_target_reachable()) {
        clear_text_edit();
        return;
    }
    if (text_edit_.delete_backward())
        push_text_to_target();
}

void InspectorOverlay::text_delete_forward() {
    if (!text_editing())
        return;
    if (!text_target_reachable()) {
        clear_text_edit();
        return;
    }
    if (text_edit_.delete_forward())
        push_text_to_target();
}

bool InspectorOverlay::handle_text_input(const TextInputEvent& event) {
    if (!active_ || !text_editing())
        return false;
    if (!text_target_reachable()) {
        clear_text_edit();
        return false;
    }
    // Consume empty input too, so it never reaches a widget under the edit.
    if (!event.text.empty())
        text_insert(event.text);
    return true;
}

bool InspectorOverlay::handle_text_key(const KeyEvent& event) {
    // A target a tree rebuild freed ends the edit; the key is then an
    // ordinary one.
    if (!text_target_reachable()) {
        clear_text_edit();
        return false;
    }
    switch (event.key) {
    case KeyCode::enter:
        commit_text_edit();
        return true;
    case KeyCode::escape:
        cancel_text_edit();
        // Leaving the edit returns to the Select tool.
        set_tool(Tool::select);
        return true;
    case KeyCode::backspace:
        text_delete_backward();
        return true;
    case KeyCode::delete_:
        text_delete_forward();
        return true;
    default:
        break;
    }
    if (event.isMainModifier()) {
        switch (event.key) {
        case KeyCode::a:
            text_select_all();
            return true;
        case KeyCode::c:
            text_copy();
            return true;
        case KeyCode::x:
            text_cut();
            return true;
        case KeyCode::v:
            text_paste();
            return true;
        case KeyCode::left:
            text_move_home(event.isShiftDown());
            return true;
        case KeyCode::right:
            text_move_end(event.isShiftDown());
            return true;
        default:
            break;
        }
    }
    if (event.key == KeyCode::left || event.key == KeyCode::right) {
        const int direction = event.key == KeyCode::left ? -1 : 1;
        if (event.isAltDown())
            text_move_word(direction, event.isShiftDown());
        else
            text_move_caret(direction, event.isShiftDown());
        return true;
    }
    // The edit is a single line, so Up and Down go to its ends.
    if (event.key == KeyCode::home || event.key == KeyCode::up) {
        text_move_home(event.isShiftDown());
        return true;
    }
    if (event.key == KeyCode::end_ || event.key == KeyCode::down) {
        text_move_end(event.isShiftDown());
        return true;
    }
    // Everything else is character input, which arrives through
    // handle_text_input; declining here lets the host deliver it.
    return false;
}

bool InspectorOverlay::mouse_text_tool(const ::vellum::view::MouseEvent& event, bool is_press) {
    if (tool_ != Tool::text || !is_press)
        return false;
    // The element under the press, or its nearest ancestor with editable text.
    const View* host = root_.hit_test(event.position);
    while (host && !overlay::view_has_editable_text(host))
        host = host->parent();
    // Pressing anywhere but the element being edited commits that edit.
    if (text_editing() && host != text_target_)
        commit_text_edit();
    if (host)
        begin_text_edit(const_cast<View*>(host));
    // The Text tool owns canvas presses: it never selects or drags.
    return true;
}

void InspectorOverlay::paint_text_edit(canvas::Canvas& canvas) {
    if (!text_editing() || !text_target_reachable())
        return;
    const auto r = overlay::bounds_in_root(text_target_, root_);
    canvas.save();

    if (const auto* label = dynamic_cast<const Label*>(text_target_)) {
        // Label::text_edit_metrics resolves the same font, spacing and
        // alignment the Label paints with, so the caret lands on the glyphs.
        const auto m = label->text_edit_metrics(canvas, text_edit_.text());
        // The metrics are in the label's unscaled local space; the paint pass
        // scales the subtree about its transform origin, so map them the same
        // way.
        const float s = overlay::effective_scale_in_root(label, root_);
        const float ox = label->bounds().width * label->transform_origin_x();
        const float oy = label->bounds().height * label->transform_origin_y();
        const float text_x = r.x + ox + s * (m.local_text_left - ox);
        const float band_y = r.y + oy + s * (m.local_band_y - oy);
        const float band_h = s * m.band_height;
        const auto caret_x = [&](std::size_t bytes) {
            if (m.caret_x_by_byte.empty())
                return 0.0f;
            return s * m.caret_x_by_byte[std::min(bytes, m.caret_x_by_byte.size() - 1)];
        };
        if (text_has_selection()) {
            const auto [lo, hi] = text_edit_.selection();
            const float x0 = text_x + caret_x(lo);
            const float x1 = text_x + caret_x(hi);
            canvas.set_fill_color(overlay::kTextSelection);
            canvas.fill_rect(x0, band_y, std::max(1.0f, x1 - x0), band_h);
        }
        if (text_edit_.tick_caret_visible()) {
            canvas.set_fill_color(overlay::kTextCaret);
            canvas.fill_rect(text_x + caret_x(text_edit_.caret()), band_y, kCaretWidth, band_h);
        }
        canvas.restore();
        return;
    }

    // A TextEditor is one font, top aligned, so a prefix measure is enough.
    float font_size = kFallbackFontSize;
    if (const auto* editor = dynamic_cast<const ::vellum::view::TextEditor*>(text_target_))
        font_size = editor->font_size();
    canvas.set_font_full("system", font_size, 400, 0, 0.0f);
    const auto prefix_w = [&](std::size_t bytes) {
        return bytes == 0 ? 0.0f : canvas.measure_text(text_edit_.text().substr(0, bytes));
    };
    const float band_h = std::min(r.height, font_size * 1.3f);
    if (text_has_selection()) {
        const auto [lo, hi] = text_edit_.selection();
        const float x0 = r.x + prefix_w(lo);
        const float x1 = r.x + prefix_w(hi);
        canvas.set_fill_color(overlay::kTextSelection);
        canvas.fill_rect(x0, r.y, std::max(1.0f, x1 - x0), band_h);
    }
    if (text_edit_.tick_caret_visible()) {
        canvas.set_fill_color(overlay::kTextCaret);
        canvas.fill_rect(r.x + prefix_w(text_edit_.caret()), r.y, kCaretWidth, band_h);
    }
    canvas.restore();
}

} // namespace pulp::vellum_components
