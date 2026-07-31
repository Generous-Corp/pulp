#pragma once

// The patch, in words, wired to the patch in pictures.
//
// One line per cable. Hovering a line lights its cable in the rack above and
// dims the rest; that pairing is the product. A list of connections on its own
// is a netlist, and a rack on its own is a photograph -- it is the link between
// them that teaches anything.
//
// How much each line says is the depth setting: Terse gives the connection,
// Standard adds the reasoning, Learning adds the aside about why the role
// matters at all.

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

    /// What one line reads as at the current depth. Pure, so the wording is
    /// asserted without a render.
    std::string line_text(std::size_t index) const;

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
    std::optional<std::size_t> hovered() const { return hovered_; }

private:
    void rebuild();
    std::string port_label(const std::string& module_id,
                           const std::string& port_id) const;

    std::vector<Connection> connections_;
    std::vector<RackModule> modules_;
    std::vector<pulp::view::View*> rows_;
    ExplainDepth depth_ = ExplainDepth::standard;
    float wrapped_at_ = -1.0f;
    bool rewrap_pending_ = false;
    std::optional<std::size_t> hovered_;
};

}  // namespace forge_modular
