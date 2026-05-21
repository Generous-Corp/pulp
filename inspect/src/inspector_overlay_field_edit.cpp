// inspector_overlay_field_edit.cpp — Phase 3b live-editable box-model
// fields for the visual inspector overlay.
//
// Extracted from inspector_overlay.cpp in the 2026-05 refactor (roadmap
// P10-2). Pure mechanical move — the InspectorOverlay member methods
// below are byte-identical to their previous definitions in
// inspector_overlay.cpp; only their translation unit changed. Shared
// color constants live in inspector_overlay_internal.hpp; the
// structural layout constants are static-constexpr members of
// InspectorOverlay reached through the public header.

#include "inspector_overlay_internal.hpp"

#include <pulp/inspect/inspector_overlay.hpp>

#include <choc/text/choc_JSON.h>

#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <utility>

namespace pulp::inspect {

// ── Phase 3b — Live-editable box-model fields ───────────────────────────────
//
// Click on a numeric value in the property panel → enter edit mode.
// Typed digits / decimal / sign extend the buffer; arrows nudge (±1
// plain, ±10 Shift, ±100 Cmd matching Figma); Enter commits (tweak
// + persisted); Esc cancels (revert); Tab commits + moves to the
// next editable field. The tweak is emitted via the SAME
// emit_tweak_for_selection() path Phase 3a drag-handles use, so
// downstream persistence (Phase 1 disk write) gets both gestures for
// free.
//
// Real-time preview: we write to the underlying View (flex().padding
// = N etc.) on every keystroke + arrow nudge so Yoga reflows live —
// matching the spec's "updates in real-time as Yoga reflow runs"
// requirement. The tweak (anchor-keyed persisted edit) is emitted
// only on commit, so a user who Esc-cancels never poisons the store.

int InspectorOverlay::editable_field_at(Point p) const {
    for (std::size_t i = 0; i < editable_fields_.size(); ++i) {
        const auto& f = editable_fields_[i];
        if (p.x >= f.bounds.x && p.x <= f.bounds.x + f.bounds.width &&
            p.y >= f.bounds.y && p.y <= f.bounds.y + f.bounds.height)
            return static_cast<int>(i);
    }
    return -1;
}

float InspectorOverlay::read_field_value(std::string_view field_path) const {
    if (!selected_) return 0.0f;
    const auto& f = selected_->flex();
    if (field_path == "layout.width")    return f.preferred_width;
    if (field_path == "layout.height")   return f.preferred_height;
    if (field_path == "layout.padding")  return f.padding;
    if (field_path == "layout.margin")   return f.margin;
    if (field_path == "style.opacity")   return selected_->opacity();
    // style.font_size is documented in the spec but View has no
    // direct font_size accessor at the View level — Label / TextEditor
    // own their own font sizes. Skip until widget-aware property
    // mapping ships (noted in PR body).
    return 0.0f;
}

void InspectorOverlay::write_field_value(std::string_view field_path, float value) {
    if (!selected_) return;
    auto& f = selected_->flex();
    bool layout_touched = false;
    if (field_path == "layout.width") {
        f.preferred_width = value; layout_touched = true;
    } else if (field_path == "layout.height") {
        f.preferred_height = value; layout_touched = true;
    } else if (field_path == "layout.padding") {
        f.padding = value; layout_touched = true;
    } else if (field_path == "layout.margin") {
        f.margin = value; layout_touched = true;
    } else if (field_path == "style.opacity") {
        // Opacity clamps to [0,1] inside View::set_opacity.
        selected_->set_opacity(value);
    }
    if (layout_touched) {
        selected_->invalidate_layout();
        // Yoga propagates from the dirty node up to the next absolute-
        // position container or the root; mark up to the root so the
        // next paint pass definitely recomputes. (Cheap — the flag is
        // just a bool.)
        for (View* v = selected_; v; v = v->parent())
            v->invalidate_layout();
    }
}

bool InspectorOverlay::begin_field_edit(std::string field_path, float initial_value) {
    if (!selected_) return false;
    if (field_path.empty()) return false;
    editing_field_ = std::move(field_path);
    edit_target_view_ = selected_;
    edit_original_value_ = initial_value;
    // Format as integer when the value is whole-number-ish; this
    // matches the static display logic in paint_props_section so the
    // user sees the same digits they were looking at a frame ago.
    std::ostringstream oss;
    if (std::abs(initial_value - std::round(initial_value)) < 1e-4f) {
        oss << static_cast<int>(std::round(initial_value));
    } else {
        oss << std::fixed << std::setprecision(2) << initial_value;
    }
    edit_buffer_ = oss.str();
    edit_caret_pos_ = edit_buffer_.size();
    return true;
}

bool InspectorOverlay::commit_field_edit() {
    if (editing_field_.empty()) return false;

    // Parse buffer; tolerate trailing whitespace / empty (treat empty
    // as "no change" — revert without emitting a tweak).
    bool emitted = false;
    if (!edit_buffer_.empty()) {
        char* end = nullptr;
        double parsed = std::strtod(edit_buffer_.c_str(), &end);
        if (end != edit_buffer_.c_str()) {
            float value = static_cast<float>(parsed);
            // Real-time preview already applied on each keystroke, but
            // ensure the final committed value is correct (in case the
            // last keystroke wasn't a digit).
            write_field_value(editing_field_, value);
            // Emit the tweak — persisted edit, anchor-keyed.
            emit_tweak_for_selection(editing_field_,
                                     choc::value::createFloat32(value),
                                     "inspector-keyboard-edit");
            emitted = true;
        }
    }

    editing_field_.clear();
    edit_buffer_.clear();
    edit_caret_pos_ = 0;
    edit_target_view_ = nullptr;
    return emitted;
}

void InspectorOverlay::cancel_field_edit() {
    if (editing_field_.empty()) return;
    // Revert the underlying View to the original value — real-time
    // preview may have mutated it.
    write_field_value(editing_field_, edit_original_value_);
    editing_field_.clear();
    edit_buffer_.clear();
    edit_caret_pos_ = 0;
    edit_target_view_ = nullptr;
}

void InspectorOverlay::apply_edit_buffer_to_view() {
    if (editing_field_.empty()) return;
    if (edit_buffer_.empty()) return;
    char* end = nullptr;
    double parsed = std::strtod(edit_buffer_.c_str(), &end);
    if (end == edit_buffer_.c_str()) return;  // not a number yet
    write_field_value(editing_field_, static_cast<float>(parsed));
}

// Translate a KeyCode into the digit / sign / decimal character it
// would produce in a US-layout context. Shift is intentionally
// ignored — the spec only needs unmodified digits + period + minus.
// (Full keyboard mapping is platform-layer concern; this is a
// pragmatic subset for box-model numeric entry.)
static char key_to_char(KeyCode k, bool shift) {
    int v = static_cast<int>(k);
    if (v >= '0' && v <= '9') return static_cast<char>(v);
    // We treat the keys as both their unshifted and shifted symbols
    // for "." and "-" because Pulp's KeyCode enum doesn't define
    // separate "period" / "minus" entries — platform code dispatches
    // them via text-input. Phase 3b accepts numeric editing only;
    // digits cover 99% of use. Decimal entry on platforms without a
    // text-input path can be added by extending KeyCode (filed as a
    // follow-up in the PR body if it becomes a real ask).
    (void)shift;
    return 0;
}

bool InspectorOverlay::handle_edit_key(const KeyEvent& event) {
    // ── Cancel: Esc ────────────────────────────────────────────────
    if (event.key == KeyCode::escape) {
        cancel_field_edit();
        return true;
    }

    // ── Commit: Enter ──────────────────────────────────────────────
    if (event.key == KeyCode::enter) {
        commit_field_edit();
        return true;
    }

    // ── Tab: commit + move to next field ──────────────────────────
    if (event.key == KeyCode::tab) {
        // Find current field index, then move +1 (Shift+Tab = -1)
        // among the editable_fields_ list (populated by the last
        // paint). The list is repopulated each paint so it's a stable
        // ordering matching what the user sees.
        std::string current = editing_field_;
        commit_field_edit();

        int idx = -1;
        for (std::size_t i = 0; i < editable_fields_.size(); ++i) {
            if (editable_fields_[i].path == current) { idx = static_cast<int>(i); break; }
        }
        if (idx >= 0 && !editable_fields_.empty()) {
            int step = event.isShiftDown() ? -1 : 1;
            int next = (idx + step) % static_cast<int>(editable_fields_.size());
            if (next < 0) next += static_cast<int>(editable_fields_.size());
            const auto& nf = editable_fields_[next];
            // Re-read the value: previous commit may have changed it.
            float v = read_field_value(nf.path);
            begin_field_edit(nf.path, v);
        }
        return true;
    }

    // ── Backspace: trim one char ──────────────────────────────────
    if (event.key == KeyCode::backspace) {
        if (edit_caret_pos_ > 0 && !edit_buffer_.empty()) {
            edit_buffer_.erase(edit_caret_pos_ - 1, 1);
            --edit_caret_pos_;
            apply_edit_buffer_to_view();
        }
        return true;
    }

    // ── Arrow nudging: ±1 / ±10 (shift) / ±100 (cmd) ──────────────
    if (event.key == KeyCode::up || event.key == KeyCode::down) {
        float step = 1.0f;
        if (event.isShiftDown()) step = 10.0f;
        if (event.isMainModifier()) step = 100.0f;
        if (event.key == KeyCode::down) step = -step;

        // Parse current buffer + apply step. Fall back to original
        // value if parse fails (e.g. buffer is empty / only "-").
        float current = edit_original_value_;
        if (!edit_buffer_.empty()) {
            char* end = nullptr;
            double parsed = std::strtod(edit_buffer_.c_str(), &end);
            if (end != edit_buffer_.c_str())
                current = static_cast<float>(parsed);
        }
        float new_v = current + step;

        // Re-format the buffer; keep integer formatting if both ends
        // are whole numbers so we don't surprise the user with
        // suddenly-decimal values from arrow nudging.
        std::ostringstream oss;
        if (std::abs(new_v - std::round(new_v)) < 1e-4f &&
            std::abs(current - std::round(current)) < 1e-4f) {
            oss << static_cast<int>(std::round(new_v));
        } else {
            oss << std::fixed << std::setprecision(2) << new_v;
        }
        edit_buffer_ = oss.str();
        edit_caret_pos_ = edit_buffer_.size();
        apply_edit_buffer_to_view();
        return true;
    }

    // ── Left / Right: caret movement ──────────────────────────────
    if (event.key == KeyCode::left) {
        if (edit_caret_pos_ > 0) --edit_caret_pos_;
        return true;
    }
    if (event.key == KeyCode::right) {
        if (edit_caret_pos_ < edit_buffer_.size()) ++edit_caret_pos_;
        return true;
    }

    // ── Home / End ────────────────────────────────────────────────
    if (event.key == KeyCode::home) {
        edit_caret_pos_ = 0;
        return true;
    }
    if (event.key == KeyCode::end_) {
        edit_caret_pos_ = edit_buffer_.size();
        return true;
    }

    // ── Digit insertion ───────────────────────────────────────────
    // Refuse digits when a modifier (Cmd / Ctrl) is held so we don't
    // swallow chord shortcuts.
    if (event.isCmdDown() || event.isCtrlDown()) return false;

    char c = key_to_char(event.key, event.isShiftDown());
    if (c >= '0' && c <= '9') {
        edit_buffer_.insert(edit_caret_pos_, 1, c);
        ++edit_caret_pos_;
        apply_edit_buffer_to_view();
        return true;
    }

    return false;
}

} // namespace pulp::inspect
