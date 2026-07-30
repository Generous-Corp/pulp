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
    // Placed over the composer. The prompt card is centred at 680 wide in a
    // 1280 design, so the list hangs from its left edge.
    root->flex().dim_start = {300, pulp::view::DimensionUnit::px};
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
    if (candidates_.size() > static_cast<std::size_t>(kMaxRows))
        candidates_.resize(kMaxRows);
    // Land on something insertable if there is one, so Enter does the useful
    // thing rather than picking a module the user cannot wire.
    selected_ = 0;
    for (std::size_t i = 0; i < candidates_.size(); ++i) {
        if (candidates_[i].insertable()) { selected_ = static_cast<int>(i); break; }
    }
    rebuild_rows();
}

void MentionOverlay::rebuild_rows() {
    if (!list_ || !root_) return;
    while (list_->child_count() > 0) list_->remove_child(list_->child_at(0));

    for (std::size_t i = 0; i < candidates_.size(); ++i) {
        const auto& c = candidates_[i];
        auto row = std::make_unique<View>();
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
        list_->add_child(std::move(row));
    }
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

bool MentionOverlay::handle_key(int key_code) {
    if (!open_) return false;
    if (key_code == kKeyEscape) { close(); return true; }
    if (candidates_.empty()) return false;

    if (key_code == kKeyDown || key_code == kKeyUp) {
        const int n = static_cast<int>(candidates_.size());
        selected_ = (selected_ + (key_code == kKeyDown ? 1 : n - 1)) % n;
        rebuild_rows();
        return true;
    }
    if (key_code == kKeyReturn) {
        const auto& c = candidates_[static_cast<std::size_t>(selected_)];
        // Enter on a module that is not installed does nothing rather than
        // inserting it: a patch wired to a module the user does not have cannot
        // make sound, and silently producing one is worse than refusing.
        if (!c.insertable()) return true;
        if (on_choose) on_choose(c.slug);
        close();
        return true;
    }
    return false;
}

void MentionOverlay::close() {
    open_ = false;
    if (root_) root_->set_visible(false);
}

}  // namespace forge_modular
