#pragma once

// Native GPU UI for the Bendr example processor, built on Pulp's view stack.
//
// Pitch/formant XY pad with semitone grid + crosshair ball, a live
// log-frequency spectrum, a freeze toggle, and a pitched-delay + latency
// HUD. Layout is fully proportional to the view bounds so it never clips
// and scales with the (aspect-locked) host window. Pointer drives params
// through proper host gestures (begin/set/end) so changes stick in the
// DAW; the ball tracks a local value during a drag so it follows the
// cursor regardless of audio-thread parameter echo.

#include "reference_processor.hpp"
#include <pulp/format/standalone_settings.hpp>
#include <pulp/state/midi_parameter_map.hpp>
#include <pulp/state/parameter_edit.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/view.hpp>
#include <pulp/canvas/canvas.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace bendr {

namespace cv = pulp::canvas;
namespace vw = pulp::view;

namespace palette {
inline constexpr cv::Color bg = cv::Color::rgba8(30, 30, 46);
inline constexpr cv::Color surface = cv::Color::rgba8(49, 50, 68);
inline constexpr cv::Color elevated = cv::Color::rgba8(69, 71, 90);
inline constexpr cv::Color border = cv::Color::rgba8(88, 91, 112);
inline constexpr cv::Color accent = cv::Color::rgba8(137, 180, 250);
inline constexpr cv::Color accent2 = cv::Color::rgba8(245, 194, 231);
inline constexpr cv::Color success = cv::Color::rgba8(166, 227, 161);
inline constexpr cv::Color warn = cv::Color::rgba8(250, 179, 135);
inline constexpr cv::Color text = cv::Color::rgba8(205, 214, 244);
inline constexpr cv::Color text_dim = cv::Color::rgba8(166, 173, 200);
// XY pad — a calm cool scheme (sky/teal), no harsh pink.
inline constexpr cv::Color pad_grid = cv::Color::rgba8(58, 60, 82);
inline constexpr cv::Color pad_grid_axis = cv::Color::rgba8(80, 84, 110);
inline constexpr cv::Color pad_cross = cv::Color::rgba8(137, 220, 235, 60);
inline constexpr cv::Color pad_handle = cv::Color::rgba8(137, 220, 235);
inline constexpr cv::Color pad_handle_glow = cv::Color::rgba8(137, 220, 235, 46);
inline constexpr cv::Color pad_handle_ring = cv::Color::rgba8(198, 244, 250);
// Spectrum — teal line over a soft fill.
inline constexpr cv::Color spectrum_line = cv::Color::rgba8(148, 226, 213);
inline constexpr cv::Color spectrum_fill = cv::Color::rgba8(148, 226, 213, 38);
} // namespace palette

class ReferenceUi : public vw::View {
public:
    ReferenceUi(pulp::state::StateStore& store, SpectrumBus* spectrum,
                pulp::state::MidiParameterMap* midi_map,
                const std::atomic<float>* latency_ms = nullptr)
        : store_(store), spectrum_(spectrum), midi_map_(midi_map),
          latency_ms_(latency_ms), edit_(store) {
        // The editor has live content (spectrum, the XY handle and value
        // readouts that must track host automation), so it repaints every
        // frame. (The "wedge on parameter adjust" was NOT this — it was an
        // uncaught NSAttributedString exception in ANOTHER Pulp plugin's
        // editor paint killing the shared AU host process; fixed in the SDK
        // canvas text path + drawRect exception guard.)
        set_continuous_repaint(true);
        // NOT focusable at the root: a focusable root claims the keyboard
        // input-focus slot on EVERY click/drag, which makes the plugin view
        // hold the host window's first responder and swallow the host's
        // keyboard routing (Logic's Musical Typing goes silent — "adjusting
        // the plugin kills the instrument"). Keyboard focus is claimed only
        // while a type-in field is active (enter_typein → claim_input_focus,
        // commit/cancel → release_input_focus).
        // Prefer the GPU (Dawn/Skia/Metal) editor host for smooth rendering;
        // falls back to the CPU host if GPU init isn't available in the DAW's
        // process.
        set_requires_gpu_host(true);
    }

    void on_resized() override { layout(); }

    void paint(cv::Canvas& canvas) override {
        if (layout_dirty_) layout();
        const float W = local_bounds().width, H = local_bounds().height;
        const float s = scale();

        canvas.set_fill_color(palette::bg);
        canvas.fill_rect(0, 0, W, H);

        canvas.set_fill_color(palette::text);
        canvas.set_font("Inter", 20.0f * s);
        canvas.fill_text("Bendr", 20 * s, 32 * s);
        canvas.set_fill_color(palette::text_dim);
        canvas.set_font("Inter", 12.0f * s);
        canvas.fill_text("Real-time pitch & formant shifting with spectral freeze and pitched delay",
                         20 * s, 50 * s);

        paint_settings_button(canvas);
        paint_xy_pad(canvas);
        paint_spectrum(canvas);
        paint_freeze(canvas);
        paint_controls(canvas);
        paint_learn_banner(canvas);

        // Self-driven repaint loop for the live spectrum + automation echo.
        request_repaint();
    }

    // Pointer handling — IMPORTANT: the mac plugin host fires BOTH
    // on_mouse_event AND on_mouse_down/drag/up for a single gesture (it
    // delivers down/up via on_mouse_event + the legacy callback, and drag
    // ONLY via on_mouse_drag). Handling each callback independently would
    // double-process every press — toggling freeze twice (so it never
    // changes) and opening the pad gesture twice (mismatched begin/end →
    // the DAW intermittently rejects the move and the ball snaps back).
    // So all callbacks funnel through pointer_press/move/release, which
    // dedup on pointer_down_ and are robust to every host's event
    // convention.
    void on_mouse_event(const vw::MouseEvent& e) override {
        if (e.is_wheel) { scroll_over(e.position, e.scroll_delta_y); return; }
        switch (e.phase) {
            case vw::MousePhase::press:   pointer_press(e.position, e.button); break;
            case vw::MousePhase::drag:    pointer_move(e.position); break;
            case vw::MousePhase::release: pointer_release(); break;
            case vw::MousePhase::hover:   break;
            case vw::MousePhase::automatic:
                // Ambiguous legacy convention. is_down=true = down: open the
                // gesture, or — if already open — treat as a drag tick (a
                // held pointer that keeps reporting position). is_down=false
                // is deliberately IGNORED here: on a force-touch trackpad the
                // rich path can emit a spurious up mid-drag, which would end
                // the gesture and make the handle stop following / snap. The
                // real release arrives reliably via on_mouse_up (mac host) or
                // an explicit release phase (headless), so we only end there.
                if (e.is_down) {
                    if (pointer_down_) pointer_move(e.position);
                    else pointer_press(e.position, e.button);
                }
                break;
        }
        if (vw::View::on_pointer_event) vw::View::on_pointer_event(e);
    }
    void on_mouse_down(vw::Point p) override { pointer_press(p, vw::MouseButton::left); }
    void on_mouse_drag(vw::Point p) override { pointer_move(p); }
    void on_mouse_up(vw::Point) override { pointer_release(); }

    // The macOS plugin host gates acceptsFirstResponder on the focused view's
    // accepts_text_input(). Claiming the input-focus slot is not enough: without
    // this the editor never becomes first responder in a DAW and on_key_event /
    // on_text_input below are never called. Reporting true only while a type-in
    // is open hands the keyboard straight back, so the host keeps its transport
    // keys and Musical Typing the rest of the time.
    bool accepts_text_input() const override { return edit_ctl_ >= 0; }

    bool on_key_event(const vw::KeyEvent& e) override {
        if (edit_ctl_ < 0 || !e.is_down) return false;
        if (e.key == vw::KeyCode::enter)      { commit_typein(); return true; }
        if (e.key == vw::KeyCode::escape)     { cancel_typein(); return true; }
        if (e.key == vw::KeyCode::tab) {
            // Commit the current field and move focus to the next numeric field
            // (Shift+Tab walks backward). Toggle rows (Delay/Link) are skipped.
            const int next = next_number_field(edit_ctl_, e.isShiftDown());
            commit_typein();
            if (next >= 0) enter_typein(next);
            return true;
        }
        if (e.key == vw::KeyCode::backspace) {
            if (typein_fresh_) { edit_buf_.clear(); typein_fresh_ = false; } // delete selection
            else if (!edit_buf_.empty()) edit_buf_.pop_back();
            return true;
        }
        return false;
    }
    // The HOST took the keyboard back (the user clicked a host control or
    // switched windows) while a type-in was open: commit it, exactly like a
    // click-away inside the plugin. The view host has already cleared the
    // input-focus slot, so commit_typein's release_input_focus() no-ops —
    // this just closes the editing UI and applies the typed value.
    void on_focus_changed(bool gained) override {
        vw::View::on_focus_changed(gained);
        if (!gained && edit_ctl_ >= 0) commit_typein();
    }
    void on_text_input(const vw::TextInputEvent& e) override {
        if (edit_ctl_ < 0) return;
        for (char ch : e.text) {
            const bool valid = (ch >= '0' && ch <= '9') ||
                               (ch == '.' && edit_buf_.find('.') == std::string::npos) ||
                               (ch == '-' && (typein_fresh_ || edit_buf_.empty()));
            if (!valid) continue;
            if (typein_fresh_) { edit_buf_.clear(); typein_fresh_ = false; } // replace selection
            if (ch == '.' && edit_buf_.find('.') != std::string::npos) continue;
            edit_buf_.push_back(ch);
        }
    }

    // Test accessors (headless interaction verification).
    void layout_for_test() { layout(); }
    vw::Point freeze_center_for_test() { if (layout_dirty_) layout();
        return {freeze_.x + freeze_.width * 0.5f, freeze_.y + freeze_.height * 0.5f}; }
    vw::Point pad_center_for_test() { if (layout_dirty_) layout();
        return {pad_.x + pad_.width * 0.5f, pad_.y + pad_.height * 0.5f}; }
    vw::Rect pad_rect_for_test() { if (layout_dirty_) layout(); return pad_; }
    vw::Point ctl_up_center_for_test(int i) { if (layout_dirty_) layout();
        auto& r = ctl_up_[static_cast<size_t>(i)]; return {r.x + r.width*0.5f, r.y + r.height*0.5f}; }
    int edit_ctl_for_test() const { return edit_ctl_; }
    vw::Point ctl_field_center_for_test(int i) { if (layout_dirty_) layout();
        auto& r = ctl_field_[static_cast<size_t>(i)]; return {r.x + r.width*0.5f, r.y + r.height*0.5f}; }

private:
    static bool in_rect(vw::Point p, const vw::Rect& r) {
        return p.x >= r.x && p.x <= r.x + r.width && p.y >= r.y && p.y <= r.y + r.height;
    }
    float scale() const {
        return std::max(0.5f, local_bounds().height / 560.0f);
    }

    void layout() {
        const float W = local_bounds().width, H = local_bounds().height;
        const float s = scale();
        const float m = 20 * s, top = 64 * s;
        // Standalone-only gear, right-aligned on the title line. An OVERLAY: it
        // lives above `top`, so it never affects the pad/spectrum/control layout
        // below (the title row already reserves this space).
        const float sb_w = 104 * s, sb_h = 30 * s;
        settings_btn_ = {W - m - sb_w, 13 * s, sb_w, sb_h};
        // XY pad: a square that fits the available height, left column.
        const float pad_size = std::min(W * 0.48f, H - top - m);
        pad_ = {m, top, pad_size, pad_size};
        const float rx = pad_.right() + m;
        const float rw = W - rx - m;
        spectrum_rect_ = {rx, top, rw, (H - top - m) * 0.42f};
        freeze_ = {rx, spectrum_rect_.bottom() + 12 * s, rw, 44 * s};
        hud_ = {rx, freeze_.bottom() + 12 * s, rw,
                std::max(60.0f, H - (freeze_.bottom() + 12 * s) - m)};

        // Editable control rows inside the panel (leave a strip at the bottom
        // for the latency readout).
        const int N = static_cast<int>(ctls().size());
        const float pt = hud_.y + 8 * s, pb = hud_.bottom() - 20 * s;
        const float row_h = (pb - pt) / N;
        for (int i = 0; i < N; ++i) {
            const float ry = pt + i * row_h;
            const float fw = std::min(hud_.width * 0.46f, 150.0f * s);
            const float fx = hud_.right() - 12 * s - fw;
            ctl_field_[static_cast<size_t>(i)] = {fx, ry + 2 * s, fw, row_h - 6 * s};
            const float aw = ctl_field_[static_cast<size_t>(i)].height;
            ctl_down_[static_cast<size_t>(i)] = {fx, ry + 2 * s, aw, row_h - 6 * s};
            ctl_up_[static_cast<size_t>(i)] =
                {fx + fw - aw, ry + 2 * s, aw, row_h - 6 * s};
        }
        layout_dirty_ = false;
    }

    struct Ctl {
        pulp::state::ParamID id;
        const char* label;
        bool is_toggle;
        float lo, hi, step;
        int decimals;
        const char* unit;
    };
    static const std::array<Ctl, 7>& ctls() {
        static const std::array<Ctl, 7> c{{
            {kPitch,    "Pitch",    false, -12.f, 12.f,  0.5f, 2, "st"},
            {kFormant,  "Formant",  false, -12.f, 12.f,  0.5f, 2, "st"},
            {kMix,      "Mix",      false,   0.f, 100.f, 1.f,  0, "%"},
            {kDelayOn,  "Delay",    true,    0.f, 1.f,   1.f,  0, ""},
            {kDelayMs,  "Time",     false,  30.f, 2000.f,10.f, 0, "ms"},
            {kFeedback, "Feedback", false,   0.f, 95.f,  1.f,  0, "%"},
            {kLink,     "Link",     true,    0.f, 1.f,   1.f,  0, ""},
        }};
        return c;
    }

    // ── unified, deduped pointer state machine ──
    void pointer_press(vw::Point p, vw::MouseButton button) {
        if (pointer_down_) return;            // dedup: one press, one open
        if (layout_dirty_) layout();
        pointer_down_ = true;
        // A press anywhere commits an open type-in first.
        if (edit_ctl_ >= 0) commit_typein();
        if (button == vw::MouseButton::right && midi_map_) {
            if (in_rect(p, pad_)) arm_learn(kPitch, "pitch (X)");
            else if (in_rect(p, freeze_)) arm_learn(kFreeze, "freeze");
            else learn_armed_ = false;
            return;
        }
        learn_armed_ = false;                 // left-click dismisses the banner
        // Standalone gear → open the SDK Audio/MIDI Settings tab. Only live when
        // hosted in the standalone settings chrome (no-op/hidden in a DAW).
        if (pulp::format::standalone_settings_available(this) && in_rect(p, settings_btn_)) {
            pulp::format::open_standalone_settings(this);
            return;
        }
        if (in_rect(p, freeze_)) {
            toggle_freeze();
            return;
        }
        if (in_rect(p, pad_)) {
            dragging_ = true;
            begin_pad_gesture();
            apply_pad(p);
            return;
        }
        // Control panel: arrows step, toggles flip, value body starts a drag.
        const int N = static_cast<int>(ctls().size());
        for (int i = 0; i < N; ++i) {
            const Ctl& c = ctls()[static_cast<size_t>(i)];
            if (c.is_toggle) {
                if (in_rect(p, ctl_field_[static_cast<size_t>(i)]))
                    ctl_set_oneshot(c, store_.get_value(c.id) >= 0.5f ? 0.f : 1.f);
                continue;
            }
            if (in_rect(p, ctl_down_[static_cast<size_t>(i)])) {
                ctl_set_oneshot(c, store_.get_value(c.id) - c.step); return;
            }
            if (in_rect(p, ctl_up_[static_cast<size_t>(i)])) {
                ctl_set_oneshot(c, store_.get_value(c.id) + c.step); return;
            }
            if (in_rect(p, ctl_field_[static_cast<size_t>(i)])) {
                active_ctl_ = i;
                drag_moved_ = false;
                drag_start_y_ = p.y;
                drag_start_val_ = store_.get_value(c.id);
                ctl_edit_.begin(c.id);
                return;
            }
        }
    }
    void pointer_move(vw::Point p) {
        if (dragging_) { apply_pad(p); return; }
        if (active_ctl_ >= 0) {
            if (std::abs(p.y - drag_start_y_) > 2.0f) drag_moved_ = true;
            const Ctl& c = ctls()[static_cast<size_t>(active_ctl_)];
            // Vertical drag: a full panel-height drag spans the whole range.
            const float span = std::max(40.0f, hud_.height);
            const float dv = (drag_start_y_ - p.y) / span * (c.hi - c.lo);
            ctl_edit_.set(c.id, std::clamp(drag_start_val_ + dv, c.lo, c.hi));
        }
    }
    void pointer_release() {
        if (!pointer_down_) return;           // dedup: ignore the echo up
        pointer_down_ = false;
        if (dragging_) { dragging_ = false; end_pad_gesture(); }
        if (active_ctl_ >= 0) {
            const int i = active_ctl_;
            ctl_edit_.finish();
            active_ctl_ = -1;
            // A click without a drag opens type-in on that field.
            if (!drag_moved_) enter_typein(i);
        }
    }

    // ── numeric type-in ──
    void enter_typein(int i) {
        edit_ctl_ = i;
        claim_input_focus();
        // Pre-fill with the current value, shown "selected": the user sees the
        // original, and the first keystroke replaces it (Logic-style).
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.*f", ctls()[static_cast<size_t>(i)].decimals,
                      static_cast<double>(store_.get_value(ctls()[static_cast<size_t>(i)].id)));
        edit_buf_ = buf;
        typein_fresh_ = true;
    }
    void commit_typein() {
        if (edit_ctl_ < 0) return;
        const Ctl& c = ctls()[static_cast<size_t>(edit_ctl_)];
        if (!edit_buf_.empty() && edit_buf_ != "-" && edit_buf_ != ".")
            ctl_set_oneshot(c, static_cast<float>(std::atof(edit_buf_.c_str())));
        edit_ctl_ = -1;
        edit_buf_.clear();
        typein_fresh_ = false;
        // Hand the keyboard back to the host (Musical Typing etc.) the moment
        // text entry ends. (Tab-advance re-claims via enter_typein.)
        release_input_focus();
    }
    void cancel_typein() {
        edit_ctl_ = -1;
        edit_buf_.clear();
        typein_fresh_ = false;
        release_input_focus();
    }

    // Next numeric (non-toggle) control index for Tab navigation, wrapping
    // around. `reverse` walks backward (Shift+Tab). Returns -1 if there are no
    // numeric fields. `from` may be -1 (no field active yet).
    int next_number_field(int from, bool reverse) const {
        const auto& cs = ctls();
        const int n = static_cast<int>(cs.size());
        for (int step = 1; step <= n; ++step) {
            const int j = ((from + (reverse ? -step : step)) % n + n) % n;
            if (!cs[static_cast<size_t>(j)].is_toggle) return j;
        }
        return -1;
    }

    // ── interaction ── driven through pulp::state::ParameterEdit, which
    // brackets the edit in host gestures (so DAWs record/play back the
    // automation) and caches the in-flight value so the ball follows the
    // cursor regardless of the host's per-block parameter echo.
    void begin_pad_gesture() { edit_.begin({kPitch, kFormant}); }
    void end_pad_gesture() { edit_.finish(); }
    void apply_pad(vw::Point p) {
        const float fx = std::clamp((p.x - pad_.x) / pad_.width, 0.0f, 1.0f);
        const float fy = std::clamp((p.y - pad_.y) / pad_.height, 0.0f, 1.0f);
        edit_.set(kPitch, -12.0f + fx * 24.0f);
        edit_.set(kFormant, 12.0f - fy * 24.0f);
    }
    void toggle_freeze() {
        const float v = store_.get_value(kFreeze) >= 0.5f ? 0.0f : 1.0f;
        pulp::state::ParameterEdit toggle(store_);
        toggle.begin(kFreeze);
        toggle.set(kFreeze, v);
        toggle.finish();
    }
    void arm_learn(pulp::state::ParamID id, const char* name) {
        midi_map_->arm_learn(id);
        learn_armed_ = true;
        learn_name_ = name;
    }
    // Scroll over a control field steps it by one step (up = increase).
    void scroll_over(vw::Point p, float dy) {
        if (layout_dirty_) layout();
        if (dy == 0.0f) return;
        const int N = static_cast<int>(ctls().size());
        for (int i = 0; i < N; ++i) {
            const Ctl& c = ctls()[static_cast<size_t>(i)];
            if (c.is_toggle) continue;
            if (in_rect(p, ctl_field_[static_cast<size_t>(i)])) {
                const float dir = dy > 0.0f ? -1.0f : 1.0f; // wheel-up increases
                ctl_set_oneshot(c, store_.get_value(c.id) + dir * c.step);
                return;
            }
        }
    }
    float shown_pitch() const { return edit_.display_value(kPitch, store_.get_value(kPitch)); }
    float shown_formant() const { return edit_.display_value(kFormant, store_.get_value(kFormant)); }

    // ── painting ──
    void paint_xy_pad(cv::Canvas& canvas) {
        const float s = scale();
        const float radius = 12 * s;
        canvas.set_fill_color(palette::surface);
        canvas.fill_rounded_rect(pad_.x, pad_.y, pad_.width, pad_.height, radius);

        // Clip everything inside the rounded surface so neither the grid nor
        // the crosshair pokes a hard square past the rounded corners.
        canvas.save();
        canvas.clip_rect(pad_.x, pad_.y, pad_.width, pad_.height);

        // Interior grid only — the rounded fill is the boundary, so the outer
        // (±12) lines are skipped to avoid a square frame at the corners.
        canvas.set_line_width(1.0f);
        for (int st = -9; st <= 9; st += 3) {
            const float f = (st + 12) / 24.0f;
            const float x = pad_.x + f * pad_.width;
            const float y = pad_.y + (1.0f - f) * pad_.height;
            canvas.set_stroke_color(st == 0 ? palette::pad_grid_axis : palette::pad_grid);
            canvas.stroke_line(x, pad_.y + radius, x, pad_.bottom() - radius);
            canvas.stroke_line(pad_.x + radius, y, pad_.right() - radius, y);
        }
        canvas.set_fill_color(palette::text_dim);
        canvas.set_font("Inter", 11.0f * s);
        canvas.fill_text("pitch →", pad_.x + 10 * s, pad_.bottom() - 10 * s);
        canvas.fill_text("↑ formant", pad_.x + 10 * s, pad_.y + 20 * s);

        const float bx = pad_.x + ((shown_pitch() + 12.0f) / 24.0f) * pad_.width;
        const float by = pad_.y + (1.0f - (shown_formant() + 12.0f) / 24.0f) * pad_.height;
        // Soft crosshair (low-contrast) + a glow ring + the handle.
        canvas.set_stroke_color(palette::pad_cross);
        canvas.set_line_width(1.0f);
        canvas.stroke_line(pad_.x, by, pad_.right(), by);
        canvas.stroke_line(bx, pad_.y, bx, pad_.bottom());
        canvas.set_fill_color(palette::pad_handle_glow);
        canvas.fill_circle(bx, by, 16 * s);
        canvas.set_fill_color(palette::pad_handle);
        canvas.fill_circle(bx, by, 9 * s);
        canvas.set_stroke_color(palette::pad_handle_ring);
        canvas.set_line_width(2.0f);
        canvas.stroke_circle(bx, by, 9 * s);
        canvas.restore();
    }

    void paint_spectrum(cv::Canvas& canvas) {
        const float s = scale();
        const auto& r = spectrum_rect_;
        canvas.set_fill_color(palette::surface);
        canvas.fill_rounded_rect(r.x, r.y, r.width, r.height, 8 * s);
        canvas.set_fill_color(palette::text_dim);
        canvas.set_font("Inter", 11.0f * s);
        canvas.fill_text("spectrum (log f)", r.x + 8 * s, r.y + 16 * s);

        const int n = kSpectrumBins;
        const SpectrumFrame* frame = spectrum_ ? &spectrum_->read() : nullptr;
        // Ease the displayed magnitudes toward the latest published frame so
        // the curve animates smoothly and looks alive even when frames arrive
        // in bursts (fast rise to catch transients, slower fall).
        for (int i = 0; i < n; ++i) {
            const float db = frame ? (*frame)[static_cast<size_t>(i)] : -120.0f;
            const float target = std::clamp((db + 90.0f) / 90.0f, 0.0f, 1.0f);
            float& v = spec_display_[static_cast<size_t>(i)];
            v += (target > v ? 0.5f : 0.18f) * (target - v);
        }

        canvas.save();
        canvas.clip_rect(r.x, r.y, r.width, r.height);
        const float base = r.bottom() - 6 * s, topY = r.y + 24 * s;
        const float x0 = r.x + 6 * s, xspan = r.width - 12 * s;
        const float inv_logn = 1.0f / std::log10((float)n);
        std::array<cv::Canvas::Point2D, kSpectrumBins + 2> poly{};
        int pc = 0;
        for (int i = 1; i < n; ++i) {
            const float lf = std::log10(1.0f + i) * inv_logn;
            const float x = x0 + lf * xspan;
            const float y = base - spec_display_[static_cast<size_t>(i)] * (base - topY);
            poly[static_cast<size_t>(pc++)] = {x, y};
        }
        // Soft fill under the curve.
        const float xr = x0 + xspan;
        poly[static_cast<size_t>(pc++)] = {xr, base};
        poly[static_cast<size_t>(pc++)] = {x0, base};
        canvas.set_fill_color(palette::spectrum_fill);
        canvas.fill_path(poly.data(), static_cast<size_t>(pc));
        // Line on top — connect the curve points (the last two poly entries
        // are the baseline corners, so stop before them).
        const int curve_pts = pc - 2;
        canvas.set_stroke_color(palette::spectrum_line);
        canvas.set_line_width(1.6f);
        canvas.stroke_path(poly.data(), static_cast<size_t>(curve_pts));
        canvas.restore();
    }

    // Right-aligned "⚙ Settings" button on the title row. Shown ONLY in the
    // standalone (a TabPanel/settings ancestor exists); in a DAW the host owns
    // audio + MIDI routing so there is nothing to open. Click switches the
    // standalone chrome to its Audio/MIDI Settings tab (SDK-provided).
    void paint_settings_button(cv::Canvas& canvas) {
        if (!pulp::format::standalone_settings_available(this)) return;
        const float s = scale();
        const auto& b = settings_btn_;
        canvas.set_fill_color(palette::elevated);
        canvas.fill_rounded_rect(b.x, b.y, b.width, b.height, 7 * s);
        canvas.set_fill_color(palette::text);
        canvas.set_font("Inter", 13.0f * s);
        canvas.fill_text("⚙ Settings", b.x + 15 * s, b.y + b.height * 0.66f);
    }

    void paint_freeze(cv::Canvas& canvas) {
        const float s = scale();
        const bool frozen = store_.get_value(kFreeze) >= 0.5f;
        canvas.set_fill_color(frozen ? palette::warn : palette::elevated);
        canvas.fill_rounded_rect(freeze_.x, freeze_.y, freeze_.width, freeze_.height, 8 * s);
        canvas.set_fill_color(frozen ? palette::bg : palette::text);
        canvas.set_font("Inter", 14.0f * s);
        canvas.fill_text(frozen ? "● FROZEN" : "FREEZE",
                         freeze_.x + 16 * s, freeze_.y + freeze_.height * 0.62f);
    }

    // Current display value of a control (the live drag value while editing
    // its field, else the store — which tracks host automation).
    float ctl_value(int i) const {
        const Ctl& c = ctls()[static_cast<size_t>(i)];
        if (i == active_ctl_) return ctl_edit_.display_value(c.id, store_.get_value(c.id));
        return store_.get_value(c.id);
    }
    void ctl_set_oneshot(const Ctl& c, float v) {
        v = std::clamp(v, c.lo, c.hi);
        pulp::state::ParameterEdit e(store_);
        e.begin(c.id); e.set(c.id, v); e.finish();
    }

    void paint_controls(cv::Canvas& canvas) {
        const float s = scale();
        canvas.set_fill_color(palette::surface);
        canvas.fill_rounded_rect(hud_.x, hud_.y, hud_.width, hud_.height, 8 * s);
        const int N = static_cast<int>(ctls().size());
        for (int i = 0; i < N; ++i) {
            const Ctl& c = ctls()[static_cast<size_t>(i)];
            const vw::Rect& fld = ctl_field_[static_cast<size_t>(i)];
            // Label, right-aligned to the field.
            canvas.set_fill_color(palette::text_dim);
            canvas.set_font("Inter", 12.0f * s);
            canvas.fill_text(c.label, hud_.x + 12 * s, fld.y + fld.height * 0.72f);
            if (c.is_toggle) {
                const bool on = store_.get_value(c.id) >= 0.5f;
                const float bs = fld.height;
                canvas.set_fill_color(on ? palette::pad_handle : palette::elevated);
                canvas.fill_rounded_rect(fld.x, fld.y, bs, bs, 4 * s);
                if (on) {
                    canvas.set_stroke_color(palette::bg);
                    canvas.set_line_width(2.0f);
                    canvas.stroke_line(fld.x + bs*0.28f, fld.y + bs*0.52f, fld.x + bs*0.44f, fld.y + bs*0.70f);
                    canvas.stroke_line(fld.x + bs*0.44f, fld.y + bs*0.70f, fld.x + bs*0.74f, fld.y + bs*0.32f);
                }
                continue;
            }
            // Field background (highlighted while typing) + ‹ value ›.
            const bool editing = (i == edit_ctl_);
            canvas.set_fill_color(editing ? palette::pad_handle_glow : palette::elevated);
            canvas.fill_rounded_rect(fld.x, fld.y, fld.width, fld.height, 4 * s);
            if (editing) {
                canvas.set_stroke_color(palette::pad_handle);
                canvas.set_line_width(1.5f);
                canvas.stroke_rounded_rect(fld.x, fld.y, fld.width, fld.height, 4 * s);
            }
            canvas.set_fill_color(palette::text_dim);
            canvas.set_font("Inter", 13.0f * s);
            const vw::Rect& dn = ctl_down_[static_cast<size_t>(i)];
            const vw::Rect& up = ctl_up_[static_cast<size_t>(i)];
            canvas.fill_text("‹", dn.x + dn.width * 0.32f, dn.y + dn.height * 0.70f);
            canvas.fill_text("›", up.x + up.width * 0.40f, up.y + up.height * 0.70f);
            char buf[40];
            const float tx = fld.x + fld.width * 0.28f, ty = fld.y + fld.height * 0.70f;
            if (editing && typein_fresh_) {
                // Original value, shown "selected" (highlighted) — the next
                // keystroke replaces it.
                std::snprintf(buf, sizeof buf, "%s", edit_buf_.c_str());
                const float tw = static_cast<float>(edit_buf_.size()) * 8.0f * s + 4 * s;
                canvas.set_fill_color(palette::pad_handle);
                canvas.fill_rounded_rect(tx - 2 * s, fld.y + fld.height * 0.18f,
                                         tw, fld.height * 0.64f, 3 * s);
                canvas.set_fill_color(palette::bg);
            } else if (editing) {
                std::snprintf(buf, sizeof buf, "%s_", edit_buf_.c_str());
                canvas.set_fill_color(palette::text);
            } else {
                std::snprintf(buf, sizeof buf, "%.*f%s%s", c.decimals,
                              static_cast<double>(ctl_value(i)),
                              c.unit[0] ? " " : "", c.unit);
                canvas.set_fill_color(palette::text);
            }
            canvas.set_font("Inter", 13.0f * s);
            canvas.fill_text(buf, tx, ty);
        }
        // Latency strip.
        canvas.set_fill_color(palette::success);
        canvas.set_font("Inter", 11.0f * s);
        char lat[48];
        const float ms = latency_ms_ ? latency_ms_->load(std::memory_order_relaxed) : 0.0f;
        std::snprintf(lat, sizeof lat, "latency  %.1f ms", static_cast<double>(ms));
        canvas.fill_text(lat, hud_.x + 12 * s, hud_.bottom() - 7 * s);
    }

    void paint_learn_banner(cv::Canvas& canvas) {
        // Clear the banner once the audio thread has consumed the CC.
        if (learn_armed_ && midi_map_ && !midi_map_->learn_armed() && learn_seen_armed_)
            learn_armed_ = false;
        if (learn_armed_ && midi_map_ && midi_map_->learn_armed())
            learn_seen_armed_ = true;
        if (!learn_armed_) return;
        const float s = scale();
        const float W = local_bounds().width;
        const float bw = std::min(W - 40 * s, 360 * s), bh = 34 * s;
        const float bx = (W - bw) * 0.5f, by = 8 * s;
        canvas.set_fill_color(palette::warn);
        canvas.fill_rounded_rect(bx, by, bw, bh, 8 * s);
        canvas.set_fill_color(palette::bg);
        canvas.set_font("Inter", 13.0f * s);
        char line[96];
        std::snprintf(line, sizeof line, "MIDI LEARN: %s — move a CC", learn_name_);
        canvas.fill_text(line, bx + 14 * s, by + bh * 0.64f);
    }

    pulp::state::StateStore& store_;
    SpectrumBus* spectrum_ = nullptr;
    pulp::state::MidiParameterMap* midi_map_ = nullptr;
    const std::atomic<float>* latency_ms_ = nullptr;
    pulp::state::ParameterEdit edit_;
    pulp::state::ParameterEdit ctl_edit_{store_};
    vw::Rect pad_{}, spectrum_rect_{}, freeze_{}, hud_{}, settings_btn_{};
    std::array<vw::Rect, 7> ctl_field_{}, ctl_down_{}, ctl_up_{};
    int active_ctl_ = -1;
    bool drag_moved_ = false;
    int edit_ctl_ = -1;
    bool typein_fresh_ = false;   // value shown "selected"; first key replaces
    std::string edit_buf_;
    float drag_start_y_ = 0.0f, drag_start_val_ = 0.0f;
    bool layout_dirty_ = true;
    bool pointer_down_ = false;
    bool dragging_ = false;
    std::array<float, kSpectrumBins> spec_display_{};
    bool learn_armed_ = false;
    bool learn_seen_armed_ = false;
    const char* learn_name_ = "";
};

} // namespace bendr
