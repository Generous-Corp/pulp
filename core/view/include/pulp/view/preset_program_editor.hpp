#pragma once

/// @file preset_program_editor.hpp
/// Bank and program editor for a PresetLibrary's MIDI program map: pick a bank,
/// see its 128 slots with the preset in each, and assign, clear or move presets.
///
/// The editor has no text field: renaming or renumbering a bank, and choosing
/// which preset goes in a slot, are requests the host answers with its own
/// dialog or picker (`on_bank_text_request`, `on_assign_request`), then calls the
/// matching `complete_*` method.

#include <pulp/canvas/canvas.hpp>
#include <pulp/state/preset_library.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace pulp::view {

class PresetProgramEditor : public View {
  public:
    explicit PresetProgramEditor(state::PresetLibrary& library) : library_(library) {
        set_focusable(true);
        refresh();
    }

    /// Asked to choose a preset for `slot` (a double click or Enter on a row).
    /// The host shows a picker and calls `complete_assign` with the chosen key.
    std::function<void(state::BankProgram slot)> on_assign_request;

    enum class BankTextKind { rename, renumber };
    /// Asked for a bank name or number; answer with `complete_bank_text`.
    std::function<void(BankTextKind kind, const std::string& initial)> on_bank_text_request;

    /// Re-read the library's map. The shown bank and slot are kept when they
    /// still exist; otherwise the first bank is shown.
    void refresh() {
        map_ = library_.program_map();
        const auto& banks = map_.banks();
        bank_index_ = -1;
        for (size_t i = 0; i < banks.size(); ++i)
            if (banks[i].number == bank_number_)
                bank_index_ = static_cast<int>(i);
        if (bank_index_ < 0 && !banks.empty()) {
            bank_index_ = 0;
            bank_number_ = banks[0].number;
        }
    }

    bool has_banks() const {
        return bank_index_ >= 0;
    }
    int bank_count() const {
        return static_cast<int>(map_.banks().size());
    }
    /// The bank shown, or nullptr when the map is empty.
    const state::ProgramBank* current_bank() const {
        return bank_index_ < 0 ? nullptr : &map_.banks()[static_cast<size_t>(bank_index_)];
    }
    int selected_program() const {
        return selected_program_;
    }
    void select_program(int program) {
        selected_program_ = std::clamp(program, 0, 127);
    }

    void next_bank() {
        step_bank(1);
    }
    void previous_bank() {
        step_bank(-1);
    }

    /// The slot the editor is pointing at (bank shown, program selected).
    std::optional<state::BankProgram> selected_slot() const {
        const auto* b = current_bank();
        if (!b)
            return std::nullopt;
        return state::BankProgram{b->number, static_cast<std::uint8_t>(selected_program_)};
    }
    /// The preset in a slot of the shown bank, or nullopt when empty or missing.
    std::optional<state::PresetSummary> preset_in(int program) const {
        const auto* b = current_bank();
        if (!b)
            return std::nullopt;
        return library_.preset_at({b->number, static_cast<std::uint8_t>(program)});
    }

    // ── Edits ───────────────────────────────────────────────────────────

    /// Empty the selected slot.
    bool clear_selected() {
        const auto slot = selected_slot();
        if (!slot)
            return false;
        const bool ok = library_.clear_program(*slot);
        refresh();
        return ok;
    }
    /// The host's answer to `on_assign_request`: put `key` in the selected slot.
    bool complete_assign(const std::string& key) {
        const auto slot = selected_slot();
        if (!slot)
            return false;
        const bool ok = library_.assign_program(*slot, key);
        refresh();
        return ok;
    }
    /// The host's answer to `on_bank_text_request`. A rename may be empty; a
    /// renumber must be a bank number that is free and in range.
    bool complete_bank_text(BankTextKind kind, const std::string& text) {
        const auto* b = current_bank();
        if (!b)
            return false;
        const auto number = b->number;
        bool ok = false;
        if (kind == BankTextKind::rename) {
            ok = library_.set_bank_name(number, text);
        } else {
            const auto parsed = state::parse_bank_program(text + ":0");
            if (!parsed)
                return false;
            ok = library_.renumber_bank(number, parsed->bank);
            if (ok)
                bank_number_ = parsed->bank;
        }
        refresh();
        return ok;
    }
    /// Ask the host to rename or renumber the shown bank.
    bool request_bank_text(BankTextKind kind) {
        const auto* b = current_bank();
        if (!b || !on_bank_text_request)
            return false;
        on_bank_text_request(kind,
                             kind == BankTextKind::rename ? b->name : std::to_string(b->number));
        return true;
    }

    // ── View ────────────────────────────────────────────────────────────

    float row_height() const {
        return row_height_;
    }

    void paint(canvas::Canvas& canvas) override {
        auto b = local_bounds();
        canvas.set_fill_color(resolve_color("preset_browser_bg", canvas::Color::hex(0x16213e)));
        canvas.fill_rounded_rect(b.x, b.y, b.width, b.height, 6);

        canvas.set_fill_color(resolve_color("surface", canvas::Color::hex(0x1a1a2e)));
        canvas.fill_rect(b.x, b.y, b.width, kHeaderHeight);
        canvas.set_font("system", 13);
        canvas.set_fill_color(resolve_color("text", canvas::Color::hex(0xe0e0e0)));

        const auto* bank = current_bank();
        std::string title = "(no banks)";
        if (bank) {
            title = "Bank " + std::to_string(bank->number);
            if (!bank->name.empty())
                title += "  " + bank->name;
        }
        canvas.fill_text(title, b.x + 30, b.y + kHeaderHeight / 2 + 4);
        canvas.set_fill_color(resolve_color("text_muted", canvas::Color::hex(0x808090)));
        canvas.fill_text("\xe2\x97\x80", b.x + 8, b.y + kHeaderHeight / 2 + 4);            // ◀
        canvas.fill_text("\xe2\x96\xb6", b.x + b.width - 18, b.y + kHeaderHeight / 2 + 4); // ▶
        if (!bank)
            return;

        const float list_y = b.y + kHeaderHeight + 4;
        const float list_h = b.y + b.height - list_y;
        canvas.save();
        canvas.clip_rect(b.x, list_y, b.width, list_h);
        const int first = static_cast<int>(scroll_offset_ / row_height_);
        const int count = static_cast<int>(list_h / row_height_) + 2;
        for (int p = first; p < std::min(first + count, 128); ++p) {
            const float y = list_y + static_cast<float>(p) * row_height_ - scroll_offset_;
            if (p == selected_program_) {
                canvas.set_fill_color(
                    resolve_color("list_selected_bg", canvas::Color::hex(0x0f3460)));
                canvas.fill_rect(b.x + 2, y, b.width - 4, row_height_);
            }
            canvas.set_font("system", 10);
            canvas.set_fill_color(resolve_color("text_muted", canvas::Color::hex(0x808090)));
            canvas.fill_text(std::to_string(p), b.x + 6, y + row_height_ * 0.65f);

            canvas.set_font("system", 13);
            const auto slot_key = bank->slots.find(static_cast<std::uint8_t>(p));
            if (slot_key == bank->slots.end()) {
                canvas.set_fill_color(resolve_color("text_muted", canvas::Color::hex(0x606070)));
                canvas.fill_text("(empty)", b.x + 36, y + row_height_ * 0.65f);
                continue;
            }
            const auto preset = library_.find(slot_key->second);
            canvas.set_fill_color(preset ? resolve_color("text", canvas::Color::hex(0xe0e0e0))
                                         : resolve_color("accent", canvas::Color::hex(0xe94560)));
            canvas.fill_text(preset ? preset->display_name : slot_key->second + " (not installed)",
                             b.x + 36, y + row_height_ * 0.65f);
        }
        canvas.restore();
    }

    void on_mouse_event(const MouseEvent& event) override {
        if (!event.is_down)
            return;
        const auto b = local_bounds();
        if (event.position.y < kHeaderHeight) {
            if (event.position.x < 25)
                previous_bank();
            else if (event.position.x > b.width - 25)
                next_bank();
            else if (event.click_count == 2)
                request_bank_text(BankTextKind::rename);
            return;
        }
        const float list_y = kHeaderHeight + 4;
        if (event.position.y < list_y)
            return;
        const int p = static_cast<int>((event.position.y - list_y + scroll_offset_) / row_height_);
        if (p < 0 || p > 127)
            return;
        selected_program_ = p;
        if (event.click_count == 2)
            request_assign();
    }

    bool on_key_event(const KeyEvent& event) override {
        if (!event.is_down)
            return false;
        switch (event.key) {
        case KeyCode::up:
            select_program(selected_program_ - 1);
            return true;
        case KeyCode::down:
            select_program(selected_program_ + 1);
            return true;
        case KeyCode::left:
            previous_bank();
            return true;
        case KeyCode::right:
            next_bank();
            return true;
        case KeyCode::enter:
            return request_assign();
        case KeyCode::delete_:
        case KeyCode::backspace:
            return clear_selected();
        default:
            return false;
        }
    }

    bool wants_mouse_input() const override {
        return true;
    }

  private:
    static constexpr float kHeaderHeight = 32.0f;

    state::PresetLibrary& library_;
    state::ProgramMap map_;
    int bank_index_ = -1;
    std::uint16_t bank_number_ = 0;
    int selected_program_ = 0;
    float row_height_ = 24.0f;
    float scroll_offset_ = 0.0f;

    bool request_assign() {
        const auto slot = selected_slot();
        if (!slot || !on_assign_request)
            return false;
        on_assign_request(*slot);
        return true;
    }

    void step_bank(int delta) {
        const auto& banks = map_.banks();
        if (banks.empty() || bank_index_ < 0)
            return;
        const int n = static_cast<int>(banks.size());
        bank_index_ = (bank_index_ + delta % n + n) % n;
        bank_number_ = banks[static_cast<size_t>(bank_index_)].number;
    }
};

} // namespace pulp::view
