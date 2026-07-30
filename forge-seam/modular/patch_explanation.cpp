#include "forge/patch_explanation.hpp"

#include <forge/design_tokens.hpp>

#include <pulp/view/widgets.hpp>

namespace forge_modular {

namespace color = forge::design::color;

using pulp::view::FlexAlign;
using pulp::view::FlexDirection;
using pulp::view::Label;
using pulp::view::View;

namespace {

/// The plain-English name of a role, for the aside Learning adds.
const char* role_primer(SignalRole role) {
    switch (role) {
        case SignalRole::audio:
            return "What you actually hear. It has to reach an output or there is silence.";
        case SignalRole::pitch:
            return "Which note, and when. Pitch is a voltage; a gate is a yes/no.";
        case SignalRole::clock:
            return "The pulse everything times itself against.";
        case SignalRole::mod:
            return "Slow changes that make a static sound move. Nothing here makes noise on its own.";
    }
    return "";
}

/// Characters per display line, tuned to the Build stage at the design width.
/// Approximate by construction -- an exact fit would need the resolved width,
/// which is not known when the rows are built.
constexpr std::size_t kColumns = 118;

}  // namespace

void PatchExplanation::set_connections(std::vector<Connection> connections,
                                       std::vector<RackModule> modules) {
    connections_ = std::move(connections);
    modules_ = std::move(modules);
    hovered_.reset();
    rebuild();
}

void PatchExplanation::set_depth(ExplainDepth depth) {
    if (depth == depth_) return;
    depth_ = depth;
    rebuild();
}

std::string PatchExplanation::port_label(const std::string& module_id,
                                         const std::string& port_id) const {
    for (const auto& m : modules_) {
        if (m.id != module_id) continue;
        for (const auto& p : m.ports)
            if (p.id == port_id) return m.name + " " + p.name;
        return m.name + " " + port_id;
    }
    return module_id + " " + port_id;
}

std::string PatchExplanation::line_text(std::size_t index) const {
    if (index >= connections_.size()) return {};
    const auto& c = connections_[index];

    // The connection itself, always: at every depth the reader can still see
    // what is plugged into what. Depth adds, it never removes the wiring.
    std::string text = port_label(c.from_module, c.from_port) + " \xE2\x86\x92 " +
                       port_label(c.to_module, c.to_port);

    if (depth_ == ExplainDepth::terse) return text;
    if (!c.why.empty()) text += " \xE2\x80\x94 " + c.why;
    if (depth_ == ExplainDepth::learning) {
        const auto* primer = role_primer(c.role);
        if (primer && *primer) text += " " + std::string(primer);
    }
    return text;
}

std::vector<std::string> PatchExplanation::wrap(const std::string& text,
                                                std::size_t columns) {
    std::vector<std::string> lines;
    if (columns == 0) { lines.push_back(text); return lines; }

    // Existing line breaks are the author's and are kept: an explanation that
    // groups cables by role loses that grouping if its newlines are reflowed
    // away into one paragraph.
    if (text.find('\n') != std::string::npos) {
        std::size_t start = 0;
        while (start <= text.size()) {
            auto nl = text.find('\n', start);
            if (nl == std::string::npos) nl = text.size();
            for (auto& piece : wrap(text.substr(start, nl - start), columns))
                lines.push_back(piece);
            if (nl == text.size()) break;
            start = nl + 1;
        }
        return lines;
    }

    std::string line;
    std::size_t i = 0;
    while (i < text.size()) {
        // Take the next word, including the space that precedes it.
        std::size_t end = text.find(' ', i);
        if (end == std::string::npos) end = text.size();
        const auto word = text.substr(i, end - i);

        if (!line.empty() && line.size() + 1 + word.size() > columns) {
            lines.push_back(line);
            line = word;
        } else {
            if (!line.empty()) line += ' ';
            line += word;
        }
        i = end + 1;
    }
    if (!line.empty()) lines.push_back(line);
    if (lines.empty()) lines.push_back(std::string{});
    return lines;
}

void PatchExplanation::rebuild() {
    while (child_count() > 0) remove_child(child_at(0));
    rows_.clear();

    flex().direction = FlexDirection::column;
    flex().gap = 6;

    for (std::size_t i = 0; i < connections_.size(); ++i) {
        // Built exactly like a Forge chat bubble: a column with a bounded
        // width, holding a label that is also 100% wide and does not shrink.
        // That is the shape soft-wrap measures correctly. A row with a
        // flex-grown text column does not -- its width is unresolved when the
        // label measures, so the row stays one line tall and the wrapped
        // second line paints over the row beneath it.
        auto row = std::make_unique<View>();
        row->flex().direction = FlexDirection::column;
        row->flex().dim_width = {100, pulp::view::DimensionUnit::percent};
        row->flex().flex_shrink = 0;
        row->flex().padding_left = 24;   // room for the role dot
        row->flex().padding_right = 10;
        row->flex().padding_top = 6;
        row->flex().padding_bottom = 6;
        row->set_border_radius(7);

        // A dot in the cable's own colour, so a line and its cable are matched
        // by colour before anything is hovered at all. Absolute, so it never
        // competes with the label for width.
        const auto rgb = role_color(connections_[i].role);
        auto dot = std::make_unique<View>();
        dot->set_position(View::Position::absolute);
        // Only horizontal insets exist on FlexStyle, so the dot sits at the
        // row's content top and the row's top padding lines it up with the
        // first line of text.
        dot->flex().dim_start = {8, pulp::view::DimensionUnit::px};
        dot->flex().preferred_width = 8;
        dot->flex().preferred_height = 8;
        dot->set_background_color(pulp::canvas::Color::rgba8(
            static_cast<std::uint8_t>((rgb >> 16) & 0xFF),
            static_cast<std::uint8_t>((rgb >> 8) & 0xFF),
            static_cast<std::uint8_t>(rgb & 0xFF)));
        dot->set_border_radius(4);
        row->add_child(std::move(dot));

        for (const auto& text : wrap(line_text(i), kColumns)) {
            auto label = std::make_unique<Label>(text);
            label->set_font_family(forge::design::type::display);
            label->set_font_size(12.5f);
            label->set_text_color(color::text_muted);
            label->flex().dim_width = {100, pulp::view::DimensionUnit::percent};
            // No explicit height: a single-line Label measures itself
            // correctly. Pinning one only fights a measurement that is
            // already right.
            label->flex().flex_shrink = 0;
            row->add_child(std::move(label));
        }

        rows_.push_back(row.get());
        add_child(std::move(row));
    }
    request_repaint();
}

void PatchExplanation::hover_line(std::optional<std::size_t> index) {
    if (index && *index >= connections_.size()) index.reset();
    if (index == hovered_) return;
    hovered_ = index;

    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (!rows_[i]) continue;
        const bool on = hovered_ && *hovered_ == i;
        rows_[i]->set_background_color(on ? color::surface_raised
                                          : pulp::canvas::Color::rgba8(0, 0, 0, 0));
        rows_[i]->request_repaint();
    }
    if (on_hover) on_hover(hovered_);
}

}  // namespace forge_modular
