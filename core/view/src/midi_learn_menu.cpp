#include <pulp/view/midi_learn_menu.hpp>

#include <utility>

namespace pulp::view {

std::vector<ContextMenu::Item> midi_learn_menu_items(const MidiLearnTarget& target,
                                                     state::ParamID param) {
    std::vector<ContextMenu::Item> items;
    if (!target.is_available())
        return items;

    const MidiLearnParamState state = target.state ? target.state(param) : MidiLearnParamState{};
    const bool listening = state.phase == MidiLearnParamState::Phase::listening;
    const bool mapped = state.phase == MidiLearnParamState::Phase::mapped;

    ContextMenu::Item learn;
    learn.id = listening ? kMidiLearnCommandCancel : kMidiLearnCommandBegin;
    learn.label = listening ? "Cancel MIDI Learn" : "MIDI Learn";
    learn.enabled = listening ? static_cast<bool>(target.cancel) : static_cast<bool>(target.begin);
    items.push_back(std::move(learn));

    if (mapped) {
        ContextMenu::Item label;
        label.label = "Mapped to " + state.mapping_label;
        label.enabled = false;
        items.push_back(std::move(label));

        ContextMenu::Item clear;
        clear.id = kMidiLearnCommandClear;
        clear.label = "Clear MIDI Mapping";
        clear.enabled = static_cast<bool>(target.clear);
        items.push_back(std::move(clear));
    }
    return items;
}

bool run_midi_learn_menu_command(const MidiLearnTarget& target, state::ParamID param, int command) {
    if (!target.is_available())
        return false;
    switch (command) {
    case kMidiLearnCommandBegin:
        if (!target.begin)
            return false;
        target.begin(param);
        return true;
    case kMidiLearnCommandCancel:
        if (!target.cancel)
            return false;
        target.cancel();
        return true;
    case kMidiLearnCommandClear:
        if (!target.clear)
            return false;
        target.clear(param);
        return true;
    default:
        return false;
    }
}

ContextMenu* show_midi_learn_menu(View* root, Point root_pos, const MidiLearnTarget& target,
                                  state::ParamID param) {
    if (root == nullptr)
        return nullptr;
    auto items = midi_learn_menu_items(target, param);
    if (items.empty())
        return nullptr;
    return ContextMenu::show(root, root_pos, std::move(items),
                             [target, param](std::optional<int> chosen) {
                                 if (chosen)
                                     run_midi_learn_menu_command(target, param, *chosen);
                             });
}

namespace {
// Root-space point for a point local to `widget`: the widget's absolute origin
// (its bounds summed up the parent chain, the arithmetic View::overlay_contains
// uses) plus the local point. Ancestor scale and scroll are not applied.
Point to_root(const View& widget, Point local) {
    float x = local.x;
    float y = local.y;
    for (const View* v = &widget; v != nullptr; v = v->parent()) {
        x += v->bounds().x;
        y += v->bounds().y;
    }
    return {x, y};
}
} // namespace

void attach_midi_learn_menu(View& widget, View* root, state::ParamID param,
                            MidiLearnTarget target) {
    View* const widget_ptr = &widget;
    widget.on_context_menu = [widget_ptr, root, param, target = std::move(target)](Point local) {
        show_midi_learn_menu(root, to_root(*widget_ptr, local), target, param);
    };
}

MidiLearnOverlay::MidiLearnOverlay(state::ParamID param, MidiLearnTarget target)
    : param_(param), target_(std::move(target)) {
    set_hit_testable(false);
    refresh();
}

void MidiLearnOverlay::refresh() {
    MidiLearnParamState next;
    if (target_.is_available() && target_.state)
        next = target_.state(param_);
    if (next.phase == state_.phase && next.mapping_label == state_.mapping_label &&
        next.just_bound == state_.just_bound)
        return;
    state_ = std::move(next);
    request_repaint();
}

void MidiLearnOverlay::paint(canvas::Canvas& canvas) {
    if (state_.phase == MidiLearnParamState::Phase::idle)
        return;

    const Rect b = local_bounds();
    const auto accent = resolve_color("accent.primary", canvas::Color::rgba8(100, 150, 255));
    const auto text_c = resolve_color("text.on_accent", canvas::Color::rgba8(255, 255, 255));

    const auto chip = [&](const std::string& text, canvas::Color fill) {
        canvas.set_font("sans-serif", 10.0f);
        const float w = canvas.measure_text(text) + 10.0f;
        const float h = 14.0f;
        canvas.set_fill_color(fill);
        canvas.fill_rounded_rect(b.x, b.y - h - 2.0f, w, h, 3.0f);
        canvas.set_fill_color(text_c);
        canvas.fill_text(text, b.x + 5.0f, b.y - 5.0f);
    };

    if (state_.phase == MidiLearnParamState::Phase::listening) {
        canvas.set_stroke_color(accent);
        canvas.set_line_width(2.0f);
        canvas.stroke_rounded_rect(b.x, b.y, b.width, b.height, 4.0f);
        chip("Listening...", accent);
        return;
    }

    if (state_.just_bound) {
        canvas.set_stroke_color(accent);
        canvas.set_line_width(2.0f);
        canvas.stroke_rounded_rect(b.x, b.y, b.width, b.height, 4.0f);
        chip(state_.mapping_label, accent);
    }
    canvas.set_fill_color(accent);
    canvas.fill_circle(b.x + b.width - 4.0f, b.y + 4.0f, 2.5f);
}

} // namespace pulp::view
