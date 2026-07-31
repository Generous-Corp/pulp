#pragma once

// The patch, in words, wired to the patch in pictures.
//
// One line per cable. Hovering a line lights its cable in the rack above and
// dims the rest; that pairing is the product. A list of connections on its own
// is a netlist, and a rack on its own is a photograph -- it is the link between
// them that teaches anything.
//
// The wiring and the reasoning are set differently and stacked, never run
// together into one sentence. A connection is a fact about jacks and reads as
// one -- monospaced, in the strong ink, the way the panels silkscreen it. A
// reason is prose about intent and reads as one, indented under the cable it
// belongs to. Joined with a dash into a single wrapped paragraph, as they once
// were, the eye cannot tell which half is the patch and which half is the
// argument, and the list stops being scannable.
//
// How much each line says is the depth setting: Terse gives the connection,
// Standard adds the reasoning, Learning adds the aside about why the role
// matters at all.
//
// Two ways in, not one. A cable is the small unit and a ROLE is the large one:
// pointing at a role's heading lights every cable that carries it, so "what is
// the audio path" is answered as a shape in the rack rather than as three
// separate sentences.

#include "forge/rack_layout.hpp"
#include "forge/rack_preview.hpp"

#include <pulp/view/view.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace forge_modular {

/// How much a line explains. Mirrors the Build title bar's tabs.
enum class ExplainDepth { terse, standard, learning };

class PatchExplanation : public pulp::view::View {
public:
    /// Light a cable when a line is pointed at. No value means nothing is.
    std::function<void(std::optional<std::size_t>)> on_hover;

    /// Light every cable of a role when its heading is pointed at. No value
    /// means no role is.
    std::function<void(std::optional<SignalRole>)> on_role_hover;

    void set_connections(std::vector<Connection> connections,
                         std::vector<RackModule> modules);
    void set_depth(ExplainDepth depth);

    /// Re-wraps when the pane's width changes. Without this the explanation
    /// keeps whatever wrap it was built with, which is the wrong one whenever
    /// it is built before it is measured -- i.e. always.
    void on_resized() override;

    /// The row drawn for one connection, or null if there is none.
    ///
    /// Exposed for tests: the view also holds role HEADINGS, which are a row of
    /// two labels side by side and so break any assertion written for a stack
    /// of wrapped lines. A test that means "the cable rows" should say so
    /// rather than walking every child and hoping.
    const pulp::view::View* row_for(std::size_t connection) const {
        return connection < rows_.size() ? rows_[connection] : nullptr;
    }
    ExplainDepth depth() const { return depth_; }

    /// The heading drawn for a role, or null when the patch has no cable
    /// carrying it. Exposed so a test can point at the same rectangle a mouse
    /// would.
    const pulp::view::View* heading_for(SignalRole role) const;

    /// The wiring for one cable: what is plugged into what, at every depth.
    /// Pure, so the wording is asserted without a render.
    std::string line_text(std::size_t index) const;

    /// The reasoning for one cable, or empty when the depth does not show it
    /// or the cable came with none. Separate from `line_text` because the two
    /// are set differently and are two different kinds of claim.
    std::string why_text(std::size_t index) const;

    /// Break text into display lines at a word boundary.
    ///
    /// Explicit rather than leaning on Label's soft-wrap, which draws its
    /// wrapped lines on top of each other: the second line lands a few points
    /// below the first regardless of line height or row height, so a wrapped
    /// explanation is unreadable. Emitting one single-line Label per line
    /// sidesteps that and is exactly assertable.
    static std::vector<std::string> wrap(const std::string& text,
                                         std::size_t columns);

    std::size_t line_count() const { return connections_.size(); }

    /// Point at a line, as a hover would. Named so a test drives the same path
    /// the mouse does rather than a private shortcut.
    void hover_line(std::optional<std::size_t> index);

    /// Point at a role's heading. Lights every cable that role owns, and lets
    /// go of any single cable that was lit -- the two readings are exclusive,
    /// or the rack shows one role plus one stray wire and means neither.
    void hover_role(std::optional<SignalRole> role);

    /// Route the pointer to whichever row or heading it is over.
    ///
    /// Without these the pairing only ran one way: the rack lit a line, but
    /// pointing at a line lit nothing, because nothing ever called
    /// hover_line() from a mouse.
    void on_hover_move(pulp::view::Point local_pos) override;
    void on_mouse_leave() override;

    /// Re-wrap if a resize asked for one and there was no loop to defer onto.
    ///
    /// A hosted plugin has no dispatcher of its own, so the deferred re-wrap
    /// never ran and the rows stayed laid out for the width the view was
    /// first built at. Driven from the shell's poll, which is neither a
    /// layout pass nor an event delivery.
    void apply_pending_rewrap();
    std::optional<std::size_t> hovered() const { return hovered_; }
    std::optional<SignalRole> hovered_role() const { return hovered_role_; }

private:
    void rebuild();
    /// Paint the current hover onto the rows and headings that exist now.
    void apply_hover_styles();
    std::string port_label(const std::string& module_id,
                           const std::string& port_id) const;

    struct Heading {
        pulp::view::View* view = nullptr;
        SignalRole role = SignalRole::audio;
    };

    std::vector<Connection> connections_;
    std::vector<RackModule> modules_;
    std::vector<pulp::view::View*> rows_;
    std::vector<Heading> headings_;
    ExplainDepth depth_ = ExplainDepth::standard;
    float wrapped_at_ = -1.0f;
    bool rewrap_pending_ = false;
    /// A re-wrap was asked for and could not be scheduled. Applied at the
    /// next poll rather than dropped.
    bool stale_wrap_ = false;
    std::optional<std::size_t> hovered_;
    std::optional<SignalRole> hovered_role_;
};

}  // namespace forge_modular
