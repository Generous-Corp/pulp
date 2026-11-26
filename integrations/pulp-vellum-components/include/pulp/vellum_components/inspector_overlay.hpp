#pragma once

/// @file inspector_overlay.hpp
/// The inspector's canvas overlay for a Vellum view tree: hover and selection
/// state, the selection box with its handles, the type badge, and the
/// measurements (padding and margin bands, distances to the parent and
/// between views), and a pixel loupe, painted over the inspected root.
///
/// The overlay paints no side panel, so the whole canvas stays live for
/// picking. The inspector window is the tree and property surface; a host
/// mirrors selection between the two through `selected_view` and
/// `set_selected_view`.

#include <pulp/vellum_components/aliases.hpp>
#include <pulp/vellum_components/atlas_inventory.hpp>
#include <pulp/vellum_components/field_edit.hpp>
#include <pulp/vellum_components/knob_panel.hpp>
#include <pulp/vellum_components/overlay_geometry.hpp>
#include <pulp/vellum_components/render_pass_stats.hpp>
#include <pulp/vellum_components/text_edit.hpp>

#include <vellum/view/input_events.hpp>

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace pulp::vellum_components {

/// The value of a committed property edit: a number for a layout or opacity
/// field, text for a view's copy.
using EditValue = std::variant<float, std::string>;

/// Receives each committed edit: the edited view's anchor id, the dotted
/// property path ("layout.padding"), the new value and what produced it.
using EditSink = std::function<void(std::string_view anchor, std::string_view path,
                                    const EditValue& value, std::string_view source)>;

/// Where to put a badge so it stays readable.
struct BadgePlacement {
    float x = 0.0f;
    float y = 0.0f;
    bool below = false; ///< true when flipped below the selection
};

/// Place a `badge_w` by `badge_h` badge for a selection whose top-left is
/// (`sel_x`, `sel_y`) and whose height is `sel_h`. The badge sits `gap` above
/// the selection; when its top would fall above `top_margin` it flips below
/// the selection. `x` is clamped so the badge never leaves the root, and
/// `root_w <= 0` disables the right-hand clamp.
///
/// Pure arithmetic, so it needs no canvas.
BadgePlacement compute_badge_placement(float sel_x, float sel_y, float sel_h, float badge_w,
                                       float badge_h, float root_w, float gap = 5.0f,
                                       float top_margin = 0.0f);

class InspectorOverlay {
  public:
    /// What follows the pointer: nothing until a click (`follows_focus`), or
    /// the hovered view on every pointer move (`follows_mouse`).
    enum class SelectionMode : std::uint8_t { follows_focus, follows_mouse };

    /// `root` is borrowed and must outlive the overlay.
    explicit InspectorOverlay(View& root);
    ~InspectorOverlay();

    InspectorOverlay(const InspectorOverlay&) = delete;
    InspectorOverlay& operator=(const InspectorOverlay&) = delete;

    /// The view this overlay inspects and paints into. The paint hook compares
    /// it with the root being painted so the overlay never paints into another
    /// surface, such as the inspector window's own.
    View& inspected_root() const {
        return root_;
    }

    bool is_active() const {
        return active_;
    }
    /// Turning the overlay off drops the selection, the hover and the
    /// distance anchor so reopening starts clean.
    void set_active(bool active);
    void toggle() {
        set_active(!active_);
    }

    View* selected_view() const {
        return selected_;
    }
    View* hovered_view() const {
        return hovered_;
    }

    /// Select `view` directly, for a window that shares the selection. Null
    /// clears it. Works whether or not the overlay is active.
    void set_selected_view(View* view) {
        selected_ = view;
    }

    /// Move the selection to its parent. False, and no change, at the root or
    /// with no selection.
    bool select_parent();

    SelectionMode selection_mode() const {
        return selection_mode_;
    }
    void set_selection_mode(SelectionMode mode) {
        selection_mode_ = mode;
    }
    void toggle_selection_mode() {
        selection_mode_ = selection_mode_ == SelectionMode::follows_focus
                              ? SelectionMode::follows_mouse
                              : SelectionMode::follows_focus;
    }

    /// Show the eight resize handles on the selection and the move and
    /// resize cursors over it. Off by default.
    void set_dragging_enabled(bool enabled) {
        dragging_enabled_ = enabled;
    }
    bool dragging_enabled() const {
        return dragging_enabled_;
    }

    /// The affordance of a press at `pos` (root space): a handle resizes, the
    /// body moves, anywhere else, or with dragging off or nothing selected,
    /// none.
    overlay::CursorAffordance cursor_affordance_at(Point pos) const;

    /// `cursor_affordance_at` as the `View::CursorStyle` value a host applies,
    /// or -1 to keep the cursor of the view under the pointer.
    int cursor_style_for(Point pos) const;

    /// The canvas tool: what a press on the canvas does. Select (`V`) is the
    /// default and selects. Text (`T`) edits the copy of the Label or
    /// TextEditor under the press in place, and never selects or drags.
    enum class Tool : std::uint8_t { select, text };
    Tool tool() const {
        return tool_;
    }
    /// Leaving the Text tool with an edit open commits it.
    void set_tool(Tool tool);
    void toggle_tool() {
        set_tool(tool_ == Tool::select ? Tool::text : Tool::select);
    }

    /// Where committed edits go. The overlay edits the live view itself and
    /// reports the change here for whatever persists it; with no sink the live
    /// edit still happens and nothing is reported. Only views with an anchor
    /// id are reported.
    void set_edit_sink(EditSink sink) {
        edit_sink_ = std::move(sink);
    }
    bool has_edit_sink() const {
        return static_cast<bool>(edit_sink_);
    }

    /// True while a numeric field is being edited.
    bool is_editing() const {
        return field_edit_.active();
    }
    /// The property path being edited ("layout.padding"), or empty.
    const std::string& editing_field() const {
        return field_edit_.path();
    }
    /// The text typed so far, and the caret's index into it.
    const std::string& edit_buffer() const {
        return field_edit_.text();
    }
    std::size_t edit_caret_pos() const {
        return field_edit_.caret();
    }

    /// Start editing `field_path` ("style.opacity", "layout.width",
    /// "layout.height", "layout.padding" or "layout.margin") on the selection,
    /// showing `initial_value`. False, and no edit, with no selection or an
    /// empty path. The edit keeps going to the view it began on even if the
    /// selection moves.
    bool begin_field_edit(std::string field_path, float initial_value);

    /// Apply the typed text, report it to the edit sink and stop editing.
    /// True when a value was reported: false for an empty or non-numeric
    /// buffer, a view without an anchor id, or no sink. Editing stops either
    /// way.
    bool commit_field_edit();

    /// Stop editing and restore the value the field had when the edit began.
    void cancel_field_edit();

    /// True while a view's copy is being edited in place.
    bool text_editing() const {
        return text_target_ != nullptr;
    }
    /// The live edited copy; empty when not editing.
    const std::string& text_edit_buffer() const {
        return text_edit_.text();
    }
    View* text_edit_target() const {
        return text_target_;
    }

    /// Start editing `view`'s copy, caret at the end, and select it. False,
    /// and no edit, for a null view or one without editable text (see
    /// `overlay::view_has_editable_text`). Typing previews on the view live.
    /// A numeric field edit that was open is committed first.
    bool begin_text_edit(View* view);
    /// Keep the live copy and report it to the edit sink under
    /// `text_tweak_path()`. True when a changed copy was reported; false for
    /// unchanged text, a view with no anchor id, no sink or a view that left
    /// the tree. The edit ends either way.
    bool commit_text_edit();
    /// End the edit and restore the copy it began with.
    void cancel_text_edit();

    /// UTF-8 character input for the open text edit. The host routes it here
    /// before the view tree (`install_inspector_hooks` does). True when an
    /// edit is open and took the text; false otherwise, so the text reaches
    /// the focused widget.
    bool handle_text_input(const ::vellum::view::TextInputEvent& event);

    /// The caret's byte offset, and the selected byte range (low, high).
    std::size_t text_caret() const {
        return text_edit_.caret();
    }
    std::pair<std::size_t, std::size_t> text_selection() const {
        return text_edit_.selection();
    }
    bool text_has_selection() const {
        return text_edit_.has_selection();
    }
    void text_move_caret(int delta, bool extend);
    void text_move_word(int direction, bool extend);
    void text_move_home(bool extend);
    void text_move_end(bool extend);
    void text_select_all();
    /// Insert at the caret, replacing any selection.
    void text_insert(const std::string& utf8);
    void text_delete_backward();
    void text_delete_forward();
    /// Copy, cut and paste through the clipboard, which defaults to the
    /// platform's. Cut removes the selection even if the clipboard write
    /// failed. Each returns whether the clipboard was read or written.
    bool text_copy();
    bool text_cut();
    bool text_paste();
    void set_text_clipboard(overlay::TextClipboard clipboard);

    /// The property path a committed copy is reported under. "text" by
    /// default; an empty path is ignored.
    const std::string& text_tweak_path() const {
        return text_tweak_path_;
    }
    void set_text_tweak_path(std::string path) {
        if (!path.empty())
            text_tweak_path_ = std::move(path);
    }

    /// True while a text edit is open on the selected view: the selection
    /// paints a thin outline and a caret instead of the resize box and
    /// handles.
    bool selection_uses_subtle_edit_outline() const {
        return text_editing() && selected_ != nullptr && selected_ == text_target_;
    }

    /// The render pass viewer: a panel docked to the top right that shows,
    /// for each pass type, the last, mean and peak CPU time and draw calls
    /// over the last `overlay::kPassHistoryFrames` frames, with a sparkline
    /// and a count of frames over budget. `P` toggles it. It reads a
    /// `RenderPassSource` the host supplies and borrows (null, the default,
    /// shows "No render pass source attached"). The timings are CPU
    /// wall-time, not GPU time.
    void set_render_pass_source(const overlay::RenderPassSource* source) {
        pass_source_ = source;
    }
    const overlay::RenderPassSource* render_pass_source() const {
        return pass_source_;
    }
    /// Record the source's last frame into the history. Every paint calls
    /// this, so the history keeps filling while the viewer is hidden. True when
    /// a new frame was recorded; false with no source, no frame run or a frame
    /// already recorded.
    bool capture_pass_frame();
    /// Every pass type in compositing order with its statistics.
    std::vector<overlay::PassAttribution> pass_attribution() const {
        return pass_history_.attribution();
    }
    const overlay::PassHistory& pass_history() const {
        return pass_history_;
    }
    int budget_overrun_count() const {
        return pass_history_.overrun_count();
    }
    std::uint64_t pass_frames_captured() const {
        return pass_history_.frames_captured();
    }
    bool pass_viewer_enabled() const {
        return pass_viewer_enabled_;
    }
    void set_pass_viewer_enabled(bool enabled) {
        pass_viewer_enabled_ = enabled;
    }
    void toggle_pass_viewer() {
        pass_viewer_enabled_ = !pass_viewer_enabled_;
    }

    /// The texture atlas viewer: the same docked panel as the pass viewer,
    /// listing each atlas of an inventory the host supplies and borrows with
    /// its size, page and entry counts and an occupancy bar that turns amber at
    /// 60 percent and red at 85. `A` toggles it. The pass viewer wins the panel
    /// when both are on. With no inventory it says none is attached; with an
    /// empty one, that no atlas is available.
    void set_atlas_inventory(const overlay::AtlasInventory* inventory) {
        atlas_inventory_ = inventory;
    }
    const overlay::AtlasInventory* atlas_inventory() const {
        return atlas_inventory_;
    }
    bool atlas_viewer_visible() const {
        return atlas_viewer_visible_;
    }
    void set_atlas_viewer_visible(bool visible) {
        atlas_viewer_visible_ = visible;
    }
    void toggle_atlas_viewer() {
        atlas_viewer_visible_ = !atlas_viewer_visible_;
    }
    /// The atlas rows the last paint laid out; zero while the viewer is
    /// hidden, empty or the overlay is off.
    std::size_t atlas_row_count() const {
        return atlas_row_count_;
    }

    /// The knob panel: a panel docked to the bottom right with one row of
    /// segments for each knob of a schema, and, once an agent request sink is
    /// set, an "Ask agent" field with a Send button. `Shift+T` shows and hides
    /// it. Choosing a segment records it (`knob_selection`) and reports the
    /// knob id and the value the segment emits through the apply sink, and the
    /// overlay changes no file: whatever applies the knob owns the design.
    void set_knob_schema(overlay::KnobSchema schema) {
        knob_schema_ = std::move(schema);
    }
    const overlay::KnobSchema& knob_schema() const {
        return knob_schema_;
    }
    bool knob_panel_visible() const {
        return knob_panel_visible_;
    }
    void set_knob_panel_visible(bool visible) {
        knob_panel_visible_ = visible;
    }
    void toggle_knob_panel() {
        knob_panel_visible_ = !knob_panel_visible_;
    }

    /// A knob choice: the knob's id, and the value its segment emits (a
    /// slider's numeric anchor, otherwise the label).
    struct KnobApplyEdit {
        std::string knob_id;
        std::string value;
    };
    void set_knob_apply_sink(std::function<void(const KnobApplyEdit&)> sink) {
        knob_apply_sink_ = std::move(sink);
    }
    /// The value last chosen for a knob, or empty before any choice. The
    /// overlay tracks it for display only; what applies the knob owns the
    /// real state.
    std::string knob_selection(std::string_view knob_id) const;

    /// A free-text request typed into the agent field. The field and its Send
    /// button show only while a sink is set, since a control that goes nowhere
    /// would mislead.
    struct AgentRequestEdit {
        std::string text;
    };
    void set_agent_request_sink(std::function<void(const AgentRequestEdit&)> sink) {
        agent_request_sink_ = std::move(sink);
    }
    bool has_agent_request_sink() const {
        return static_cast<bool>(agent_request_sink_);
    }

    /// Where a built request goes. It is a hand-off, not a send: the overlay
    /// makes no model call, and a host's handler queues the payload for the
    /// agent to pick up on its next turn.
    using AgentRequestHandoff = std::function<void(const overlay::AgentRequestPayload&)>;
    /// Install an agent request sink that builds the payload for each request
    /// from `context` and `now` (both optional) and passes it to `handoff`. A
    /// null `handoff` removes the sink.
    void set_agent_request_handoff(AgentRequestHandoff handoff,
                                   std::function<overlay::AgentRequestContext()> context = {},
                                   std::function<std::string()> now = {});

    /// One segment's rectangle from the last paint, in root space, for a
    /// driver that wants to click it without redoing the layout.
    struct KnobHit {
        std::string knob_id;
        std::string value;
        overlay::Rect bounds;
    };
    const std::vector<KnobHit>& knob_hits() const {
        return knob_hits_;
    }
    overlay::Rect agent_field_bounds() const {
        return agent_field_bounds_;
    }
    overlay::Rect agent_send_bounds() const {
        return agent_send_bounds_;
    }
    /// The panel's rectangle from the last paint; empty while it is hidden.
    overlay::Rect knob_panel_bounds() const {
        return knob_panel_bounds_;
    }
    /// The draft in the agent field, and whether it has the keyboard.
    const std::string& agent_text_buffer() const {
        return agent_edit_.text();
    }
    bool agent_field_active() const {
        return agent_field_active_;
    }

    /// Offer a key to the overlay. Cmd+I (Ctrl+I elsewhere) toggles it. While
    /// the agent field has focus it owns the keyboard: Enter sends, Esc defocuses
    /// and keeps the draft, Backspace and Left and Right edit it, and every
    /// other key is declined. While a text edit is open it owns the keyboard: Enter commits, Esc cancels
    /// and returns to the Select tool, Backspace and Delete remove, the
    /// arrows, Home, End and Alt-arrows move (Shift extends), and the main
    /// modifier with A, C, X and V selects, copies, cuts and pastes; every
    /// other key, being character input, is declined so it arrives through
    /// `handle_text_input`. While a field is being edited the edit owns the
    /// keyboard: digits extend the
    /// buffer, Up and Down nudge by 1 (10 with Shift, 100 with Cmd), Left,
    /// Right, Home and End move the caret, Backspace trims, Enter commits, Esc
    /// cancels and Tab commits and moves to the next field (Shift+Tab to the
    /// previous); every other key falls through to the view tree untouched.
    /// Otherwise, while active, V and T pick the tool, P toggles the render
    /// pass viewer, A the atlas viewer, Shift+T the knob panel, Escape deselects and keeps the overlay on, M
    /// flips the selection mode and Z toggles the loupe. Returns true when the
    /// key was consumed.
    bool handle_key_event(const ::vellum::view::KeyEvent& event);

    /// Offer a mouse event, in root space, to the overlay. A press selects the
    /// view under it (Alt+press measures from an anchor) and is consumed; a
    /// plain move only tracks the hover and is not. Returns true when
    /// consumed. Inactive overlays consume nothing.
    bool handle_mouse_event(const ::vellum::view::MouseEvent& event);

    /// Paint the overlay into `canvas`, which is already showing the inspected
    /// root: the highlight, then a line with its pixel distance from the
    /// measurement anchor, and from the Alt-hover target, to the selection,
    /// then the selection's padding and margin bands and its distances to its
    /// parent, then the loupe on top of everything. Selection and hover that
    /// no longer point into the tree are dropped first. A no-op while
    /// inactive.
    void paint(canvas::Canvas& canvas);

    /// The pixel loupe: a bottom-left panel magnifying the pixels around the
    /// pointer, with the sampled pixel outlined and its position and colour
    /// read out. The `Z` key toggles it while the overlay is active, and
    /// turning the overlay off closes it. It re-centres on every mouse event
    /// while open and consumes none.
    bool zoom_active() const {
        return zoom_active_;
    }
    void set_zoom_active(bool active);
    void toggle_zoom() {
        set_zoom_active(!zoom_active_);
    }

    /// Panel pixels per source pixel, clamped to [4, 40]. 20 by default.
    int zoom_factor() const {
        return zoom_factor_;
    }
    void set_zoom_factor(int factor);

    /// The root-space point the loupe last sampled.
    Point zoom_sample_center() const {
        return zoom_sample_center_;
    }

    /// The colour at that point from the last paint: read back from the
    /// surface when it can, else the declared background of the view under
    /// it, else fully transparent.
    canvas::Color zoom_center_color() const {
        return zoom_center_color_;
    }

    /// The view the Alt-hover measurement currently reaches toward, or null.
    const View* alt_hover_target() const {
        return alt_hover_target_;
    }
    /// The view an Alt+press pinned as the start of a measurement, or null.
    const View* distance_anchor() const {
        return distance_anchor_;
    }

  private:
    void drop_stale_views();
    bool handle_text_key(const ::vellum::view::KeyEvent& event);
    bool mouse_text_tool(const ::vellum::view::MouseEvent& event, bool is_press);
    void paint_text_edit(canvas::Canvas& canvas);
    void paint_pass_panel(canvas::Canvas& canvas);
    void paint_knob_panel(canvas::Canvas& canvas);
    bool mouse_knob_panel(const ::vellum::view::MouseEvent& event, bool is_press);
    bool handle_agent_key(const ::vellum::view::KeyEvent& event);
    void submit_agent_request();
    void paint_atlas_panel(canvas::Canvas& canvas);
    bool text_target_reachable() const;
    void push_text_to_target();
    void clear_text_edit();
    bool handle_edit_key(const ::vellum::view::KeyEvent& event);
    void apply_edit_buffer_to_view();
    void start_field_edit(View& target, std::string field_path, float initial_value);
    void clear_field_edit();
    bool emit_edit(const View& target, std::string_view path, EditValue value,
                   std::string_view source) const;
    void paint_highlight(canvas::Canvas& canvas);
    void paint_distance_lines(canvas::Canvas& canvas);
    void paint_box_model(canvas::Canvas& canvas, const View& view);
    void paint_zoom_panel(canvas::Canvas& canvas);
    void update_zoom_sample(canvas::Canvas& canvas);
    bool resolve_view_color_at(Point p, canvas::Color& out) const;
    bool select_with_mouse(const ::vellum::view::MouseEvent& event, bool is_press,
                           bool is_drag_tick, bool is_release);

    View& root_;
    bool active_ = false;
    View* selected_ = nullptr;
    View* hovered_ = nullptr;
    View* distance_anchor_ = nullptr;
    View* alt_hover_target_ = nullptr;
    SelectionMode selection_mode_ = SelectionMode::follows_focus;
    bool dragging_enabled_ = false;
    EditSink edit_sink_;
    overlay::FieldEditBuffer field_edit_;
    View* field_target_ = nullptr; ///< the view the open field edit writes to
    Tool tool_ = Tool::select;
    View* text_target_ = nullptr; ///< the view whose copy is being edited
    std::string text_original_;   ///< the copy when the edit began
    overlay::TextEditBuffer text_edit_;
    overlay::TextClipboard clipboard_ = overlay::TextClipboard::system();
    std::string text_tweak_path_ = "text";
    const overlay::RenderPassSource* pass_source_ = nullptr;
    overlay::PassHistory pass_history_;
    bool pass_viewer_enabled_ = false;
    const overlay::AtlasInventory* atlas_inventory_ = nullptr;
    bool atlas_viewer_visible_ = false;
    overlay::KnobSchema knob_schema_;
    bool knob_panel_visible_ = false;
    std::map<std::string, std::string> knob_selection_;
    std::function<void(const KnobApplyEdit&)> knob_apply_sink_;
    std::function<void(const AgentRequestEdit&)> agent_request_sink_;
    std::vector<KnobHit> knob_hits_;
    overlay::Rect agent_field_bounds_;
    overlay::Rect agent_send_bounds_;
    overlay::Rect knob_panel_bounds_;
    overlay::TextEditBuffer agent_edit_;
    bool agent_field_active_ = false;
    std::size_t atlas_row_count_ = 0;

    static constexpr float kFontSize = 11.0f;
    bool zoom_active_ = false;
    int zoom_factor_ = 20;
    Point zoom_sample_center_{};
    canvas::Color zoom_center_color_{};

    static constexpr int kZoomGridCells = 11; ///< odd, so the sampled cell is exact
    static constexpr int kZoomFactorMin = 4;
    static constexpr int kZoomFactorMax = 40;
    static constexpr float kZoomReadoutHeight = 36.0f;
    static constexpr float kZoomPanelMargin = 12.0f;
    static constexpr float kEdgeFontSize = 9.0f;
    static constexpr float kBadgeHeight = 16.0f;
    static constexpr float kHandleHalfSide = 4.0f;
    static constexpr float kCaretWidth = 1.5f;
    static constexpr float kPanelWidth = 260.0f;
    static constexpr float kPanelMargin = 12.0f;
    static constexpr float kPanelPadding = 8.0f;
    static constexpr float kPassChipWidth = 8.0f;
    static constexpr float kFallbackFontSize = 13.0f;
};

/// Route the View layer's inspector hooks (paint, key, mouse, text, cursor) to
/// `inspector`. The hooks are process-wide, so one overlay owns them at a
/// time; a later call replaces the earlier owner. Every hook reads the owner
/// when it fires, so `uninstall_inspector_hooks`, or the owner's destructor,
/// leaves the installed hooks inert rather than dangling. Paint, mouse and
/// cursor events carry the root they belong to and are ignored unless it is
/// the inspected root.
void install_inspector_hooks(InspectorOverlay& inspector);

/// Release every hook slot and forget the owner.
void uninstall_inspector_hooks();

} // namespace pulp::vellum_components
