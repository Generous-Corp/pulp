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

/// How wide the panel in this artwork is, in HP, or 0 when it does not say.
///
/// A panel SVG is drawn at the true size of the thing it is a picture of --
/// every Eurorack panel is 128.5 mm tall, and its width is its HP -- so the
/// artwork knows a module's width even when the patch, the plugin manifest and
/// the port map all fail to. Read from the root tag's own aspect rather than
/// from either dimension alone, so it does not matter whether a vendor wrote
/// their panel in millimetres, in points, or in bare user units.
///
/// Exposed so a test can put a real vendor's header in and get the width its
/// module actually is.
int panel_hp_from_artwork(const std::string& svg);

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
    void set_panel_directory(std::string dir) {
        panel_dir_ = std::move(dir);
        resolve_panel_widths();
    }

    /// Is there artwork for this module, ours or the vendor's? Exposed so a
    /// test can ask whether a panel will be DRAWN rather than infer it from
    /// pixels -- a blank slab and a dark panel look alike in a comparison.
    /// How many knobs one of OUR modules declares, and how wide the widest
    /// is in millimetres. Exposed so a test can assert that the manifest is
    /// being read and its SIZES honoured -- a preview that drew every control
    /// at one size would look plausible and be wrong about every trimpot.
    std::pair<std::size_t, float> knob_summary(const std::string& model) const {
        const auto& k = module_knobs(model);
        float widest = 0.0f;
        for (const auto& one : k) widest = std::max(widest, one.diameter_mm);
        return {k.size(), widest};
    }

    bool has_artwork_for(const std::string& plugin,
                         const std::string& model) const {
        static const std::string kOurs = "ForgeModular";
        return !(plugin == kOurs ? panel_svg(model) : vendor_svg(plugin, model))
                    .empty();
    }

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

    // ── Getting close enough to read it ─────────────────────────────────────
    //
    // A 39-module patch fits, and at the scale it fits nothing on a panel is
    // legible: the whole point of drawing the rack rather than listing it is
    // lost at the exact size where the rack gets interesting. The bindings are
    // VCV Rack's own, so somebody who has used Rack already knows them --
    // Cmd/Ctrl with plus or minus zooms, Cmd/Ctrl with 0 goes back to the fit,
    // a two-finger drag pans -- plus pinch, which is the gesture a trackpad
    // offers for this and the one the platform actually delivers today. See
    // on_key_event for which of these currently reach the app.

    /// Where the camera is. Always within bounds -- every setter clamps.
    const RackView& view() const { return view_; }
    /// Move the camera. Clamped against the rack currently on screen, so a
    /// caller cannot install a pan that puts the rack off the side.
    void set_view(RackView v);
    /// One step in or out. Returns whether anything moved, so a caller can
    /// tell "zoomed" from "already as far in as it goes" rather than assuming.
    bool zoom_in();
    bool zoom_out();
    /// Back to the fit, centred. Returns whether anything moved.
    bool reset_view();
    /// Drag the rack by a delta in view points.
    bool pan_by(float dx, float dy);

    /// The zoom/pan bindings, for a caller that routes keys here. Returns
    /// whether the key was one of them, so an unhandled key still reaches
    /// whatever is behind the preview.
    ///
    /// The platform modifier with 0 goes back to the fit, and with plus or
    /// minus zooms. Only the first of those arrives on macOS today: the host's
    /// virtual-keycode table names the letters, the digits, `;` and `'`, and
    /// nothing else, so `=` and `-` reach the app as KeyCode::unknown. The
    /// bindings are here and correct for the moment that table carries them;
    /// until then pinch is the zoom that works.
    bool on_key_event(const pulp::view::KeyEvent& event) override;

    /// Pinch zooms, about the fit rather than about the fingers.
    ///
    /// This is the zoom path that actually reaches the preview on a trackpad,
    /// and it is the gesture somebody reaches for first. The macOS window host
    /// delivers it to the deepest view under the fingers; the PLUGIN view host
    /// does not implement magnify at all, so in a DAW the preview zooms by
    /// keyboard and pans by trackpad.
    void on_gesture_event(const pulp::view::GestureEvent& event) override;

    /// A two-finger drag arrives as a wheel event; it pans rather than
    /// scrolling an enclosing view, which is why this claims the wheel.
    bool wants_wheel_scroll() const override { return true; }
    void on_mouse_event(const pulp::view::MouseEvent& event) override;

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
    RackView view_;

    /// Clamp a candidate camera against the rack as it is on screen now.
    RackView clamped(RackView v) const;

    const RackModule* find(const std::string& id) const;

    /// The panel SVG for a module, loaded once and kept. Empty when there is
    /// none, in which case the plain face is drawn instead -- an honest
    /// placeholder beats a borrowed panel that misidentifies the module.
    const std::string& panel_svg(const std::string& slug) const;

    std::string panel_dir_;
    /// Artwork for modules we did not make, keyed "plugin/model". Kept apart
    /// from `panel_cache_` because slugs collide across plugins.
    mutable std::map<std::string, std::string> vendor_cache_;

    /// Read `<dir>/<slug>.svg`, preferring the light face Rack shows.
    static std::string read_panel(const std::string& dir, const std::string& slug);

    /// Give every module whose width nobody measured the width its own artwork
    /// is drawn at. Runs whenever either half -- the rack or the directory the
    /// panels are read from -- arrives, because the two arrive in either order.
    void resolve_panel_widths();
    /// One knob, in panel millimetres, as the module's manifest declares it.
    /// One control. Ours arrive in millimetres from a manifest; a vendor's
    /// arrive in panel points from CARTOG, and converting either into the
    /// other's units would be a second place for the two to disagree.
    struct KnobSpec {
        float x_mm = 0, y_mm = 0, diameter_mm = 0;
        bool already_points = false;
    };
    /// The knobs of one of OUR modules, cached per model.
    const std::vector<KnobSpec>& module_knobs(const std::string& model) const;
    /// Paint them over the panel, which is where Rack composites its own.
    /// Screens and lamps from the scan: touch plates, level meters, readouts.
    void draw_screens(pulp::canvas::Canvas& canvas, const PanelBox& panel,
                      const RackModule& mod, float scale) const;

    /// Every jack the scan recorded, patched or not.
    void draw_jacks(pulp::canvas::Canvas& canvas, const PanelBox& panel,
                    const RackModule& mod, float scale) const;

    void draw_knobs(pulp::canvas::Canvas& canvas, const PanelBox& panel,
                    const RackModule& mod, float scale) const;
    mutable std::map<std::string, std::vector<KnobSpec>> knob_cache_;

    /// Artwork for somebody else's module, from wherever Rack keeps it.
    const std::string& vendor_svg(const std::string& plugin,
                                  const std::string& model) const;
    mutable std::map<std::string, std::string> panel_cache_;
};

}  // namespace forge_modular
