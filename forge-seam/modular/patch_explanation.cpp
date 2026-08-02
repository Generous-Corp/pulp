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

/// The same, for the monospaced face the wiring is set in. Every glyph is one
/// advance wide there, so this is the measurement rather than an average.
constexpr float kMonoCharWidth = 7.35f;

/// Type sizes. The wiring is a shade smaller than the prose it explains: it is
/// a label, and monospace at a matched size reads as louder than the sentence
/// it is supposed to be subordinate to.
constexpr float kWiringSize = 12.0f;
constexpr float kProseSize = 12.5f;

/// How far a reason is indented under the cable it belongs to. Enough that the
/// wiring above it starts a column of its own.
constexpr float kWhyIndent = 13.0f;

/// The row's own padding, which the text does not get to use.
constexpr float kRowPadding = 34.0f;

/// Columns that fit a given content width, floored so a very narrow pane still
/// breaks somewhere rather than emitting one enormous line.
std::size_t columns_for(float width, float char_width = kCharWidth,
                        float indent = 0.0f) {
    const float usable = width - kRowPadding - indent;
    if (usable < 40.0f) return 20;
    return static_cast<std::size_t>(usable / char_width);
}

}  // namespace

void PatchExplanation::set_connections(std::vector<Connection> connections,
                                       std::vector<RackModule> modules) {
    connections_ = std::move(connections);
    modules_ = std::move(modules);
    hovered_.reset();
    hovered_role_.reset();
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
    //
    // Just the wiring. The reason is a different claim and is set as one, on
    // its own line -- see why_text(). Concatenating them here produced a
    // paragraph in which "VCO SAW" and "the raw material the filter shapes"
    // carried the same weight, and the list could no longer be skimmed for
    // the connection alone.
    return port_label(c.from_module, c.from_port) + " \xE2\x86\x92 " +
           port_label(c.to_module, c.to_port);
}

std::string PatchExplanation::why_text(std::size_t index) const {
    if (index >= connections_.size()) return {};
    if (depth_ == ExplainDepth::terse) return {};
    // The role primer is NOT repeated here. It belongs to the role, not to
    // each cable that has it, and a patch with six modulation cables used to
    // print the same sentence six times -- which reads as padding and made
    // "learning has more lines than standard" true without teaching anything.
    // It is written once, under the heading, in rebuild().
    return connections_[index].why;
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
        // No loop to defer onto. The tree still must not be rebuilt from
        // inside the layout pass -- that is what segfaults -- so the request
        // is KEPT and the next poll applies it.
        //
        // Clearing it here was the bug: a hosted plugin has no dispatcher of
        // its own, so the re-wrap simply never happened and the rows stayed
        // laid out for whatever width the view was FIRST built at. Resize the
        // window and the text wrapped to more lines than the layout had
        // allowed for, and ran over what was below it.
        stale_wrap_ = true;
    }
}

void PatchExplanation::apply_pending_rewrap() {
    // Called from the shell's poll, which is neither a layout pass nor an
    // event delivery -- the two places where replacing these children is
    // unsafe.
    if (!stale_wrap_) return;
    stale_wrap_ = false;
    rewrap_pending_ = false;
    if (std::abs(bounds().width - wrapped_at_) >= 1.0f) rebuild();
}

void PatchExplanation::set_request(std::string request) {
    if (request == request_) return;
    request_ = std::move(request);
    rebuild();
}

void PatchExplanation::rebuild() {
    while (child_count() > 0) remove_child(child_at(0));
    rows_.clear();
    headings_.clear();

    flex().direction = FlexDirection::column;
    // Cables of one role sit tight against each other and the space is spent
    // at the group boundaries instead, which is what makes the grouping
    // visible. A uniform gap spaced a role's own cables exactly as far apart
    // as two different roles, so the headings were the only thing saying the
    // list had a shape.
    flex().gap = 2;

    // Derived from the pane the explanation is actually in, not a constant.
    const auto columns = columns_for(bounds().width);

    // The request first, in its own quiet box.
    //
    // Everything below this is what the patch DOES. What it was asked to do
    // appeared only in the chat rail, which is a different panel and often
    // scrolled away by the time the rack is drawn — so the explanation opened
    // straight into "AUDIO / VCO PLS -> VCF IN" with nothing to compare it
    // against. Repeating it at the end is not the same: the question a reader
    // has is "is this what I asked for", and they have it before they read,
    // not after.
    if (!request_.empty()) {
        auto box = std::make_unique<pulp::view::View>();
        box->flex().direction = FlexDirection::column;
        box->flex().dim_width = {100, pulp::view::DimensionUnit::percent};
        box->flex().padding_left = 10;
        box->flex().padding_right = 10;
        box->flex().padding_top = 8;
        box->flex().padding_bottom = 8;
        box->flex().margin_bottom = 10;
        box->set_background_color(forge::design::color::surface_raised);

        for (const auto& line : wrap(request_, columns)) {
            auto l = std::make_unique<pulp::view::Label>(line);
            l->set_text_color(forge::design::color::text_muted);
            l->set_font_size(12);
            box->add_child(std::move(l));
        }
        add_child(std::move(box));
    }
    const auto wiring_columns = columns_for(bounds().width, kMonoCharWidth);
    const auto why_columns = columns_for(bounds().width, kCharWidth, kWhyIndent);
    wrapped_at_ = bounds().width;

    // A patch nobody generated -- imported, or one of the shipped examples --
    // has no per-cable reasoning and never will. It still deserves a sentence,
    // and the only honest one is computed from the wiring itself: how big it
    // is, and how far the sound travels to get out. Shown only when there is
    // no prose, so a generated patch is not told what it already says better.
    bool any_prose = false;
    for (const auto& c : connections_)
        if (!c.why.empty()) { any_prose = true; break; }

    if (!any_prose && !connections_.empty()) {
        std::size_t audio = 0;
        for (const auto& c : connections_)
            if (c.role == SignalRole::audio) ++audio;
        std::string intro = std::to_string(modules_.size()) +
            (modules_.size() == 1 ? " module, " : " modules, ") +
            std::to_string(connections_.size()) +
            (connections_.size() == 1 ? " cable." : " cables.");
        if (audio > 0) {
            intro += " The audio path is " + std::to_string(audio) +
                     (audio == 1 ? " cable long" : " cables long") +
                     "; everything else exists to make " +
                     (audio == 1 ? "it move." : "those move.");
        } else {
            // Worth saying plainly: a patch with no audio role reaching an
            // output is silent, and that is the first thing to check.
            intro += " Nothing here is carrying audio.";
        }

        auto note = std::make_unique<View>();
        note->flex().direction = FlexDirection::column;
        note->flex().dim_width = {100, pulp::view::DimensionUnit::percent};
        note->flex().flex_shrink = 0;
        note->flex().padding_bottom = 6;
        for (const auto& piece : wrap(intro, columns)) {
            auto line = std::make_unique<Label>(piece);
            line->set_font_family(forge::design::type::display);
            line->set_font_size(12.5f);
            line->set_text_color(color::text_muted);
            line->flex().dim_width = {100, pulp::view::DimensionUnit::percent};
            line->flex().flex_shrink = 0;
            note->add_child(std::move(line));
        }
        add_child(std::move(note));
    }

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
        // The air between one role and the next. Deliberately much larger than
        // the gap between two cables of the same role.
        header->flex().padding_top = 16;
        header->flex().padding_bottom = 6;

        auto glyph = std::make_unique<RoleGlyph>(group.role);
        glyph->flex().margin_right = 7;
        glyph->flex().margin_top = 2;
        header->add_child(std::move(glyph));

        // The strong ink: a heading is the thing being scanned for, and set in
        // the same grey as the sentences beneath it, it stopped acting as one.
        auto title = std::make_unique<Label>(group.title);
        title->set_font_family(forge::design::type::mono);
        title->set_font_size(10.5f);
        title->set_text_color(color::text_strong);
        title->flex().flex_shrink = 0;
        header->add_child(std::move(title));

        // A rule from the title to the count, so a group reads as a band
        // across the pane rather than as one more line of text.
        auto rule = std::make_unique<View>();
        rule->flex().flex_grow = 1;
        rule->flex().preferred_height = 1;
        rule->flex().align_self = FlexAlign::center;
        rule->flex().margin_left = 10;
        rule->flex().margin_right = 10;
        rule->set_background_color(color::line);
        header->add_child(std::move(rule));

        // The count, because "3 CABLES" tells you the shape of the patch
        // before you read a word of it.
        auto count = std::make_unique<Label>(
            std::to_string(members.size()) +
            (members.size() == 1 ? " CABLE" : " CABLES"));
        count->set_font_family(forge::design::type::mono);
        count->set_font_size(10.5f);
        count->set_text_color(color::text_faint);
        count->flex().flex_shrink = 0;
        header->add_child(std::move(count));
        header->set_border_radius(7);
        headings_.push_back({header.get(), group.role});
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
                note_block->flex().direction = FlexDirection::row;
                note_block->flex().dim_width = {100, pulp::view::DimensionUnit::percent};
                note_block->flex().flex_shrink = 0;
                note_block->flex().padding_left = 24;
                note_block->flex().padding_bottom = 6;

                // A rule down the side, so the aside reads as a quotation
                // about the role rather than as the role's first cable. It
                // stretches to whatever the text beside it needs, which is
                // what the default cross-axis stretch already gives.
                auto edge = std::make_unique<View>();
                edge->flex().preferred_width = 1;
                edge->flex().flex_shrink = 0;
                edge->set_background_color(color::line);
                note_block->add_child(std::move(edge));

                auto note_text = std::make_unique<View>();
                note_text->flex().direction = FlexDirection::column;
                note_text->flex().flex_grow = 1;
                note_text->flex().padding_left = 11;
                for (const auto& piece :
                     wrap(primer, columns_for(bounds().width, kCharWidth, 12.0f))) {
                    auto note = std::make_unique<Label>(piece);
                    note->set_font_family(forge::design::type::display);
                    note->set_font_size(12.0f);
                    note->set_text_color(color::text_muted);
                    note->flex().dim_width = {100, pulp::view::DimensionUnit::percent};
                    note->flex().flex_shrink = 0;
                    note_text->add_child(std::move(note));
                }
                note_block->add_child(std::move(note_text));
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
        row->flex().padding_top = 5;
        row->flex().padding_bottom = 5;
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
        // Tuned to the monospaced face the wiring is set in: its ink sits
        // higher in the line box than the reading face does, and a dot placed
        // for the reading face lands under the wiring rather than beside it.
        dot->flex().margin_top = 7;
        dot->set_background_color(pulp::canvas::Color::rgba8(
            static_cast<std::uint8_t>((rgb >> 16) & 0xFF),
            static_cast<std::uint8_t>((rgb >> 8) & 0xFF),
            static_cast<std::uint8_t>(rgb & 0xFF)));
        dot->set_border_radius(4);
        row->add_child(std::move(dot));

        // The wiring, monospaced and in the strong ink: a jack name is a label
        // off a panel, and setting it in the same face and grey as the prose
        // beneath made the two indistinguishable at a glance.
        for (const auto& text : wrap(line_text(i), wiring_columns)) {
            auto label = std::make_unique<Label>(text);
            label->set_font_family(forge::design::type::mono);
            label->set_font_size(kWiringSize);
            label->set_text_color(color::text_strong);
            label->flex().dim_width = {100, pulp::view::DimensionUnit::percent};
            // No explicit height: a single-line Label measures itself
            // correctly. Pinning one only fights a measurement that is
            // already right.
            label->flex().flex_shrink = 0;
            row->add_child(std::move(label));
        }

        // The reason, in the reading face, indented under the cable it
        // explains -- a sentence about why someone made this choice, which is
        // a different kind of statement from the choice itself.
        if (const auto why = why_text(i); !why.empty()) {
            auto why_block = std::make_unique<View>();
            why_block->flex().direction = FlexDirection::column;
            why_block->flex().dim_width = {100, pulp::view::DimensionUnit::percent};
            why_block->flex().flex_shrink = 0;
            why_block->flex().padding_left = kWhyIndent;
            why_block->flex().padding_top = 3;
            for (const auto& text : wrap(why, why_columns)) {
                auto label = std::make_unique<Label>(text);
                label->set_font_family(forge::design::type::display);
                label->set_font_size(kProseSize);
                label->set_text_color(color::text_muted);
                label->flex().dim_width = {100, pulp::view::DimensionUnit::percent};
                label->flex().flex_shrink = 0;
                why_block->add_child(std::move(label));
            }
            row->add_child(std::move(why_block));
        }

        rows_[i] = row.get();
        add_child(std::move(row));
        }
    }
    // The views the hover was painted on are gone; paint it onto the new ones.
    apply_hover_styles();
    request_repaint();
}

const View* PatchExplanation::heading_for(SignalRole role) const {
    for (const auto& h : headings_)
        if (h.role == role) return h.view;
    return nullptr;
}

/// Paint the current hover onto whatever rows and headings exist now.
///
/// Called after a rebuild as well as from the two setters, because a rebuild
/// throws away the very views the highlight was painted on: change depth while
/// pointing at a role and the fresh heading came back unlit, while the setter's
/// "you are already hovering this" early return meant pointing at it again did
/// nothing at all.
void PatchExplanation::apply_hover_styles() {
    const auto clear = pulp::canvas::Color::rgba8(0, 0, 0, 0);
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (!rows_[i]) continue;
        const bool on = hovered_ && *hovered_ == i;
        rows_[i]->set_background_color(on ? color::surface_raised : clear);
        rows_[i]->request_repaint();
    }
    for (const auto& h : headings_) {
        if (!h.view) continue;
        const bool on = hovered_role_ && *hovered_role_ == h.role;
        h.view->set_background_color(on ? color::surface_raised : clear);
        h.view->request_repaint();
    }
}

void PatchExplanation::hover_line(std::optional<std::size_t> index) {
    if (index && *index >= connections_.size()) index.reset();
    // A cable and a role are two readings of the same rack, and holding both
    // would light one role plus one stray wire outside it.
    if (index) hover_role(std::nullopt);
    if (index == hovered_) return;
    hovered_ = index;
    apply_hover_styles();
    if (on_hover) on_hover(hovered_);
}

void PatchExplanation::hover_role(std::optional<SignalRole> role) {
    if (role && !heading_for(*role)) role.reset();
    if (role) hover_line(std::nullopt);
    if (role == hovered_role_) return;
    hovered_role_ = role;
    apply_hover_styles();
    if (on_role_hover) on_role_hover(hovered_role_);
}

void PatchExplanation::on_hover_move(pulp::view::Point local_pos) {
    // Rows and headings are laid out as a stack, so the pointer's y alone says
    // which one it is over -- and a point in the gap between two of them is
    // over neither, which is the honest answer.
    for (const auto& h : headings_) {
        if (!h.view) continue;
        const auto b = h.view->bounds();
        if (local_pos.y >= b.y && local_pos.y < b.y + b.height) {
            hover_role(h.role);
            return;
        }
    }
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (!rows_[i]) continue;
        const auto b = rows_[i]->bounds();
        if (local_pos.y >= b.y && local_pos.y < b.y + b.height) {
            hover_line(i);
            return;
        }
    }
    hover_line(std::nullopt);
    hover_role(std::nullopt);
}

void PatchExplanation::on_mouse_leave() {
    hover_line(std::nullopt);
    hover_role(std::nullopt);
}

}  // namespace forge_modular
