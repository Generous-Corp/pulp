#include <pulp/view/text_editor.hpp>
#include "text_edit_model.hpp"

#include <algorithm>

namespace pulp::view {

bool TextEditor::password_contents_allowed() const {
    return allow_password_clipboard || clipboard_policy == ClipboardPolicy::allow_password_contents;
}

bool TextEditor::clipboard_export_allowed() const {
    if (!enabled()) return false;
    if (clipboard_policy == ClipboardPolicy::disabled) return false;
    if (password_mode && !password_contents_allowed()) return false;
    return true;
}

bool TextEditor::clipboard_import_allowed() const {
    if (!can_edit()) return false;
    if (clipboard_policy == ClipboardPolicy::disabled) return false;
    return true;
}

bool TextEditor::copy_to_clipboard() {
    if (!clipboard_export_allowed()) return false;
    if (!has_selection()) return false;
    platform::Clipboard::set_text(selected_text());
    return true;
}

bool TextEditor::cut_to_clipboard() {
    if (!can_edit()) return false;
    if (!clipboard_export_allowed()) return false;
    if (!has_selection()) return false;
    auto text = selected_text();
    const int start = text_edit::clamp_boundary(text_, std::min(selection_start_, selection_end_));
    const int end = text_edit::clamp_boundary(text_, std::max(selection_start_, selection_end_));
    if (!delete_range(start, end)) return false;
    platform::Clipboard::set_text(text);
    return true;
}

bool TextEditor::paste_from_clipboard() {
    if (!clipboard_import_allowed()) return false;
    if (!platform::Clipboard::has_text()) return false;
    auto opt_text = platform::Clipboard::get_text();
    if (!opt_text || opt_text->empty()) return false;
    return replace_selection_or_insert(*opt_text, UndoCoalesce::none, true);
}

} // namespace pulp::view
