#include "forge/patch_explanation.hpp"

#include <cmath>

#include <pulp/events/main_thread_dispatcher.hpp>

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
/// Roughly how wide one character is at the 12.5pt display face, in points.
///
/// Wrapping was a fixed 118 columns, which was right when the explanation sat
/// on the stage at about 820pt. Moved into the chat column at 430pt it ran
/// every line off the right edge -- and no test saw it, because they all
/// asserted the STRINGS, which were correct, rather than whether they fitted.
constexpr float kCharWidth = 6.95f;

/// Columns that fit a given content width, floored so a very narrow pane still
/// breaks somewhere rather than emitting one enormous line.
std::size_t columns_for(float width) {
    // The row's own padding, which the text does not get to use.
    const float usable = width - 34.0f;
    if (usable < 40.0f) return 20;
    return static_cast<std::size_t>(usable / kCharWidth);
}

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
        // `display` when the generator resolved a clearer name for prose --
        // numbered when a patch holds two of the same model, so the sentence
        // cannot claim a module is patched into itself.
        const auto& shown = m.display.empty() ? m.name : m.display;
        for (const auto& p : m.ports)
            if (p.id == port_id) return shown + " " + p.name;
        return shown + " " + port_id;
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
    // The role primer is NOT repeated here. It belongs to the role, not to
    // each cable that has it, and a patch with six modulation cables used to
    // print the same sentence six times -- which reads as padding and made
    // "learning has more lines than standard" true without teaching anything.
    // It is written once, under the heading, in rebuild().
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

/// A small mark for each role, drawn in the same geometric language Forge
/// uses elsewhere -- deliberately NOT added to Forge's own icon enum, which
/// would put a Rack-shaped name in the core the other three products share.
///
/// One shape per role, each saying what the role IS rather than decorating it:
/// sound radiating, a step, a pulse, a wave.
class RoleGlyph : public pulp::view::View {
public:
    explicit RoleGlyph(SignalRole role) : role_(role) {
        flex().preferred_width = 11;
        flex().preferred_height = 11;
        flex().flex_shrink = 0;
    }

    void paint(pulp::canvas::Canvas& canvas) override {
        const auto b = bounds();
        const float cx = b.width / 2.0f, cy = b.height / 2.0f;
        const auto rgb = role_color(role_);
        const auto tint = pulp::canvas::Color::rgba8(
            static_cast<std::uint8_t>((rgb >> 16) & 0xFF),
            static_cast<std::uint8_t>((rgb >> 8) & 0xFF),
            static_cast<std::uint8_t>(rgb & 0xFF));
        canvas.set_stroke_color(tint);
        canvas.set_fill_color(tint);
        canvas.set_line_width(1.3f);

        switch (role_) {
            case SignalRole::audio:
                // Sound leaving something: a body and two arcs.
                canvas.fill_rect(cx - 4.0f, cy - 2.0f, 3.0f, 4.0f);
                canvas.stroke_arc(cx - 1.0f, cy, 2.6f, -1.0f, 1.0f);
                canvas.stroke_arc(cx - 1.0f, cy, 4.4f, -1.0f, 1.0f);
                break;
            case SignalRole::pitch:
                // A step: which note, and the change to the next.
                canvas.stroke_line(cx - 4.5f, cy + 3.0f, cx - 0.5f, cy + 3.0f);
                canvas.stroke_line(cx - 0.5f, cy + 3.0f, cx - 0.5f, cy - 3.0f);
                canvas.stroke_line(cx - 0.5f, cy - 3.0f, cx + 4.5f, cy - 3.0f);
                break;
            case SignalRole::clock:
                // A pulse train: on, off, on.
                canvas.stroke_line(cx - 5.0f, cy + 3.0f, cx - 3.0f, cy + 3.0f);
                canvas.stroke_line(cx - 3.0f, cy + 3.0f, cx - 3.0f, cy - 3.0f);
                canvas.stroke_line(cx - 3.0f, cy - 3.0f, cx - 1.0f, cy - 3.0f);
                canvas.stroke_line(cx - 1.0f, cy - 3.0f, cx - 1.0f, cy + 3.0f);
                canvas.stroke_line(cx - 1.0f, cy + 3.0f, cx + 1.0f, cy + 3.0f);
                canvas.stroke_line(cx + 1.0f, cy + 3.0f, cx + 1.0f, cy - 3.0f);
                canvas.stroke_line(cx + 1.0f, cy - 3.0f, cx + 3.0f, cy - 3.0f);
                break;
            case SignalRole::mod:
                // A slow wave: nothing here makes a noise on its own.
                canvas.stroke_arc(cx - 2.5f, cy, 2.5f, 3.14159f, 6.28318f);
                canvas.stroke_arc(cx + 2.5f, cy, 2.5f, 0.0f, 3.14159f);
                break;
        }
    }

private:
    SignalRole role_;
};

void PatchExplanation::on_resized() {
    // Only when the wrap would actually change: rebuilding on every layout
    // pass would discard the hover state constantly.
    if (std::abs(bounds().width - wrapped_at_) < 1.0f) return;

    // NEVER rebuild here. on_resized() runs inside the layout pass that is
    // walking these very children, and replacing them under it segfaults.
    // Deferred to the next turn of the loop, when nothing is traversing.
    if (rewrap_pending_) return;
    rewrap_pending_ = true;
    if (!pulp::events::MainThreadDispatcher::call_async([this] {
            rewrap_pending_ = false;
            if (std::abs(bounds().width - wrapped_at_) >= 1.0f) rebuild();
        })) {
        // No loop to defer onto -- a headless render. Leave the tree alone:
        // rebuilding from inside the layout pass is what crashes, and the
        // content is set after the bounds there anyway, so the wrap is
        // already right.
        rewrap_pending_ = false;
    }
}

void PatchExplanation::rebuild() {
    while (child_count() > 0) remove_child(child_at(0));
    rows_.clear();

    flex().direction = FlexDirection::column;
    flex().gap = 6;

    // Derived from the pane the explanation is actually in, not a constant.
    const auto columns = columns_for(bounds().width);
    wrapped_at_ = bounds().width;

    // Grouped by what each cable carries, in signal order: what you hear, then
    // what decides the notes, then what keeps time, then what moves. A flat
    // list of a dozen cables is a netlist; grouped, the same dozen say how the
    // patch is organised before a single line is read.
    rows_.assign(connections_.size(), nullptr);
    struct Group {
        SignalRole role;
        const char* title;
    };
    static constexpr Group kGroups[] = {
        {SignalRole::audio, "AUDIO"},
        {SignalRole::pitch, "PITCH & GATE"},
        {SignalRole::clock, "CLOCK"},
        {SignalRole::mod,   "MODULATION"},
    };

    for (const auto& group : kGroups) {
        std::vector<std::size_t> members;
        for (std::size_t i = 0; i < connections_.size(); ++i)
            if (connections_[i].role == group.role) members.push_back(i);
        if (members.empty()) continue;   // an absent role is not a heading

        auto header = std::make_unique<View>();
        header->flex().direction = FlexDirection::row;
        header->flex().dim_width = {100, pulp::view::DimensionUnit::percent};
        header->flex().flex_shrink = 0;
        header->flex().padding_top = 10;
        header->flex().padding_bottom = 2;

        auto glyph = std::make_unique<RoleGlyph>(group.role);
        glyph->flex().margin_right = 7;
        glyph->flex().margin_top = 2;
        header->add_child(std::move(glyph));

        auto title = std::make_unique<Label>(group.title);
        title->set_font_family(forge::design::type::mono);
        title->set_font_size(10.5f);
        title->set_text_color(color::text_muted);
        title->flex().flex_grow = 1;
        header->add_child(std::move(title));

        // The count, because "3 CABLES" tells you the shape of the patch
        // before you read a word of it.
        auto count = std::make_unique<Label>(
            std::to_string(members.size()) +
            (members.size() == 1 ? " CABLE" : " CABLES"));
        count->set_font_family(forge::design::type::mono);
        count->set_font_size(10.5f);
        count->set_text_color(color::text_faint);
        header->add_child(std::move(count));
        add_child(std::move(header));

        // At the deepest setting the group says what its role IS, once. This
        // is the concept the reader is here to learn; the cables under it are
        // instances of it.
        if (depth_ == ExplainDepth::learning) {
            if (const auto* primer = role_primer(group.role); primer && *primer) {
                // In its own block: the explanation's column gap separates
                // BLOCKS, so adding the primer's lines directly here spaced
                // them like paragraphs while the cable lines beneath stayed
                // tight -- the same text set two different ways.
                auto note_block = std::make_unique<View>();
                note_block->flex().direction = FlexDirection::column;
                note_block->flex().dim_width = {100, pulp::view::DimensionUnit::percent};
                note_block->flex().flex_shrink = 0;
                note_block->flex().padding_left = 24;
                note_block->flex().padding_bottom = 2;
                for (const auto& piece : wrap(primer, columns)) {
                    auto note = std::make_unique<Label>(piece);
                    note->set_font_family(forge::design::type::display);
                    note->set_font_size(12.0f);
                    note->set_text_color(color::text_faint);
                    note->flex().dim_width = {100, pulp::view::DimensionUnit::percent};
                    note->flex().flex_shrink = 0;
                    note_block->add_child(std::move(note));
                }
                add_child(std::move(note_block));
            }
        }

        for (const std::size_t i : members) {
        // Built exactly like a Forge chat bubble: a column with a bounded
        // width, holding a label that is also 100% wide and does not shrink.
        // That is the shape soft-wrap measures correctly. A row with a
        // flex-grown text column does not -- its width is unresolved when the
        // label measures, so the row stays one line tall and the wrapped
        // second line paints over the row beneath it.
        auto row = std::make_unique<View>();
        row->flex().direction = FlexDirection::column;
        row->flex().gap = 2;             // wrapped lines want a little air
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
        // Centred on the FIRST line rather than sitting at the row's top edge,
        // where it read as a bullet on a line of its own above the text.
        dot->flex().margin_top = 12;
        dot->set_background_color(pulp::canvas::Color::rgba8(
            static_cast<std::uint8_t>((rgb >> 16) & 0xFF),
            static_cast<std::uint8_t>((rgb >> 8) & 0xFF),
            static_cast<std::uint8_t>(rgb & 0xFF)));
        dot->set_border_radius(4);
        row->add_child(std::move(dot));

        for (const auto& text : wrap(line_text(i), columns)) {
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

        rows_[i] = row.get();
        add_child(std::move(row));
        }
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
