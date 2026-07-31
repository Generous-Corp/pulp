#pragma once

// The rack, drawn.
//
// Panels composited at their true widths with cables hanging between real jack
// centres. All the arithmetic lives in rack_layout.hpp, which is asserted
// exactly; this file only paints what that returns.
//
// Hovering a line of the explanation lights its cable. That is the whole reason
// the preview is worth drawing rather than listing the connections as text: the
// sentence "the LFO is what makes it move" means nothing until the cable it
// refers to is the one glowing.

#include "forge/rack_layout.hpp"

#include <pulp/view/view.hpp>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace forge_modular {

/// How close a pointer has to be to a cable to count as pointing at it, in
/// view points. Generous enough to grab a 4-point cable without a steady hand,
/// small enough that the gap between two cables is still a gap.
inline constexpr float kCableGrabPoints = 9.0f;

class RackPreview : public pulp::view::View {
public:
    void set_rack(std::vector<RackModule> modules,
                  std::vector<Connection> connections);

    /// Where the generated panel SVGs live, so a panel is drawn as the module
    /// actually looks rather than as a labelled rectangle.
    ///
    /// The emitter already writes one per module -- the preview simply was not
    /// reading them, which is why a finished module showed an empty box with
    /// its name on it and none of the knobs it had just been given.
    void set_panel_directory(std::string dir) { panel_dir_ = std::move(dir); }

    const std::vector<RackModule>& modules() const { return modules_; }
    const std::vector<Connection>& connections() const { return connections_; }

    /// Light one cable, by its index in `connections()`. No value dims nothing
    /// -- with no hover every cable is drawn at full strength, because an
    /// unhovered rack is the normal state and must not look faded.
    void set_highlight(std::optional<std::size_t> index);
    std::optional<std::size_t> highlight() const { return highlight_; }

    /// Highlight whichever cable a role owns, for hovering a role group rather
    /// than a single line.
    void highlight_role(std::optional<SignalRole> role);

    /// How far the patch has been wired in, 0..1. Cables reach across as the
    /// build proceeds rather than appearing all at once.
    void set_progress(float t);
    float progress() const { return progress_; }

    /// The layout for a given viewport. Exposed so a test can assert what will
    /// be painted without painting it.
    RackLayout layout_for(float width, float height) const;

    /// How strongly a cable is drawn: 1 when it is the highlight or nothing is
    /// highlighted, dimmed otherwise.
    float cable_alpha(std::size_t index) const;

    /// Which cable is under a point in this view, if any.
    ///
    /// Pure given the current bounds, so a test can assert what a pointer
    /// would find without a window, a host or a mouse. Nearest wins, and only
    /// within `kCableGrabPoints` -- pointing at the empty middle of the rack
    /// must find NOTHING, or every cable lights whenever the pointer is over
    /// the stage at all.
    std::optional<std::size_t> cable_at(float x, float y) const;

    /// Pointing at a cable in the rack lights its line in the explanation.
    ///
    /// The other direction already worked, and only that direction. Hovering
    /// a sentence lit its cable, but the rack was the half a person actually
    /// looks at, and pointing at the thing you can see taught you nothing.
    std::function<void(std::optional<std::size_t>)> on_cable_hover;

    void on_hover_move(pulp::view::Point local_pos) override;
    void on_mouse_leave() override;

    void paint(pulp::canvas::Canvas& canvas) override;

private:
    std::vector<RackModule> modules_;
    std::vector<Connection> connections_;
    std::optional<std::size_t> highlight_;
    std::optional<SignalRole> highlight_role_;
    float progress_ = 1.0f;

    const RackModule* find(const std::string& id) const;

    /// The panel SVG for a module, loaded once and kept. Empty when there is
    /// none, in which case the plain face is drawn instead -- an honest
    /// placeholder beats a borrowed panel that misidentifies the module.
    const std::string& panel_svg(const std::string& slug) const;

    std::string panel_dir_;
    mutable std::map<std::string, std::string> panel_cache_;
};

}  // namespace forge_modular
