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

constexpr float kBrandColumn = 116.0f;
/// Longest name drawn before it is cut. A Label sizes to its text and does not
/// ellipsize, and flex_shrink cannot shorten a string — so Ohmer's
/// 'BRK ("Break") expander for RKD' ran straight under its GET · FREE badge.
/// Cutting the string is the only thing that actually shortens it.
constexpr std::size_t kNameChars = 22;

/// `text`, cut to `max` with an ellipsis, on a character boundary.
std::string elide(const std::string& text, std::size_t max) {
    if (text.size() <= max) return text;
    std::size_t cut = max;
    // Do not split a UTF-8 sequence; back up to a lead byte.
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    return text.substr(0, cut) + "\u2026";
}
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

/// How many spaces a mention may span. Nearly every two-word brand is two
/// words ("CV funk", "Count Modula", "Frozen Wasteland"); three covers
/// "Audible Instruments Macro Oscillator" and stops well short of swallowing a
/// sentence.
constexpr int kMaxTokenSpaces = 2;

/// The word being typed after the most recent unclosed '@', or no value.
///
/// Scanning back from the caret rather than forward from the start: a prompt can
/// contain several mentions, and only the one under the caret is being edited.
///
/// A SPACE DOES NOT END IT. It used to, unconditionally, which meant "@CV funk"
/// could never be typed — and with it went Audible Instruments, Count Modula,
/// Frozen Wasteland, Impromptu Modular and every other brand whose name has a
/// space in it. So the query is extended across a space and the caller decides:
/// the list stays open while the longer query still matches something, and the
/// moment it matches nothing the text goes back to being ordinary text.
std::optional<std::string> active_token(const std::string& text, std::size_t caret) {
    if (caret > text.size()) caret = text.size();
    int spaces = 0;
    for (std::size_t i = caret; i-- > 0;) {
        const char c = text[i];
        if (c == '@') return text.substr(i + 1, caret - i - 1);
        // A newline is a hard boundary — nobody types a module name across
        // two lines — and so is the third space, which is where "a mention
        // with a space in it" becomes "a sentence after an email address".
        if (c == '\n') return std::nullopt;
        if (c == ' ' && ++spaces > kMaxTokenSpaces) return std::nullopt;
    }
    return std::nullopt;
}

/// Does this query span a space? Those are the ones the caller may abandon.
bool spans_a_space(const std::string& query) {
    return query.find(' ') != std::string::npos;
}

}  // namespace

std::string elide_for_row(const std::string& text) {
    return elide(text, kNameChars);
}

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

    // A notice that outlives the rows. Picking a module the machine does not
    // have inserts it and says so — and it has to say so where somebody
    // typing is looking. The run card cannot: it does not exist until a build
    // starts, so the first version of this message reached nobody.
    auto notice = std::make_unique<Label>("");
    notice_ = notice.get();
    notice->set_font_family(forge::design::type::mono);
    notice->set_font_size(10);
    notice->set_multi_line(true);
    notice->set_text_color(forge::design::color::amber);
    notice->flex().dim_width = {100, pulp::view::DimensionUnit::percent};
    notice->flex().padding_left = 10;
    notice->flex().padding_right = 10;
    notice->flex().padding_top = 6;
    notice->flex().padding_bottom = 6;
    notice->set_visible(false);
    root->add_child(std::move(notice));
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
    // Every row is choosable, so the selection starts at the top rather than
    // hunting for an installed one — skipping rows a person can see is the
    // same confusion as refusing them.
    // Land on something insertable if there is one, so Enter does the useful
    // thing rather than picking a module the user cannot wire.
    selected_ = 0;
    for (std::size_t i = 0; i < candidates_.size(); ++i) {
        { selected_ = static_cast<int>(i); break; }
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
    // EVERY row inserts, installed or not.
    //
    // This refused anything not installed, on the grounds that a patch wired
    // to a module nobody has cannot sound. That is true and it was the wrong
    // place to enforce it: somebody types @, sees six rows, and can select
    // none of them — reported twice as the list being broken, which is what it
    // is from the outside. The check also duplicates one the generator already
    // makes, with wording it already shows ("hold on — this asks for something
    // you don\'t have installed") at the moment it actually matters.
    //
    // So the name goes in the prompt, and anything not installed is announced
    // rather than silently dropped.
    if (!c.insertable() && on_refused) on_refused(c);
    // Set across on_choose, whose whole job is to rewrite the field.
    inserting_ = true;
    if (on_choose) on_choose(c.slug);
    inserting_ = false;
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

        // The brand sits in a column of ITS OWN WIDTH, so the names start
        // wherever the brand happens to end: "Audible Instruments" pushes one
        // name far right while "Count Modula" leaves the next one short, and
        // the list reads as ragged text rather than a table. A fixed column
        // makes the names line up, which is what a person scans down.
        auto brand = std::make_unique<Label>(c.brand);
        brand->set_font_family(forge::design::type::mono);
        brand->set_font_size(10);
        brand->set_text_color(text_faint);
        brand->flex().preferred_width = kBrandColumn;
        brand->flex().flex_shrink = 0;
        row->add_child(std::move(brand));

        auto name = std::make_unique<Label>(elide_for_row(c.name));
        name->set_font_family(forge::design::type::display);
        name->set_font_size(13);
        // An uninstallable row is visibly quieter, because it is an offer
        // rather than a choice.
        name->set_text_color(c.insertable() ? text : text_muted);
        // A long name must give way rather than run under the badge. Ohmer's
        // 'BRK ("Break") expander for RKD' overlapped its GET · FREE, which
        // reads as two pieces of text fighting rather than one row.
        name->flex().flex_shrink = 1;
        name->flex().min_width = 0;
        row->add_child(std::move(name));

        // What the query matched, when it was not the displayed name. Braids
        // answering "br" is right, and only says so if it says so.
        if (!c.alias.empty()) {
            auto alias = std::make_unique<Label>(c.alias);
            alias->set_font_family(forge::design::type::mono);
            alias->set_font_size(10);
            alias->set_text_color(text_faint);
            row->add_child(std::move(alias));
        }

        if (!c.insertable()) {
            // "GET" said only that you could not pick it. It reads as a
            // paywall when the module is free, which most are — the library
            // index this comes from carries open-source plugins. Say which.
            auto state = std::make_unique<Label>(
                c.state == MentionCandidate::Availability::paid ? "PAID"
                                                                : "GET · FREE");
            state->set_font_family(forge::design::type::mono);
            state->set_font_size(9);
            state->set_text_color(c.state == MentionCandidate::Availability::paid
                                      ? forge::design::color::amber
                                      : accent);
            // The badge never shrinks: it is short, and it is the thing that
            // says whether the row can be picked at all.
            state->flex().flex_shrink = 0;
            row->add_child(std::move(state));
        }
        row_views_.push_back(row.get());
        list_->add_child(std::move(row));
    }
    if (last < n) affordance("\u25BC", 1);
    root_->request_repaint();
}

bool MentionOverlay::handle_text(const std::string& text, std::size_t caret) {
    // A new keystroke supersedes whatever the last pick said -- but the PICK
    // is not a keystroke. Choosing a row rewrites the field, the field reports
    // a change, and this ran and cleared the notice in the same frame it was
    // raised: the message existed for one call and reached nobody, which is
    // the second time this exact message has failed to be seen. `inserting_`
    // is set for exactly the duration of the rewrite.
    // The rewrite a pick performs is not the user typing. Choosing a row calls
    // set_text, which reports a change, which lands here — and a query that
    // now ends in a space would re-open the list over the mention that was
    // just inserted.
    if (inserting_) return false;
    if (!notice_text_.empty()) show_notice({});
    const auto token = active_token(text, caret);
    if (!token) {
        if (open_) close();
        return false;
    }
    refresh(*token);
    // A query that reaches across a space is a GUESS that a two-word brand is
    // being typed. It is right for "@CV funk" and wrong for "@vco into a
    // filter", and the two are indistinguishable until the matches are in: no
    // matches means the space really did end the mention, so the list closes
    // and the words go back to being ordinary text.
    if (candidates_.empty() && spans_a_space(*token)) {
        if (open_) close();
        return false;
    }
    open_ = true;
    if (list_) list_->set_visible(true);
    if (root_) root_->set_visible(true);
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
        // Tab completes the highlighted row, the way a shell completion does.
        // Asked for directly: "press tab to auto complete the highlited item".
        case K::tab:
            if (!candidates_.empty()) {
                choose(static_cast<std::size_t>(selected_));
                return true;
            }
            return false;
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
    if (list_) list_->set_visible(false);
    // The panel stays up while it is carrying a notice: the notice is ABOUT
    // the pick that just closed the list, so hiding it with the rows would
    // take the message away in the same frame it appeared.
    if (root_) root_->set_visible(!notice_text_.empty());
    // Same reason as show_notice: visibility is layout, not paint, and the
    // panel that stays up for the notice has to be re-measured now that the
    // rows it was sized around are gone.
    if (list_) list_->invalidate_layout();
    if (root_) {
        root_->invalidate_layout();
        for (auto* p = root_->parent(); p; p = p->parent())
            p->invalidate_layout();
    }
}

void MentionOverlay::attach_notice(pulp::view::Label* label) {
    notice_ = label;
    if (notice_) {
        notice_->set_text(notice_text_);
        notice_->set_visible(!notice_text_.empty());
    }
}

void MentionOverlay::show_notice(const std::string& text) {
    // Its OWN strip, not the list's and not the status card's.
    //
    // The first attempt at this wrote to the status card, which lives on the
    // run card and only exists while a build is happening — so somebody typing
    // a prompt saw nothing at all, which is indistinguishable from the pick
    // having done nothing. That is exactly what was reported, twice.
    //
    // The list is hidden the moment a row is chosen, so a label inside it
    // disappears with it. This sits beside the list and stays until the next
    // keystroke.
    notice_text_ = text;
    if (!notice_) return;
    notice_->set_text(text);
    notice_->set_visible(!text.empty());
    // The panel carries it, so the panel has to be up even with the rows gone.
    if (root_ && !open_) root_->set_visible(!text.empty());
    // Showing a view is a LAYOUT change, not a paint change. A repaint alone
    // draws the label at the size it last had, which for a view that has never
    // been visible is nothing at all -- the notice was set, was visible, and
    // occupied zero height. The headless render missed it because rendering to
    // a PNG lays the tree out from scratch every time; only the running app
    // reuses a stale layout.
    notice_->invalidate_layout();
    if (root_) {
        root_->invalidate_layout();
        for (auto* p = root_->parent(); p; p = p->parent())
            p->invalidate_layout();
    }
    notice_->request_repaint();
}

}  // namespace forge_modular
