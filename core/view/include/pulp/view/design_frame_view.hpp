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
    float default_value = 0.5f;   ///< opening value. A DesignControl also receives
                                  ///< this via set_control_value() right after build
                                  ///< (that seed is authoritative), so a factory that
                                  ///< reads default_value in its ctor need not — it
                                  ///< will be seeded regardless.
};

// Builds a native overlay View for a Kind::custom element. Returns the View
// (ownership moves to the DesignFrameView) or nullptr to render inert.
using DesignControlFactory =
    std::function<std::unique_ptr<View>(const DesignControlContext&)>;

// Opt-in value conduit for a custom-control overlay. A factory-built View MAY
// also inherit DesignControl to join DesignFrameView's normalized-value channel
// — exactly as the built-in kinds do, so a custom knob behaves like a knob to
// the host: set_element_value() pushes host->view, element_value() reads back,
// and user edits route through on_element_changed + host-param binding. A View
// that does NOT inherit DesignControl is still built and rendered; it simply has
// no value channel (element_value() stays -1, set_element_value() is a no-op),
// which is the pre-conduit behavior. Retrieved via dynamic_cast, mirroring the
// DropReceiver mixin idiom used elsewhere in core/view.
class DesignControl {
public:
    virtual ~DesignControl() = default;
    // Host -> view: adopt the normalized value (0..1). SILENT — implementations
    // MUST NOT re-emit through on_control_changed (this is the host pushing a
    // value in, not the user editing), matching the silent host->view push the
    // other kinds do in DesignFrameView::set_element_value.
    virtual void set_control_value(float value) = 0;
    // View -> host: the control's current normalized value (0..1).
    virtual float control_value() const = 0;
    // Installed by DesignFrameView when the overlay is built. Implementations
    // call it on a USER-driven value change so the edit funnels through the
    // frame's single change path (on_element_changed, param_key host routing).
    // Never call it from set_control_value.
    std::function<void(float)> on_control_changed;
    // Optional undo / touch-automation bracketing for a CONTINUOUS control (a
    // knob/fader): call on_gesture_begin at the start of a drag and on_gesture_end
    // at its end so the host groups one undo step and latches automation write —
    // exactly what the built-in knob does. A momentary/discrete control may leave
    // these unset. DesignFrameView wires them to its own gesture callbacks
    // (→ host begin_gesture / end_gesture). Both are installed AFTER the initial
    // seed, so they never fire during construction.
    std::function<void()> on_gesture_begin;
    std::function<void()> on_gesture_end;
};

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

// A Kind::custom element whose `factory_id` had no registered factory when the
// overlay was built, so it rendered inert (the baked SVG still shows). Surfaced
// via DesignFrameView's opt-in diagnostic so a developer learns which ids were
// referenced-but-unregistered (a forgotten register_design_control_factory or a
// typo'd id) instead of getting a silently missing control.
struct UnregisteredCustomControl {
    std::string factory_id;      ///< the id that had no factory
    std::string source_node_id;  ///< the design-source node id (e.g. Figma), if any
};

// A control whose VISIBLE option count disagrees with the value cardinality the
// HOST reports for the parameter it is bound to (HostParamSurface::
// param_step_count). Surfaced via DesignFrameView's opt-in diagnostic.
//
// This is the mis-scale that a hand-written control table produces silently: a
// radio drawn with 3 positions, wired to a 6-value parameter, normalizes against
// whichever count the code happened to reach for. The host's count is the
// authoritative one and DesignFrameView uses it — but the disagreement itself is
// still a defect in the binding (the design draws 3 of 6 reachable values, so
// half the parameter's range has no control position), and it is reported rather
// than silently absorbed. Same never-guess contract the import lane's
// RecognitionResolver holds for an unmatched component.
// Read the two counts — the arrival of a report is not by itself the diagnosis:
//
//   ui   host   meaning
//   ---- -----  ---------------------------------------------------------------
//    3     6    a MIS-SCALED control: the design draws 3 of 6 reachable values.
//               The host's 6 is used; half the range has no control position.
//    3     0    UNANSWERED, and the view had to GUESS by the 3 it draws. Either
//               the parameter is genuinely continuous (a coarse choice control
//               over a smooth range — fine, expected) or the surface cannot
//               answer at all: do_param_step_count defaults to 0, so a surface
//               that has not wired it reports 0 for every key, including
//               discrete ones. Those two are indistinguishable from here, which
//               is why this is reported instead of silently absorbed.
//    0     6    an UNBOUND key: a commit to a key no element carries.
//    0     0    a key nothing knows.
struct ParamScaleMismatch {
    std::string param_key;     ///< the bound host-parameter key
    int ui_option_count = 0;   ///< positions the control draws (element_option_count)
    int host_step_count = 0;   ///< values the host's parameter exposes (authoritative);
                               ///< 0 means the surface reported no index domain
};

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
    // Number of discrete options element `i` offers (dropdown entries, tab_group
    // tabs, stepper positions), or 0 when the element is continuous (knob /
    // fader / xy_pad) or out of range. A toggle reports 2 (off/on) even though it
    // declares no `options` list. Non-zero exactly when element_is_discrete(i).
    //
    // This is what the control DRAWS, which is the element's normalized divisor
    // only when no host parameter says otherwise. When a HostParamSurface
    // resolves the element's param_key and reports a non-zero cardinality, THAT
    // count wins (see choice_to_norm): a parameter owns its own range, and a
    // control drawn with fewer positions than the parameter has values does not
    // shrink it. The two disagreeing is reported via set_on_param_scale_mismatch.
    int element_option_count(int i) const;
    // Whether element `i` carries a finite set of positions (dropdown / tab_group
    // / stepper / toggle) rather than a continuous range. A toggle is discrete
    // with two positions even though it declares no `options` list.
    bool element_is_discrete(int i) const;
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
    // dropdown/tab_group/stepper it is the live selection mapped over the
    // element's value count (the HOST's cardinality when a HostParamSurface
    // resolves the element's param_key, else its own option count — see
    // element_option_count). Reads the live overlay widget when one exists.
    //
    // CALL CONTEXT: for a choice element this may consult host_params(), which is
    // legal from tick/update and NEVER from paint() (HostParamSurface's hard
    // rule; a debug build asserts). A painter wanting a choice's position should
    // read the value it was pushed at tick, not re-derive it mid-render.
    float element_value(int i) const;
    // Set element `i` from a normalized [0,1] value WITHOUT firing
    // on_element_changed (a host->view push: knob turn, or choice index =
    // round(v * (count-1)) applied to the live overlay widget silently). Use for
    // automation/preset application so it doesn't echo back to the host. Same
    // tick-not-paint call context as element_value for a choice element.
    void set_element_value(int i, float v);
    // Set the live text of a Kind::value_label element and repaint. No-op for
    // other kinds / out of range. Use for readouts that must track state
    // (octave / velocity / pitch-bend).
    void set_element_text(int i, std::string text);
    // The native-overlay child widget for element `i`, or nullptr (e.g. for a
    // knob, or out of range). For tests/bindings.
    View* overlay_widget(int i) const;

    // ── Unregistered-custom-control diagnostic (opt-in, default off) ──────
    // A Kind::custom element whose factory_id has NO registered factory renders
    // inert (the baked SVG still shows) — by design a custom control never
    // blanks. But that means a forgotten register_design_control_factory or a
    // typo'd id is otherwise silent. Set this callback to be told which ids were
    // referenced-but-unregistered so a developer / a --validate check can flag
    // them. Fires once per inert Kind::custom element during overlay build, on
    // the UI thread. Because the constructor builds the initial frame before a
    // caller can attach this, setting the callback also REPLAYS the unregistered
    // controls seen in the most recent build — so a callback attached right after
    // construction still learns about them. Default (no callback set) is EXACTLY
    // the old behavior: silent inert render, SVG intact. It never changes what
    // renders — only ADDS the diagnostic signal.
    void set_on_unregistered_custom_control(
        std::function<void(const UnregisteredCustomControl&)> cb);
    // The set of unregistered custom-control factory_ids seen during the last
    // overlay build (rebuilt on every frame activation), in first-seen order and
    // de-duplicated. Empty when every Kind::custom element resolved to a factory.
    // Queryable without a callback — handy for a `--validate` style assertion.
    const std::vector<UnregisteredCustomControl>&
    unregistered_custom_controls() const {
        return unregistered_custom_;
    }

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

    // ── Runtime host-parameter surface (the "port once, any host" path) ──
    // Beyond the bind-once path (a consumer wiring on_element_changed +
    // element_for_param_key), a DesignFrameView can bind DIRECTLY to the SDK's
    // framework-agnostic HostParamSurface (View::host_params()). The surface
    // hides WHICH parameter system is underneath — an embedding plug-in
    // framework's parameter tree, or Pulp's own StateStore — so one view runs
    // unchanged in every one of them.
    //
    // Enable with route_changes_to_host_params(true): thereafter a user gesture
    // on a param_key-tagged element drives host_params() directly
    // (begin_gesture / set_param / end_gesture), and sync_from_host_params()
    // pulls current values + display text back at tick. A control whose
    // param_key is empty or unknown to the surface is left to local state
    // (degrades exactly like a preview with a null surface).
    //
    // OFF by default, and that default is load-bearing. on_element_changed keeps
    // firing when routing is on, which is harmless for a consumer that OBSERVES
    // and a double-write for one that WRITES. An embed that already carries the
    // gesture itself — on_element_changed → StateStore → set_param, which is the
    // existing binder path — must NOT also turn routing on: the host would
    // receive every value and every gesture bracket twice, which reads as a
    // doubled automation write and an unbalanced begin/end pair.
    //
    // Pick exactly one path per view: the binder (leave this off), or the
    // surface (turn it on and drop the write side of your on_element_changed
    // handler, keeping it only for observation).
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

    // ── Typed commit helpers (UI → host, gesture-bracketed) ──────────────────
    // One call per user edit, keyed by host parameter. Each brackets the write in
    // a gesture (begin → change → end) so the host groups it as ONE undo step,
    // pushes the value into the bound element, and routes through the same
    // emit_* funnel a pointer gesture uses — so host-param routing and the public
    // callbacks stay consistent no matter who drives the control.
    //
    // These exist because the alternative is every consumer hand-writing the
    // bracket + the normalization per control, which is exactly where a port
    // accrues transcription bugs. `key` is a host-parameter key; the element it
    // resolves to (element_for_param_key) may be a real control or a bind-grid
    // stand-in (see build_bind_grid) — a commit works either way. A key with no
    // element resolves to nothing and reports a mismatch rather than writing to a
    // wrong index.
    //
    // Suited to a discrete edit (a click, a typed value, a step). A continuous
    // drag should bracket ONCE around the whole drag — call emit_gesture_begin /
    // emit_element_changed / emit_gesture_end directly for that, or the host sees
    // one undo step per pixel moved.

    // Commit a normalized [0, 1] value. Clamped.
    void commit_value(const std::string& key, float normalized);

    // Commit a bipolar [-1, 1] depth, mapped to normalized as (depth + 1) / 2 —
    // so -1 → 0.0, 0 → 0.5 (center), +1 → 1.0. The mapping every bipolar control
    // (pan, detune, mod depth) otherwise re-derives by hand. Clamped to [-1, 1].
    void commit_bipolar(const std::string& key, float depth);

    // Commit a discrete value INDEX. The divisor is the host's own value count
    // (HostParamSurface::param_step_count(key) - 1) — deliberately NOT a caller-
    // supplied denominator and NOT the control's visible option count. A caller
    // that passes its own divisor is re-introducing the mis-scale this helper
    // exists to remove: the count belongs to the parameter, not to the view.
    // Falls back to the element's option count when no host surface resolves the
    // key (preview / screenshot), and reports a ParamScaleMismatch when neither
    // yields an index domain. `index` is clamped into the resolved domain.
    void commit_discrete(const std::string& key, int index);

    // ── Bind grid (one element per host parameter) ───────────────────────────
    // Append an invisible, zero-hit stand-in element for every key in `keys` that
    // the active frame does not already carry a control for. The result: EVERY
    // host parameter has an element, so the two directions of the bind need no
    // per-parameter plumbing —
    //   host → UI: sync_from_host_params() pulls each key's current value at tick,
    //              so automation and preset recall land with no extra wiring;
    //   UI → host: commit_value / commit_bipolar / commit_discrete resolve any
    //              key, whether or not the design drew a control for it.
    // A stand-in draws nothing (no needle path), never hit-tests (zero hit radius
    // AND disabled), and costs one struct plus a value copy per tick — a full
    // plug-in's worth of parameters is not a measurable cost.
    //
    // Keys already bound to a real control are SKIPPED, so a design's own control
    // always wins the element_for_param_key lookup; the grid only fills gaps. The
    // grid is re-applied per frame on every frame swap, so a key drawn on frame A
    // but absent on frame B gets a real control on A and a stand-in on B.
    // Repeated calls REPLACE the grid's key set rather than accumulating.
    //
    // The keys are caller-supplied because HostParamSurface exposes no parameter
    // ENUMERATION — it answers questions about a key you already have
    // (has_param / get_param / param_step_count) and has no "list every
    // parameter". A host reaching this surface over a C ABI has that list on its
    // own side; passing it here is the honest wiring, not a guess.
    void build_bind_grid(std::vector<std::string> keys);

    // The keys the bind grid was last built with (build_bind_grid's argument),
    // whether or not each one produced a stand-in element on the active frame.
    const std::vector<std::string>& bind_grid_keys() const { return bind_grid_keys_; }

    // Whether element `i` is a bind-grid stand-in rather than a design control.
    bool element_is_bind_grid_stand_in(int i) const;

    // ── Param-scale mismatch diagnostic (opt-in, default off) ────────────────
    // Reports a control whose visible option count disagrees with the host's
    // value cardinality for the parameter it is bound to, or a commit against a
    // key with no resolvable index domain. DesignFrameView always normalizes
    // against the HOST's count when it has one — this only ADDS the signal, it
    // never changes what is emitted.
    //
    // Fires once per distinct param_key (de-duplicated, first-seen order) on the
    // UI thread. Setting the callback REPLAYS the mismatches already seen, so a
    // callback attached after the first tick still learns about them — matching
    // set_on_unregistered_custom_control.
    void set_on_param_scale_mismatch(std::function<void(const ParamScaleMismatch&)> cb);

    // The distinct param-scale mismatches seen so far, first-seen order,
    // de-duplicated by param_key. Queryable without a callback — handy for a
    // `--validate` style assertion over a ported control table.
    const std::vector<ParamScaleMismatch>& param_scale_mismatches() const {
        return param_scale_mismatches_;
    }

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
    bool wants_mouse_input() const override { return true; }

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
    // The authoritative number of VALUES element `i`'s domain has — the count
    // choice<->normalized divides against (as count-1; see
    // param_index_to_normalized). Resolution order:
    //   1. the HOST's cardinality for the element's param_key, when a
    //      HostParamSurface resolves it and reports a non-zero step count. The
    //      parameter owns its own range; a control drawn with fewer positions
    //      than the parameter has values does not shrink the parameter.
    //   2. otherwise the element's visible option count (element_option_count) —
    //      the preview / screenshot / no-surface path, where there is no host to
    //      ask and the control's own positions are the only domain that exists.
    // Returns 0 when neither yields a domain (continuous, or unknown key with no
    // options), which param_index_to_normalized reads as "no index domain".
    //
    // Reports a ParamScaleMismatch when both counts exist and disagree.
    int resolve_value_count(int i) const;

    // Record a distinct param-scale mismatch and fire the diagnostic callback.
    // De-duplicates by param_key. Const because it is called from the const
    // normalize path; the accumulator is a diagnostic log, not observable state.
    void report_scale_mismatch(const std::string& key, int ui_count, int host_count) const;

    // The host's value cardinality for `key`, or 0 when no surface resolves it.
    int host_step_count_for(const std::string& key) const;

    // Map a choice element's selected index to a normalized [0,1] value and back,
    // using resolve_value_count. Single source of truth for choice<->normalized.
    float choice_to_norm(int i, int selected) const;
    int   norm_to_choice(int i, float v) const;

    // Append the bind grid's stand-in elements for keys the active frame carries
    // no control for. Called after every frame activation (activate_frame copies
    // frames_[i].elements over elements_, which would otherwise drop the grid).
    void apply_bind_grid();
    // Sync a user choice change (overlay widget -> element + on_element_changed).
    void  notify_choice(int i, int selected);

protected:
    // User-gesture emit helpers: route to host_params() (when routing is on and
    // the element carries a key the surface resolves) AND fire the public
    // on_element_changed / on_gesture_* callback. Single funnel so every
    // value-bearing gesture path stays consistent.
    //
    // These are the supported subclass extension point. An adapter that drives
    // the editor from something other than a pointer (an embedding host pushing
    // its own automation, or a headless QA harness) can call these to push a
    // synthetic value or bracket
    // a gesture through the *same* funnel a real pointer gesture uses — so
    // host-param routing, the public callbacks, and undo bracketing all stay
    // consistent with hand-driven input. `i` is an active-frame element index:
    // host-param routing is skipped for an out-of-range index, while the public
    // on_element_changed / on_gesture_* callback still receives `i` verbatim
    // (callers own their own index validation). UI thread only.
    void emit_element_changed(int i, float value);
    void emit_gesture_begin(int i);
    void emit_gesture_end(int i);

private:
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
    // Opt-in diagnostic for referenced-but-unregistered Kind::custom controls,
    // plus the accumulator rebuilt on every overlay build. UI-thread-only.
    std::function<void(const UnregisteredCustomControl&)> on_unregistered_custom_;
    std::vector<UnregisteredCustomControl> unregistered_custom_;

    // Param-scale mismatch diagnostic. Mutable because the normalize path that
    // detects a mismatch is const — the accumulator records what was observed and
    // never feeds back into what the view emits or renders.
    std::function<void(const ParamScaleMismatch&)> on_param_scale_mismatch_;
    mutable std::vector<ParamScaleMismatch> param_scale_mismatches_;

    // Bind grid: the caller-supplied host-parameter keys, and the index of the
    // first stand-in in elements_ (all stand-ins are appended after the active
    // frame's own elements, so one index bounds them). -1 = no grid applied.
    std::vector<std::string> bind_grid_keys_;
    int bind_grid_begin_ = -1;
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
    bool wants_mouse_input() const override { return true; }

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
    bool wants_mouse_input() const override { return true; }

private:
    std::vector<std::string> options_;
    int selected_ = 0;
};

}  // namespace pulp::view
