#include "forge/mention_overlay.hpp"

#include <forge/design_tokens.hpp>

#include <pulp/view/buttons.hpp>
#include <pulp/view/widgets.hpp>

#include <algorithm>

namespace forge_modular {

namespace {

using forge::design::color::accent;
using forge::design::color::line;
using forge::design::color::surface_panel;
using forge::design::color::surface_raised;
using forge::design::color::text;
using forge::design::color::text_faint;
using forge::design::color::text_muted;
using pulp::view::FlexAlign;
using pulp::view::FlexDirection;
using pulp::view::Label;
using pulp::view::View;

constexpr float kRowHeight = 34.0f;
constexpr int kMaxRows = 6;
/// Where the list hangs: just below the composer card, which ends around
/// y=437 in the 1280x800 design. Rendered and measured rather than guessed --
/// at 380 the list sat ON the card, hiding the search and Random buttons,
/// which is its own kind of broken even though the rows were visible.
constexpr float kListTopMargin = 448.0f;

// Key codes, matching the platform's. Named so the handler reads as intent.
constexpr int kKeyReturn = 36;
constexpr int kKeyEscape = 53;
constexpr int kKeyDown = 125;
constexpr int kKeyUp = 126;

/// The word being typed after the most recent unclosed '@', or no value.
///
/// Scanning back from the caret rather than forward from the start: a prompt can
/// contain several mentions, and only the one under the caret is being edited.
std::optional<std::string> active_token(const std::string& text, std::size_t caret) {
    if (caret > text.size()) caret = text.size();
    for (std::size_t i = caret; i-- > 0;) {
        const char c = text[i];
        if (c == '@') return text.substr(i + 1, caret - i - 1);
        // A space closes it: "@ " is somebody typing an address, not a mention.
        if (c == ' ' || c == '\n') return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace

std::unique_ptr<View> MentionOverlay::build() {
    auto root = std::make_unique<View>();
    root_ = root.get();
    root->set_position(View::Position::absolute);
    root->flex().direction = FlexDirection::column;
    // Anchored ABOVE the composer, which is where a dropdown for that field
    // belongs. It had only a horizontal offset, so it rendered against the top
    // of the window -- far from the text it was completing, and easy to miss
    // entirely when a narrow query left one row.
    //
    // Bottom-anchored rather than top: the list grows upward as matches are
    // added, so the row nearest the prompt stays put instead of the whole list
    // sliding while the user types.
    // Near the composer, not the window corner. It had only a horizontal
    // offset, so it rendered against the TOP of the window -- nowhere near the
    // text it completes, and easy to miss entirely when a narrow query left a
    // single row. That is what "typing @braids does nothing" actually was.
    //
    // Absolute placement here is margin-driven, the same as every other
    // absolutely-placed view in the chrome.
    root->flex().margin_left = 300;
    root->flex().margin_top = kListTopMargin;
    root->flex().preferred_width = 380;
    root->set_background_color(surface_panel);
    root->set_border(line, 1, forge::design::radius::medium);
    root->set_visible(false);

    auto list = std::make_unique<View>();
    list_ = list.get();
    list->flex().direction = FlexDirection::column;
    root->add_child(std::move(list));
    return root;
}

void MentionOverlay::refresh(const std::string& query) {
    query_ = query;
    candidates_ = source_ ? source_(query) : std::vector<MentionCandidate>{};
    // Every match is kept. This used to truncate to the six that fit, which
    // meant there was nothing to scroll TO -- the rest were discarded before
    // anything could ask for them, so a list of two hundred silently became a
    // list of six.
    first_visible_ = 0;
    // Land on something insertable if there is one, so Enter does the useful
    // thing rather than picking a module the user cannot wire.
    selected_ = 0;
    for (std::size_t i = 0; i < candidates_.size(); ++i) {
        if (candidates_[i].insertable()) { selected_ = static_cast<int>(i); break; }
    }
    rebuild_rows();
}

int MentionOverlay::visible_rows() { return kMaxRows; }

void MentionOverlay::move_selection(int delta) {
    if (candidates_.empty()) return;
    const int n = static_cast<int>(candidates_.size());
    // Wraps, like a menu. Landing on nothing at the end of a long list is a
    // dead end the user has to back out of.
    selected_ = ((selected_ + delta) % n + n) % n;
    scroll_to_selection();
    rebuild_rows();
}

void MentionOverlay::scroll_to_selection() {
    const int n = static_cast<int>(candidates_.size());
    const int rows = kMaxRows;
    if (n <= rows) { first_visible_ = 0; return; }
    if (selected_ < first_visible_) first_visible_ = selected_;
    if (selected_ >= first_visible_ + rows) first_visible_ = selected_ - rows + 1;
    first_visible_ = std::max(0, std::min(first_visible_, n - rows));
}

void MentionOverlay::choose(std::size_t index) {
    if (index >= candidates_.size()) return;
    const auto& c = candidates_[index];
    // Choosing a module that is not installed does nothing rather than
    // inserting it: a patch wired to a module the user does not have cannot
    // make sound, and silently producing one is worse than refusing.
    if (!c.insertable()) return;
    if (on_choose) on_choose(c.slug);
    close();
}

void MentionOverlay::highlight_selected() {
    for (std::size_t r = 0; r < row_views_.size(); ++r) {
        auto* v = row_views_[r];
        if (!v) continue;
        const bool on = static_cast<int>(r) + first_visible_ == selected_;
        v->set_background_color(on ? surface_raised
                                   : pulp::canvas::Color::rgba8(0, 0, 0, 0));
        v->request_repaint();
    }
}

void MentionOverlay::rebuild_rows() {
    if (!list_ || !root_) return;
    row_views_.clear();
    while (list_->child_count() > 0) list_->remove_child(list_->child_at(0));

    const int n = static_cast<int>(candidates_.size());
    const int last = std::min(n, first_visible_ + kMaxRows);

    // "More above" / "more below", the way a menu says there is further to go.
    // Without them a windowed list is indistinguishable from a short one, and
    // the user has no reason to press Down again.
    auto affordance = [&](const char* glyph, int delta) {
        auto bar = std::make_unique<View>();
        bar->flex().direction = FlexDirection::row;
        bar->flex().justify_content = pulp::view::FlexJustify::center;
        bar->flex().align_items = FlexAlign::center;
        bar->flex().preferred_height = 16;
        auto mark = std::make_unique<Label>(glyph);
        mark->set_font_family(forge::design::type::mono);
        mark->set_font_size(9);
        mark->set_text_color(text_faint);
        bar->add_child(std::move(mark));
        // Clickable, because an arrow that is only a hint is a control that
        // does not work.
        bar->on_click = [this, delta] { move_selection(delta); };
        list_->add_child(std::move(bar));
    };
    if (first_visible_ > 0) affordance("\u25B2", -1);

    for (std::size_t i = static_cast<std::size_t>(first_visible_);
         i < static_cast<std::size_t>(last); ++i) {
        const auto& c = candidates_[i];
        auto row = std::make_unique<View>();
        // Click to choose, hover to select -- a dropdown's two obvious
        // gestures. Neither existed, so the list could be looked at and not
        // used.
        row->on_click = [this, i] { choose(i); };
        row->on_hover_enter = [this, i] {
            // Colour only. Rebuilding here destroys the row whose handler is
            // running -- the framework is mid-walk over these children when it
            // calls this -- and the walk then continues on freed memory.
            selected_ = static_cast<int>(i);
            highlight_selected();
        };
        row->flex().direction = FlexDirection::row;
        row->flex().align_items = FlexAlign::center;
        row->flex().preferred_height = kRowHeight;
        row->flex().padding_left = 10;
        row->flex().padding_right = 10;
        row->flex().gap = 8;
        if (static_cast<int>(i) == selected_)
            row->set_background_color(surface_raised);

        auto brand = std::make_unique<Label>(c.brand);
        brand->set_font_family(forge::design::type::mono);
        brand->set_font_size(10);
        brand->set_text_color(text_faint);
        row->add_child(std::move(brand));

        auto name = std::make_unique<Label>(c.name);
        name->set_font_family(forge::design::type::display);
        name->set_font_size(13);
        // An uninstallable row is visibly quieter, because it is an offer
        // rather than a choice.
        name->set_text_color(c.insertable() ? text : text_muted);
        row->add_child(std::move(name));

        if (!c.insertable()) {
            auto state = std::make_unique<Label>(
                c.state == MentionCandidate::Availability::paid ? "PAID" : "GET");
            state->set_font_family(forge::design::type::mono);
            state->set_font_size(9);
            state->set_text_color(c.state == MentionCandidate::Availability::paid
                                      ? forge::design::color::amber
                                      : accent);
            row->add_child(std::move(state));
        }
        row_views_.push_back(row.get());
        list_->add_child(std::move(row));
    }
    if (last < n) affordance("\u25BC", 1);
    root_->request_repaint();
}

bool MentionOverlay::handle_text(const std::string& text, std::size_t caret) {
    const auto token = active_token(text, caret);
    if (!token) {
        if (open_) close();
        return false;
    }
    open_ = true;
    if (root_) root_->set_visible(true);
    refresh(*token);
    // Opening on '@' does not consume the '@' itself -- the character belongs in
    // the prompt, and swallowing it would make the field feel broken.
    return false;
}

bool MentionOverlay::handle_key_event(const pulp::view::KeyEvent& event) {
    if (!open_ || !event.is_down) return false;
    using K = pulp::view::KeyCode;
    switch (event.key) {
        case K::up:     move_selection(-1); return true;
        case K::down:   move_selection(1);  return true;
        case K::escape: close();            return true;
        case K::enter:
            // Enter belongs to the LIST while it is open, or choosing a module
            // would also submit the prompt -- one keystroke doing two things,
            // one of them unasked for.
            if (!candidates_.empty()) {
                choose(static_cast<std::size_t>(selected_));
                return true;
            }
            return false;
        default: return false;
    }
}

bool MentionOverlay::handle_key(int key_code) {
    if (!open_) return false;
    if (key_code == kKeyEscape) { close(); return true; }
    if (candidates_.empty()) return false;

    if (key_code == kKeyDown || key_code == kKeyUp) {
        move_selection(key_code == kKeyDown ? 1 : -1);
        return true;
    }
    if (key_code == kKeyReturn) {
        choose(static_cast<std::size_t>(selected_));
        return true;
    }
    return false;
}

void MentionOverlay::close() {
    open_ = false;
    if (root_) root_->set_visible(false);
}

}  // namespace forge_modular
