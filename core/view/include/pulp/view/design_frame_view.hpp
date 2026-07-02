#pragma once

#include <pulp/view/svg_fragment.hpp>
#include <pulp/view/view.hpp>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulp::view {

// One interactive element overlaid on a faithfully-rendered design frame. The
// element list is TYPED and supplied by the importer (source-side semantics) —
// DesignFrameView does NOT guess from the SVG. Bounds/coords are in the SVG's own
// coordinate space. Real behavior comes from source metadata, not SVG structure.
struct DesignFrameElement {
    // `knob` is SVG-patch (rotates its needle path in the SVG). `text_field` /
    // `dropdown` / `tab_group` / `stepper` are NATIVE-OVERLAY: an opaque child
    // widget is positioned over the element's rect and replaces that baked SVG
    // region. `stepper` is a header value cycled in place by `< >` chevrons.
    // `momentary` is a press/release primitive (keys, pads, drum triggers,
    // sustain, transport): on_gesture_begin(i)=press, on_gesture_end(i)=release;
    // set_element_value(i,1/0) lights it via a NATIVE overlay, not SVG mutation.
    // `swap` is a SWAP-LINK button: clicking its rect calls set_active_frame
    // (target_frame) — the importer's `swap` link (e.g. a mode toggle). It does
    // not light or emit notes; it changes which frame the view renders.
    // `action` is a momentary command button: clicking its rect fires
    // on_action(action) — an in-design control (octave −/+, velocity, sustain,
    // pitch-bend preset). It does not light, emit notes, or swap frames; it
    // names an action the consumer maps to its own state.
    // `value_label` is a live read-only text overlay: it paints `text` over its
    // rect (replacing a baked readout glyph that build suppresses), updated via
    // set_element_text — e.g. an "OCTAVE C2" value that must track state.
    // `fader` is SVG-patch like `knob` but TRANSLATES its thumb element
    // (needle_d) by value over the track. Orientation follows the track shape:
    // a wider-than-tall rect (w>h) is a HORIZONTAL slider (value 0→left x, 1→
    // right x+w; cx = baked center); otherwise vertical (value 1→top y, 0→bottom
    // y+h; cy = baked center). `toggle` is a click-to-flip button that tints its rect
    // (bg_color, value>=0.5=on) over the baked chrome so the label shows through.
    // A toggle with `needle_d` set is a SWITCH: the dot (needle_d) sits at e.cx in
    // its OFF state and slides to the mirror across the pill center (x + w/2) when
    // on, in addition to the tint — so the design's rest state is preserved.
    // `xy_pad` is SVG-patch like `fader` but in 2D: dragging inside its rect
    // [x,y,w,h] moves the puck element (needle_d) to follow — `value` is the X
    // position (0→left, 1→right), `value_y` the Y (0→top, 1→bottom). cx/cy = the
    // puck's baked center.
    // `custom` is a registered native control: the overlay is built by a factory
    // looked up under `factory_id` in the design-control registry
    // (register_design_control_factory). If no factory is registered the element
    // renders inert (the baked SVG underneath always shows) and the importer
    // diagnoses it — a custom control never blanks or silently mis-renders.
    enum class Kind { knob, fader, toggle, text_field, dropdown, tab_group,
                      stepper, momentary, swap, action, value_label, xy_pad,
                      custom };

    Kind kind = Kind::knob;

    // ── knob (SVG-patch) ─────────────────────────────────────────────────
    float cx = 0.0f;            ///< pivot / hit center, SVG coords
    float cy = 0.0f;
    float hit_radius = 0.0f;    ///< click-target radius, SVG coords
    // Knob: the `d` of its needle path in the source SVG. Dragging rotates that
    // path around (cx, cy) by the value angle and re-renders — only the needle
    // moves; the rest of the chrome stays pixel-exact.
    std::string needle_d;
    float value = 0.5f;        ///< 0..1  (xy_pad: the X axis)
    float value_y = 0.5f;      ///< 0..1  (xy_pad: the Y axis, 0=top)
    /// toggle only: press-flash command button (sample next/prev/random, dice).
    /// Lights with the tint on press and clears on release, instead of the
    /// default sticky on/off flip — the right feel for a momentary command.
    bool flash = false;
    /// When false, the element is bypassed/disabled: hit-testing skips it (it
    /// cannot be hovered, dragged, clicked, or gesture) and a subclass/painter
    /// may dim it (see DesignFrameView::element_enabled). Defaults enabled.
    bool enabled = true;

    // ── overlay controls (text_field / dropdown / tab_group / stepper) ────
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;  ///< element rect, SVG coords
    std::string placeholder;                ///< text_field
    std::vector<std::string> options;       ///< dropdown options / tab labels
    int selected_index = 0;                 ///< dropdown / tab selection
    /// text_field: the design's own field background ("#RRGGBB"). When set, the
    /// overlay paints this exact color so it blends seamlessly with the baked
    /// box (the overlay is inset past the leading icon, which stays visible).
    /// Empty → the default dark field color.
    std::string bg_color;

    // ── momentary (keys / pads) ──────────────────────────────────────────
    /// Raw parsed number: typing keys = relative semitone (0–15), piano keys =
    /// absolute MIDI note. -1 = unset. Consumers map by this, NOT positional
    /// index (a re-export may reorder elements). Uses the x/y/w/h rect as the
    /// hit-region. `value` doubles as the pressed/lit flag (0 or 1).
    int note = -1;
    /// View scope for per-view keyboards (e.g. typing vs piano). hit_element only
    /// tests momentary elements in the active view group (see set_active_view_group).
    /// -1 = always active (ungrouped).
    int view_group = -1;

    // ── swap (swap-link button) ──────────────────────────────────────────
    /// For Kind::swap: the frame index to activate when this button is clicked
    /// (the `swap` link target). -1 = unset.
    int target_frame = -1;

    // ── action (command button) ──────────────────────────────────────────
    /// For Kind::action: the action id fired (on_action) when clicked, e.g.
    /// "octave_up". Empty = unset. Uses the x/y/w/h rect as the hit-region.
    std::string action;

    // ── value_label (live readout) ───────────────────────────────────────
    /// For Kind::value_label: the live display string painted over the rect
    /// (right-aligned to match the design's baked readouts). Updated via
    /// set_element_text.
    std::string text;
    /// value_label: left-align the text in the rect instead of right-aligning.
    /// Use for a readout that follows a fixed label (e.g. "PITCH BEND <value>")
    /// where a variable-width value must grow rightward into empty space, not
    /// leftward over the label.
    bool value_left_align = false;

    // ── custom (registered native control) ───────────────────────────────
    /// For Kind::custom: the id the overlay factory is registered under
    /// (register_design_control_factory). Empty/unregistered → inert render.
    std::string factory_id;
    /// For Kind::custom: opaque props handed to the factory (typically a JSON
    /// string the factory parses). Pulp does not interpret these.
    std::string custom_props;

    // ── provenance (design-import) ───────────────────────────────────────
    /// Source node id this overlay came from (e.g. a Figma node id like
    /// "1273:33424"), copied from the IR's IRInteractiveElement during
    /// materialization. Empty when the element wasn't lowered from a design
    /// import. Lets a tool (the inspector's Wiring lens) map a live control back
    /// to its design node — to flag "not wired up" and fetch that exact frame.
    std::string source_node_id;

    // ── host-parameter binding (foreign-host embed) ──────────────────────
    /// Optional host-parameter binding id for this control. When non-empty, a
    /// foreign-host binder (e.g. the embed shim's string-key↔host bridge) maps
    /// this element to the host parameter named by this key: it forwards the
    /// element's on_element_changed / gesture begin+end to the host, and routes
    /// host→UI pushes back by matching the key. Vendor-neutral — just an opaque
    /// id the consumer resolves against its own parameter system (a host param
    /// id, a CLAP param key, an AU parameter address, etc.); Pulp does not
    /// interpret it. A hand-built
    /// (non-imported) view sets this per control to declare its binding; an
    /// imported view may leave it empty and let the binder fall back to
    /// source_node_id. Empty = this element is not bound to a host parameter.
    std::string param_key;
};

// ── Custom-control factory registry ──────────────────────────────────────────
// Runtime name→View table for genuinely novel controls in imported designs. A
// host or shared package registers a factory under an id; the importer emits a
// Kind::custom element carrying that id; DesignFrameView builds the overlay by
// looking the factory up.

// Geometry + opaque props handed to a custom-control factory so it can build and
// bind its View. Panel coordinates (the same space DesignFrameView positions
// overlays in).
struct DesignControlContext {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    std::string factory_id;
    std::string props;            ///< opaque (typically JSON); Pulp does not parse it
    std::string source_node_id;
    float default_value = 0.5f;
};

// Builds a native overlay View for a Kind::custom element. Returns the View
// (ownership moves to the DesignFrameView) or nullptr to render inert.
using DesignControlFactory =
    std::function<std::unique_ptr<View>(const DesignControlContext&)>;

// Register / query a custom-control factory by id. UI-THREAD-ONLY: registration
// happens at host startup and lookup at overlay build, both on the UI thread, so
// the registry needs no locking. Re-registering an id replaces the prior factory.
//
// LIFETIME: the factory is stored for the process lifetime (until replaced or
// clear_design_control_factories()). It must remain callable for that whole time,
// so do NOT capture by reference/pointer any state that outlives the registrant —
// register a free function or a lambda over owned/static state. A factory
// capturing stack locals is a latent use-after-free the moment those locals go
// out of scope. (The factory itself runs on the UI thread, synchronously, from
// DesignFrameView::build_overlays.)
void register_design_control_factory(std::string factory_id,
                                     DesignControlFactory factory);
bool has_design_control_factory(const std::string& factory_id);
// Test/teardown helper: drop all registered factories.
void clear_design_control_factories();

// Remove the first <rect> in `svg` whose x/y/width/height match (within `tol`)
// the given box, returning true if one was erased. Used to suppress a design's
// BAKED selected-tab highlight so the live overlay's pill is the only one shown
// (no double-pill when the selection moves). Pure geometry — no per-design data.
bool suppress_svg_rect(std::string& svg, float x, float y, float w, float h,
                       float tol = 2.0f);

// Remove a filtered group (`<g filter="url(#...)">…</g>`) whose first drawn
// coordinate falls inside the box [x, x+w] × [y, y+h], returning true if one was
// erased. Figma bakes the SELECTED tab's digit with a soft glow (a large-blur
// drop-shadow filter); the live pill moves on click but that baked glow stays
// stuck on the originally-selected digit. Removing the filtered group at the
// originally-selected cell lets the live overlay own the digit cleanly. Pure
// geometry — no per-design data.
bool suppress_svg_glow_at(std::string& svg, float x, float y, float w, float h);

// Remove the first standalone `<path …/>` whose first drawn coordinate falls
// inside the box [x, x+w] × [y, y+h], returning true if one was erased. Used to
// drop a BAKED tab digit glyph so the live DesignTabGroup is the sole renderer
// of the digits (no faint "doubled" glyph where the live label paints over the
// baked one). Pure geometry — no per-design data.
bool suppress_svg_glyph_at(std::string& svg, float x, float y, float w, float h);

// Renders a design's own SVG document pixel-faithfully via Canvas::draw_svg
// (Skia SkSVGDOM), cropped to its panel, and overlays native interaction from a
// typed element list. This is the faithful-vector design-import lane's view: the
// importer materializes one of these per imported frame.
//
// Each drag patches only the dragged knob's needle in the SVG and repaints.
// Canvas::draw_svg parses the SVG into an SkSVGDOM keyed on the document bytes
// (process-wide SvgDomCache), so a repaint of an UNCHANGED document — static
// chrome, or a frame whose only motion is a native-overlay child — reuses the
// parsed DOM instead of rebuilding it. A render-patched document (rotated knob
// needle, translated fader/xy_pad thumb, toggle-switch slide) is a different
// string, so it misses the cache and rebuilds, keeping the dragged element
// live. The rasterized output is byte-identical to the uncached path; only the
// parse step is skipped on a hit.
class DesignFrameView : public View {
public:
    // `svg` is the full SVG document. The panel (the design body the window
    // should show edge-to-edge) is auto-detected as the largest <rect>; pass a
    // positive panel_* to override. `elements` are the interactive overlays.
    DesignFrameView(std::string svg, std::vector<DesignFrameElement> elements,
                    float panel_x = -1, float panel_y = -1,
                    float panel_w = -1, float panel_h = -1);

    int element_count() const { return static_cast<int>(elements_.size()); }
    float panel_width() const { return panel_w_; }
    float panel_height() const { return panel_h_; }
    // Kind of element `i` (knob / dropdown / tab_group / stepper / text_field),
    // or knob if out of range. Lets a binder treat knobs as continuous params
    // and dropdown/tab/stepper as normalized-index "choice" params.
    DesignFrameElement::Kind element_kind(int i) const;
    // Raw note number of momentary element `i` (typing = relative semitone 0–15,
    // piano = absolute MIDI), or -1. Consumers map by this, not positional index.
    int element_note(int i) const {
        return (i >= 0 && i < static_cast<int>(elements_.size())) ? elements_[i].note : -1;
    }
    // Panel-coord rect (x, y, w, h) of element `i`, or {0,0,0,0}. Lets a subclass
    // position its own overlay relative to an element (e.g. the piano C-labels
    // drawn under the C keys, which shift as the window moves).
    Rect element_rect(int i) const;
    // The `action` id of element `i` (for Kind::action command buttons and the
    // readout tag of Kind::value_label), or empty. Lets a consumer route by id.
    const std::string& element_action(int i) const {
        static const std::string kEmpty;
        return (i >= 0 && i < static_cast<int>(elements_.size())) ? elements_[i].action : kEmpty;
    }
    // The Y axis (0=top, 1=bottom) of an xy_pad element `i`, or 0.5. The X axis is
    // element_value(i).
    float element_value_y(int i) const {
        return (i >= 0 && i < static_cast<int>(elements_.size())) ? elements_[i].value_y : 0.5f;
    }
    // The frame index a Kind::swap element activates on click, or -1 if unset.
    int element_target_frame(int i) const {
        return (i >= 0 && i < static_cast<int>(elements_.size())) ? elements_[i].target_frame : -1;
    }
    // The readout string of a Kind::value_label element `i`, or empty.
    const std::string& element_text(int i) const {
        static const std::string kEmpty;
        return (i >= 0 && i < static_cast<int>(elements_.size())) ? elements_[i].text : kEmpty;
    }
    // Whether a Kind::value_label element `i` left-aligns its readout.
    bool element_left_align(int i) const {
        return (i >= 0 && i < static_cast<int>(elements_.size())) && elements_[i].value_left_align;
    }
    // The design-source node id of element `i` (e.g. a Figma node id), or empty
    // when the element wasn't lowered from a design import. The inspector's
    // Wiring lens reads this to map a live control back to its design node.
    const std::string& element_source_node_id(int i) const {
        static const std::string kEmpty;
        return (i >= 0 && i < static_cast<int>(elements_.size())) ? elements_[i].source_node_id
                                                                  : kEmpty;
    }
    // The host-parameter binding key of element `i` (DesignFrameElement::param_key),
    // or empty when the element declares no binding. A foreign-host binder reads
    // this to wire the element to a host parameter; see element_for_param_key for
    // the host→UI reverse lookup.
    const std::string& element_param_key(int i) const {
        static const std::string kEmpty;
        return (i >= 0 && i < static_cast<int>(elements_.size())) ? elements_[i].param_key
                                                                  : kEmpty;
    }
    // Index of the first active-frame element whose param_key == `key`, or -1 if
    // none (a NULL/empty key never matches). The host→UI direction of the bind:
    // on automation/preset recall the binder looks up the element for a key and
    // pushes the value with set_element_value (silently, no echo). Linear scan —
    // the element count is a panel's worth of controls, not a hot path.
    int element_for_param_key(const std::string& key) const {
        if (key.empty()) return -1;
        for (int i = 0; i < static_cast<int>(elements_.size()); ++i)
            if (elements_[i].param_key == key) return i;
        return -1;
    }
    // Active view group for per-view momentary keyboards (e.g. typing=0, piano=1).
    // hit_element only tests momentary elements whose view_group is -1 or equals
    // this. Switching it releases any held momentary key (note-off) so no notes
    // stick across a mode change. -1 (default) = all groups active.
    void set_active_view_group(int group);
    int active_view_group() const { return active_view_group_; }

    // ── Multi-frame (swap) support ────────────────────────────────────────
    // A DesignFrameView can hold N alternate frames — each its own SVG, typed
    // overlay element list, and panel crop — and swap which one renders. This
    // is the importer's `swap` link target: a control whose job is to replace
    // the on-screen content with another frame (e.g. a piano⇄typing mode
    // toggle). set_active_frame swaps the rendered SVG, the overlay set, AND the
    // reported intrinsic size, then calls invalidate_layout() to REQUEST a
    // re-layout. Whether the surface actually resizes is up to the host: a host
    // that sizes to intrinsic_width()/height() follows the new frame; a
    // fixed-bounds host keeps its size and the new frame is fit (letterboxed)
    // into it — clicks still map correctly either way. add_frame returns the new
    // frame's index; frame 0 is the constructor's. Switching releases any held
    // momentary key (and subclasses can react via on_active_frame_changed).
    int add_frame(std::string svg, std::vector<DesignFrameElement> elements,
                  float panel_x = -1, float panel_y = -1,
                  float panel_w = -1, float panel_h = -1);
    void set_active_frame(int index);
    int active_frame() const { return active_frame_; }
    int frame_count() const { return static_cast<int>(frames_.size()); }
    // Normalized [0,1] value of element `i`, or -1 if out of range / not a
    // value-bearing control (text_field). For a knob this is its turn; for a
    // dropdown/tab_group/stepper it is the live selection mapped to
    // selected_index / max(1, option_count - 1). Reads the live overlay widget
    // when one exists. For tests/bindings.
    float element_value(int i) const;
    // Set element `i` from a normalized [0,1] value WITHOUT firing
    // on_element_changed (a host->view push: knob turn, or choice index =
    // round(v * (count-1)) applied to the live overlay widget silently). Use for
    // automation/preset application so it doesn't echo back to the host.
    void set_element_value(int i, float v);
    // Set the live text of a Kind::value_label element and repaint. No-op for
    // other kinds / out of range. Use for readouts that must track state
    // (octave / velocity / pitch-bend).
    void set_element_text(int i, std::string text);
    // The native-overlay child widget for element `i`, or nullptr (e.g. for a
    // knob, or out of range). For tests/bindings.
    View* overlay_widget(int i) const;

    // Fired when the USER changes an element (knob drag, dropdown/tab/stepper
    // select) — index + the new normalized value. NOT fired by set_element_value
    // (that's a programmatic host->view push). A foreign-host binder forwards
    // this to its parameter system. gesture begin/end bracket a knob drag so the
    // binder can group an undo step; choice controls fire one changed (no
    // gesture). All on the UI thread.
    std::function<void(int index, float value)> on_element_changed;
    std::function<void(int index)> on_gesture_begin;
    std::function<void(int index)> on_gesture_end;
    // Fired when a Kind::action command button is clicked — its `action` id. The
    // consumer (e.g. MusicalTypingKeyboard) maps the id to its own state
    // (octave/velocity/sustain/pitch-bend). UI thread.
    std::function<void(const std::string& action)> on_action;

    // ── Runtime host-parameter surface (the "port once, three hosts" path) ──
    // Beyond the bind-once path (a consumer wiring on_element_changed +
    // element_for_param_key), a DesignFrameView can bind DIRECTLY to the SDK's
    // framework-agnostic HostParamSurface (View::host_params()). This is what
    // lets one view run unchanged embedded in JUCE (APVTS-backed), embedded in
    // iPlug2 (IParams-backed), or native (StateStore-backed).
    //
    // Enable with route_changes_to_host_params(true): thereafter a user gesture
    // on a param_key-tagged element drives host_params() directly
    // (begin_gesture / set_param / end_gesture), and sync_from_host_params()
    // pulls current values + display text back at tick. on_element_changed still
    // fires as an additional observer, so existing consumers are unaffected. A
    // control whose param_key is empty or unknown to the surface is left to
    // local state (degrades exactly like a preview with a null surface). OFF by
    // default — the embed/binder path is unchanged.
    void route_changes_to_host_params(bool enable) { route_to_host_params_ = enable; }
    bool routes_changes_to_host_params() const { return route_to_host_params_; }

    // Host→UI snapshot, called once per tick (never from paint — see the
    // HostParamSurface call-context contract). For every active-frame element
    // with a non-empty param_key that host_params() resolves, pulls the current
    // normalized value into the element (silently, via set_element_value) and,
    // for Kind::value_label readouts whose `action` names a param key, pulls the
    // formatted display text (via set_element_text). No-op when host_params() is
    // null. This is the snapshot views paint from, so per-frame ABI/host calls
    // never happen inside paint().
    void sync_from_host_params();

    // Dynamically re-key element `i` to a new host-parameter key (paged racks,
    // tabbed effect slots). Updates the element's param_key, releases the old
    // binding, and fires on_param_key_changed so an owning key→index registry
    // (e.g. the embed facade's) can mark itself dirty and rebuild. The next
    // sync_from_host_params() re-pulls the element under its new key. No-op if
    // `i` is out of range or the key is unchanged.
    void set_element_param_key(int i, std::string key);

    // Fired by set_element_param_key after a successful re-key. A foreign-host
    // embed uses this to invalidate its cached key→index registry; a native
    // consumer can ignore it (sync_from_host_params resolves live). UI thread.
    std::function<void(int index, const std::string& key)> on_param_key_changed;

    // ── Host action/command channel ─────────────────────────────────────────
    // When enabled, a Kind::action button click is ALSO forwarded to
    // View::host_actions()->send_host_action(action, "{}") in addition to
    // on_action. Lets a view trigger structural host commands (insert/remove/
    // reorder rack slot, load preset) through the same framework-agnostic
    // channel the import lane uses. OFF by default. args_json is "{}" here;
    // richer payloads are the consumer's job via on_action.
    void route_actions_to_host(bool enable) { route_actions_to_host_ = enable; }
    bool routes_actions_to_host() const { return route_actions_to_host_; }

    // ── Per-element hover + enabled/bypass state ─────────────────────────────
    // DesignFrameView had no hover concept; every faithful port needs one
    // (hover affordances, EDIT overlays, bypass dimming). Hover is tracked by
    // hit-testing pointer moves (drive it with simulate_hover / the host's
    // mouse-move). One element is hovered at a time; a disabled element is never
    // hovered and never hit.
    //
    // The state is exposed for a subclass or painter to honor visually (e.g.
    // brighten the hovered element, desaturate a disabled one) — the base view
    // tracks state + interaction gating; pixel-level SVG restyling rides on the
    // fragment-handle primitive. on_element_hover fires on the UI thread
    // with the entered/exited index so a consumer can drive its own affordance.
    int element_hovered() const { return hovered_element_; }
    bool element_is_hovered(int i) const { return i >= 0 && i == hovered_element_; }
    std::function<void(int index, bool entered)> on_element_hover;

    // ── SVG fragment handles ─────────────────────────────────────────────────
    // Tag a sub-tree of the live SVG once by a unique marker substring (a path
    // `d`, the same handle the knob-needle patch keys on), then redraw JUST that
    // fragment on demand — transformed (translate/rotate/scale), composited at an
    // opacity, and optionally recolored — composited over the already-drawn frame
    // through the SAME panel fit paint() uses, so it lands on its original spot.
    // This is the primitive the hover-brighten / bypass-dim visuals ride on,
    // and what a meter-needle redraw, reorder ghost, or focus glow reuses. The
    // string/geometry work is the pure svg_fragment.hpp helpers; these methods add
    // the Canvas::draw_svg call and the panel-fit draw box.
    void register_fragment(std::string id, std::string marker);
    bool has_fragment(const std::string& id) const {
        return fragments_.find(id) != fragments_.end();
    }
    // Draw the registered fragment `id`. Returns false if the id is unknown, its
    // marker isn't in the current SVG, the panel isn't laid out, or the backend
    // can't render SVG. Never throws / never mutates the live document.
    bool draw_fragment(canvas::Canvas& canvas, const std::string& id,
                       const FragmentTransform& xform = {}, float opacity = 1.0f,
                       const std::string& recolor_hex = {}) const;
    // Lower-level: draw an arbitrary (not pre-registered) marker fragment.
    bool draw_fragment_marker(canvas::Canvas& canvas, const std::string& marker,
                              const FragmentTransform& xform = {},
                              float opacity = 1.0f,
                              const std::string& recolor_hex = {}) const;

    // Enable/disable (bypass) element `i`. A disabled element is skipped by
    // hit-testing — it cannot be hovered, dragged, clicked, or gesture — and if
    // it was the hovered element the hover is cleared. Repaints. No-op out of
    // range.
    void set_element_enabled(int i, bool enabled);
    bool element_enabled(int i) const {
        return i >= 0 && i < static_cast<int>(elements_.size()) && elements_[i].enabled;
    }

    // The panel is the view's natural size — a host should size its window to
    // this aspect so the design fills it with no letterbox (see paint()).
    float intrinsic_width() const override { return panel_w_; }
    float intrinsic_height() const override { return panel_h_; }

    void paint(canvas::Canvas& canvas) override;
    void layout_children() override;
    void on_mouse_down(Point pos) override;
    void on_mouse_drag(Point pos) override;
    void on_mouse_up(Point pos) override;
    void on_hover_move(Point pos) override;  // hover-track the element under the pointer
    void on_mouse_leave() override;          // clear hover on exit

protected:
    // Called after the active frame changes (set_active_frame or the initial
    // constructor activation). Subclasses override to react to a frame swap —
    // e.g. release any of their own held input and re-apply external highlight
    // state to the new frame's elements. Default: no-op. (Invoked from the
    // constructor's activate_frame(0) too, where virtual dispatch resolves to
    // this base no-op — subclass state isn't built yet.)
    virtual void on_active_frame_changed() {}

    // The ONE transform shared by paint() and hit_element(): a uniform fit of the
    // panel into `bounds`, centered (letterbox when bounds aspect != panel
    // aspect). `scale` is panel→view; (ox,oy) is the view-space position of the
    // panel's top-left. paint draws through it; hit_element inverts it — so a
    // knob is hit exactly where it is drawn, at ANY host window aspect. Protected
    // so a subclass that paints its own overlay (e.g. MusicalTypingKeyboard's
    // movable overview-strip highlight) maps panel↔view through the SAME fit.
    struct PanelTransform { float scale = 0.0f, ox = 0.0f, oy = 0.0f; };
    PanelTransform panel_transform(const Rect& bounds) const;

private:
    // Map a choice element's selected index to a normalized [0,1] value and back,
    // using its option count. Single source of truth for choice<->normalized.
    float choice_to_norm(int i, int selected) const;
    int   norm_to_choice(int i, float v) const;
    // Sync a user choice change (overlay widget -> element + on_element_changed).
    void  notify_choice(int i, int selected);

    // User-gesture emit helpers: route to host_params() (when routing is on and
    // the element carries a key the surface resolves) AND fire the public
    // on_element_changed / on_gesture_* callback. Single funnel so every
    // value-bearing gesture path stays consistent.
    void emit_element_changed(int i, float value);
    void emit_gesture_begin(int i);
    void emit_gesture_end(int i);
    // Build the native-overlay child widgets (TextEditor / ComboBox / tabs) for
    // the non-knob elements of the active frame; called when a frame activates.
    void build_overlays();

    // One swappable frame. Holds the panel-detected + baked-tab-suppressed SVG,
    // its overlay element list, and resolved panel crop. Frames are built once
    // (build_frame) and copied into the active members by activate_frame.
    struct Frame {
        std::string svg;
        std::vector<DesignFrameElement> elements;
        float svg_w = 0.0f, svg_h = 0.0f;
        float panel_x = 0.0f, panel_y = 0.0f, panel_w = 0.0f, panel_h = 0.0f;
    };
    // Run panel-detect + baked-tab suppression on raw inputs and return a Frame.
    // Touches no member state (safe to call before/after activation).
    Frame build_frame(std::string svg, std::vector<DesignFrameElement> elements,
                      float panel_x, float panel_y, float panel_w, float panel_h) const;
    // Tear down the active overlay widgets, copy frame `index` into the active
    // members (svg_/elements_/panel_*), and rebuild overlays.
    void activate_frame(int index);

    int hit_element(Point pos) const;

    // A native-overlay child widget bound to one element (by index). The widget
    // is owned by the View child list; this just maps element -> widget so
    // layout_children() can position it via the panel transform.
    struct Overlay { int element_index = -1; View* widget = nullptr; };

    std::string svg_;
    std::vector<DesignFrameElement> elements_;
    std::vector<Overlay> overlays_;
    float svg_w_ = 0.0f, svg_h_ = 0.0f;            // SVG intrinsic size
    float panel_x_ = 0, panel_y_ = 0, panel_w_ = 0, panel_h_ = 0;  // crop, SVG coords
    int drag_ = -1;
    float drag_start_x_ = 0.0f, drag_start_y_ = 0.0f, drag_start_value_ = 0.0f;
    int active_view_group_ = -1;   ///< momentary view scope (-1 = all active)
    std::vector<Frame> frames_;    ///< swappable frames; [0] is the constructor's
    int active_frame_ = 0;         ///< index into frames_ currently rendered
    bool route_to_host_params_ = false;   ///< self-wire gestures to host_params()
    bool route_actions_to_host_ = false;  ///< forward action clicks to host_actions()
    int hovered_element_ = -1;            ///< element under the pointer, or -1
    std::unordered_map<std::string, std::string> fragments_;  ///< id -> marker

    // The panel→view draw box for the current bounds — the exact (x,y,w,h) paint()
    // hands Canvas::draw_svg for the full frame. A fragment mini-document drawn at
    // this box composites over its original position. Returns false (leaves out-*
    // untouched) when the panel isn't laid out.
    bool current_svg_draw_box(float& ox, float& oy, float& ow, float& oh) const;

    // Set the hovered element (fires on_element_hover on change, repaints).
    void set_hovered_element(int i);
};

// The native-overlay widget for a `tab_group` element: a compact segmented
// control drawn opaque over the design's tab strip (so it replaces the baked
// tabs + highlight). Clicking a slot selects it and moves the highlight — the
// "regular selection state" a static SVG can't provide. Styling approximates the
// design's dark strip; it is intentionally an approximation rather than
// pixel-exact theming.
class DesignTabGroup : public View {
public:
    DesignTabGroup(std::vector<std::string> labels, int selected);
    int selected() const { return selected_; }
    int tab_count() const { return static_cast<int>(labels_.size()); }
    // Set selection without firing on_select (programmatic host->view push).
    void set_selected_silent(int index);
    // Fired when the USER taps a different tab (index). Not fired by
    // set_selected_silent.
    std::function<void(int index)> on_select;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;

private:
    std::vector<std::string> labels_;
    int selected_ = 0;
};

// The native-overlay widget for a `stepper` element: a header value cycled in
// place by `< >` chevrons (the design's section-header preset selectors). It
// paints the current option centered with a `<` on the left and `>` on the
// right; clicking the left third steps to the previous option, the right third
// to the next (clamped). Nothing is drawn behind the text, so the design's
// header chrome shows through — only the value text and chevrons are ours.
class DesignStepper : public View {
public:
    DesignStepper(std::vector<std::string> options, int selected);
    int selected() const { return selected_; }
    int option_count() const { return static_cast<int>(options_.size()); }
    const std::string& current() const;
    // Set selection without firing on_select (programmatic host->view push).
    void set_selected_silent(int index);
    // Fired when the USER steps to a different option (index). Not fired by
    // set_selected_silent.
    std::function<void(int index)> on_select;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;

private:
    std::vector<std::string> options_;
    int selected_ = 0;
};

}  // namespace pulp::view
