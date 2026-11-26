#include <pulp/vellum_components/text_edit.hpp>

#include <vellum/platform/clipboard.hpp>
#include <vellum/view/text_editor.hpp>
#include <vellum/view/widgets.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace pulp::vellum_components::overlay {

namespace {

bool is_continuation(unsigned char b) {
    return (b & 0xC0) == 0x80;
}

std::size_t prev_boundary(const std::string& s, std::size_t i) {
    if (i == 0)
        return 0;
    --i;
    while (i > 0 && is_continuation(static_cast<unsigned char>(s[i])))
        --i;
    return i;
}

std::size_t next_boundary(const std::string& s, std::size_t i) {
    const std::size_t n = s.size();
    if (i >= n)
        return n;
    ++i;
    while (i < n && is_continuation(static_cast<unsigned char>(s[i])))
        ++i;
    return i;
}

// Letters, digits, underscore and every non-ASCII byte count as word text.
bool is_word_byte(unsigned char c) {
    return std::isalnum(c) || c == '_' || c >= 0x80;
}

void reflow_from(View* view) {
    for (View* v = view; v; v = v->parent())
        v->invalidate_layout();
}

} // namespace

bool view_has_editable_text(const View* view) {
    return dynamic_cast<const Label*>(view) != nullptr ||
           dynamic_cast<const ::vellum::view::TextEditor*>(view) != nullptr;
}

std::string editable_text_of(const View* view) {
    if (const auto* label = dynamic_cast<const Label*>(view))
        return label->text();
    if (const auto* editor = dynamic_cast<const ::vellum::view::TextEditor*>(view))
        return editor->text();
    return {};
}

void set_editable_text_of(View* view, const std::string& text) {
    if (auto* label = dynamic_cast<Label*>(view))
        label->set_text(text);
    else if (auto* editor = dynamic_cast<::vellum::view::TextEditor*>(view))
        editor->set_text(text);
    else
        return;
    // The text length changes the intrinsic size.
    reflow_from(view);
}

TextClipboard TextClipboard::system() {
    return {[](const std::string& text) { return ::vellum::platform::Clipboard::set_text(text); },
            []() { return ::vellum::platform::Clipboard::get_text(); }};
}

void TextEditBuffer::begin(std::string text) {
    text_ = std::move(text);
    caret_ = text_.size();
    anchor_ = caret_;
    blink_ticks_ = 0;
}

void TextEditBuffer::clear() {
    text_.clear();
    caret_ = 0;
    anchor_ = 0;
    blink_ticks_ = 0;
}

std::pair<std::size_t, std::size_t> TextEditBuffer::selection() const {
    return {std::min(caret_, anchor_), std::max(caret_, anchor_)};
}

std::string TextEditBuffer::selected_text() const {
    const auto [lo, hi] = selection();
    return text_.substr(lo, hi - lo);
}

void TextEditBuffer::move_caret(int delta, bool extend) {
    blink_ticks_ = 0;
    if (!extend && has_selection()) {
        // An arrow over a selection collapses to its near edge and stops there.
        const auto [lo, hi] = selection();
        caret_ = delta < 0 ? lo : hi;
        anchor_ = caret_;
        return;
    }
    for (int k = 0; k < std::abs(delta); ++k)
        caret_ = delta < 0 ? prev_boundary(text_, caret_) : next_boundary(text_, caret_);
    if (!extend)
        anchor_ = caret_;
}

void TextEditBuffer::move_word(int direction, bool extend) {
    blink_ticks_ = 0;
    std::size_t i = caret_;
    const auto word_at = [&](std::size_t k) {
        return is_word_byte(static_cast<unsigned char>(text_[k]));
    };
    if (direction < 0) {
        // Skip separators to the left, then the word before them.
        while (i > 0 && !word_at(i - 1))
            --i;
        while (i > 0 && word_at(i - 1))
            --i;
    } else {
        const std::size_t n = text_.size();
        while (i < n && !word_at(i))
            ++i;
        while (i < n && word_at(i))
            ++i;
    }
    caret_ = i;
    if (!extend)
        anchor_ = i;
}

void TextEditBuffer::move_home(bool extend) {
    blink_ticks_ = 0;
    caret_ = 0;
    if (!extend)
        anchor_ = 0;
}

void TextEditBuffer::move_end(bool extend) {
    blink_ticks_ = 0;
    caret_ = text_.size();
    if (!extend)
        anchor_ = caret_;
}

void TextEditBuffer::select_all() {
    blink_ticks_ = 0;
    anchor_ = 0;
    caret_ = text_.size();
}

bool TextEditBuffer::erase_selection() {
    if (!has_selection())
        return false;
    const auto [lo, hi] = selection();
    text_.erase(lo, hi - lo);
    caret_ = lo;
    anchor_ = lo;
    blink_ticks_ = 0;
    return true;
}

bool TextEditBuffer::insert(const std::string& utf8) {
    erase_selection();
    caret_ = std::min(caret_, text_.size());
    text_.insert(caret_, utf8);
    caret_ += utf8.size();
    anchor_ = caret_;
    blink_ticks_ = 0;
    return true;
}

bool TextEditBuffer::delete_backward() {
    if (erase_selection())
        return true;
    if (caret_ == 0)
        return false;
    const std::size_t prev = prev_boundary(text_, caret_);
    text_.erase(prev, caret_ - prev);
    caret_ = prev;
    anchor_ = prev;
    blink_ticks_ = 0;
    return true;
}

bool TextEditBuffer::delete_forward() {
    if (erase_selection())
        return true;
    if (caret_ >= text_.size())
        return false;
    const std::size_t next = next_boundary(text_, caret_);
    text_.erase(caret_, next - caret_);
    blink_ticks_ = 0;
    return true;
}

bool TextEditBuffer::tick_caret_visible(std::uint32_t period) {
    const bool on = ((blink_ticks_ / period) % 2) == 0;
    ++blink_ticks_;
    return on;
}

} // namespace pulp::vellum_components::overlay
