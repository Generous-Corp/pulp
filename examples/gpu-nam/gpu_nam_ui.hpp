#pragma once

// Native GPU UI for the GPU NAM example, a faithful recreation of
// NeuralAmpModelerPlugin's editor built entirely in Pulp's view/canvas stack and
// rendered through Skia/Dawn. The image + font assets are reused from the plugin
// under their own licenses (see assets/nam/ATTRIBUTION.md); none of the plugin's
// iPlug2/IGraphics UI code is used — the layout, painting, and interaction here
// are an independent Pulp implementation.
//
// Layout mirrors NeuralAmpModeler.cpp's mLayoutFunc in a 600×400 design space
// (its Background art size): a title band, a six-knob row (Input · Gate · Bass ·
// Middle · Treble · Output), Noise-Gate and EQ slide switches under their knobs,
// input/output edge meters, and Model + IR file slots. A settings gear reveals
// this demo's GPU-specific controls (audio Engine CPU/GPU + Bypass + live GPU
// status), keeping them off the faithful face panel. Pointer input drives real
// parameters through host gestures (begin/set/finish) so edits stick and record.

#include "gpu_nam_processor.hpp"
#include "gpu_nam_paths.hpp"
#include <pulp/state/parameter_edit.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/file_chooser.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/bundled_fonts.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulp::examples {

namespace cv = pulp::canvas;
namespace vw = pulp::view;

// NAM's mLayoutFunc geometry, in its 600×400 design space (all values in design
// units; the view scales uniformly to the host window). Derived independently
// from the documented IRECT math, not copied from iPlug2 code.
namespace nam_geom {
inline constexpr float kW = 600.0f, kH = 400.0f;
inline constexpr float kContentL = 30.0f, kContentT = 30.0f;
inline constexpr float kContentR = 570.0f, kContentB = 370.0f;
inline constexpr float kTitleH = 50.0f;
inline constexpr int   kNumKnobs = 6;
inline constexpr float kKnobAreaL = 50.0f, kKnobAreaR = 550.0f;
inline constexpr float kKnobCy = 152.0f, kKnobR = 33.0f;
inline constexpr float kKnobLabelY = 110.0f, kKnobValueY = 202.0f;
inline constexpr float kToggleY = 232.0f, kToggleW = 38.0f, kToggleH = 17.0f;
inline constexpr float kToggleLabelY = 268.0f;
// File rows span most of the content width like NAM: a type icon sits to the
// LEFT of a wide FileBackground field; the field carries a left-aligned
// folder + ‹ › browse cluster, centered path text, and a right-edge globe.
inline constexpr float kFileIconL = 32.0f;   // type icon (model / IR), left of field
inline constexpr float kFileFieldL = 58.0f;  // FileBackground left edge
inline constexpr float kFileR = 560.0f;      // FileBackground right edge
inline constexpr float kModelT = 312.0f, kFileH = 30.0f, kIrDy = 38.0f;
inline constexpr float kMeterInL = 10.0f, kMeterOutL = 560.0f;
inline constexpr float kMeterT = 86.0f, kMeterW = 30.0f, kMeterH = 178.0f;
// Settings gear: inside the panel on the title line, right of the title (NAM
// places it there, not in the window corner).
inline constexpr float kGearX = 534.0f, kGearY = 46.0f, kGearSz = 20.0f;
inline float knob_cx(int i) {
    const float cw = (kKnobAreaR - kKnobAreaL) / kNumKnobs;
    return kKnobAreaL + (static_cast<float>(i) + 0.5f) * cw;
}
}  // namespace nam_geom

// NAM's palette, sampled from the reference editor.
struct GnColors {
    cv::Color panel      = cv::Color::rgba8(25, 25, 25);     // #191919 letterbox
    cv::Color accent     = cv::Color::rgba8(93, 131, 226);   // #5D83E2 cornflower blue
    cv::Color accent_dim = cv::Color::rgba8(74, 104, 190);
    cv::Color text       = cv::Color::rgba8(214, 214, 218);
    cv::Color text_dim   = cv::Color::rgba8(150, 150, 156);
    cv::Color arc_track  = cv::Color::rgba8(52, 52, 60);
    cv::Color toggle_off = cv::Color::rgba8(72, 72, 80);
    cv::Color overlay    = cv::Color::rgba8(12, 12, 14);
};

class GpuNamUi : public vw::View {
public:
    // The spectrum bus is accepted for a stable create_view() signature but is
    // unused: NeuralAmpModeler's face has no spectrum display, so neither does
    // this faithful recreation.
    GpuNamUi(pulp::state::StateStore& store,
             [[maybe_unused]] pulp::examples::SpectrumBus& spectrum,
             pulp::examples::GpuNamProcessor& proc)
        : store_(store), proc_(proc), edit_(store) {
        asset_dir_ = gpu_nam_asset_dir();
        ensure_fonts();
        set_continuous_repaint(true);
        set_requires_gpu_host(true);
    }

    void paint(cv::Canvas& canvas) override {
        compute_transform();
        // Letterbox fill behind the fixed-aspect design surface.
        canvas.set_fill_color(colors_.panel);
        canvas.fill_rect(0, 0, local_bounds().width, local_bounds().height);

        paint_background(canvas);
        paint_title(canvas);
        paint_meters(canvas);
        paint_knobs(canvas);
        paint_toggles(canvas);
        paint_file_slots(canvas);
        paint_gear(canvas);
        if (show_settings_) paint_settings(canvas);
        request_repaint();
    }

    void on_mouse_event(const vw::MouseEvent& e) override {
        switch (e.phase) {
            case vw::MousePhase::press:   pointer_press(e.position); break;
            case vw::MousePhase::drag:    pointer_move(e.position); break;
            case vw::MousePhase::release: pointer_release(); break;
            case vw::MousePhase::hover:   break;
            case vw::MousePhase::automatic:
                if (e.is_down) { pointer_down_ ? pointer_move(e.position) : pointer_press(e.position); }
                break;
        }
    }
    void on_mouse_down(vw::Point p) override { pointer_press(p); }
    void on_mouse_drag(vw::Point p) override { pointer_move(p); }
    void on_mouse_up(vw::Point) override { pointer_release(); }

    // Open/close the settings overlay programmatically (screenshots, tests).
    void show_settings(bool on) { show_settings_ = on; }

    // Test accessors (headless interaction verification).
    vw::Point knob_center_for_test(int i) {
        compute_transform();
        return {sx(nam_geom::knob_cx(i)), sy(nam_geom::kKnobCy)};
    }
    vw::Point toggle_center_for_test(int which) {  // 0 = noise gate, 1 = EQ
        compute_transform();
        const int knob = which == 0 ? 1 : 3;
        return {sx(nam_geom::knob_cx(knob)), sy(nam_geom::kToggleY + nam_geom::kToggleH * 0.5f)};
    }
    vw::Point gear_center_for_test() {
        compute_transform();
        return {sx(nam_geom::kGearX + nam_geom::kGearSz * 0.5f),
                sy(nam_geom::kGearY + nam_geom::kGearSz * 0.5f)};
    }

private:
    struct KnobSpec { pulp::state::ParamID id; const char* label; float lo, hi; const char* unit; };
    static constexpr std::array<KnobSpec, nam_geom::kNumKnobs> kKnobs{{
        {kInputGain,           "Input",     -20.0f,  20.0f, "dB"},
        {kNoiseGateThreshold,  "Threshold", -100.0f,  0.0f, "dB"},
        {kToneBass,            "Bass",         0.0f, 10.0f, ""},
        {kToneMiddle,          "Middle",       0.0f, 10.0f, ""},
        {kToneTreble,          "Treble",       0.0f, 10.0f, ""},
        {kOutputGain,          "Output",     -40.0f, 40.0f, "dB"},
    }};

    // ── design→screen transform (uniform scale + centering) ──
    void compute_transform() {
        const float W = local_bounds().width, H = local_bounds().height;
        scale_ = std::min(W / nam_geom::kW, H / nam_geom::kH);
        ox_ = (W - nam_geom::kW * scale_) * 0.5f;
        oy_ = (H - nam_geom::kH * scale_) * 0.5f;
    }
    float sx(float dx) const { return ox_ + dx * scale_; }
    float sy(float dy) const { return oy_ + dy * scale_; }
    float ss(float v) const { return v * scale_; }

    // ── asset loading ──
    void ensure_fonts() {
        if (fonts_done_ || asset_dir_.empty()) return;
        fonts_done_ = true;
        cv::register_font_file(asset_dir_ + "/Michroma-Regular.ttf", "Michroma");
        cv::register_font_file(asset_dir_ + "/Roboto-Regular.ttf", "Roboto");
    }
    std::string img(const char* name) const { return asset_dir_.empty() ? "" : asset_dir_ + "/" + name; }
    const std::string& svg(const char* name) {
        auto it = svg_cache_.find(name);
        if (it != svg_cache_.end()) return it->second;
        std::string content;
        if (!asset_dir_.empty()) {
            std::ifstream f(asset_dir_ + "/" + name, std::ios::binary);
            std::ostringstream ss; ss << f.rdbuf(); content = ss.str();
        }
        return svg_cache_.emplace(name, std::move(content)).first->second;
    }

    // ── painters ──
    void paint_background(cv::Canvas& canvas) {
        canvas.draw_image_from_file(img("Background.jpg"), sx(0), sy(0),
                                    ss(nam_geom::kW), ss(nam_geom::kH));
    }

    void paint_title(cv::Canvas& canvas) {
        // Neutral product title in Michroma (NOT NAM's wordmark); we state
        // factually that this plays NAM .nam captures.
        canvas.set_fill_color(colors_.text);
        canvas.set_font("Michroma", ss(19.0f));
        canvas.set_text_align(cv::TextAlign::center);
        const float cy = nam_geom::kContentT + nam_geom::kTitleH * 0.5f + 7.0f;
        canvas.fill_text("GPU NAM", sx(nam_geom::kW * 0.5f), sy(cy));
        canvas.set_text_align(cv::TextAlign::left);
    }

    void paint_gear(cv::Canvas& canvas) {
        const auto& s = svg("Gear.svg");
        if (!s.empty())
            canvas.draw_svg(s, sx(nam_geom::kGearX), sy(nam_geom::kGearY),
                            ss(nam_geom::kGearSz), ss(nam_geom::kGearSz));
    }

    void paint_knobs(cv::Canvas& canvas) {
        for (int i = 0; i < nam_geom::kNumKnobs; ++i) paint_knob(canvas, i);
    }

    void paint_knob(cv::Canvas& canvas, int i) {
        const KnobSpec& k = kKnobs[static_cast<std::size_t>(i)];
        const float cx = nam_geom::knob_cx(i), cy = nam_geom::kKnobCy, R = nam_geom::kKnobR;
        const float scx = sx(cx), scy = sy(cy), sR = ss(R);

        // Label above the knob.
        canvas.set_fill_color(colors_.text_dim);
        canvas.set_font("Roboto", ss(10.5f));
        canvas.set_text_align(cv::TextAlign::center);
        canvas.fill_text(k.label, scx, sy(nam_geom::kKnobLabelY));

        // Knob face (reused bitmap) + value arc + indicator.
        canvas.draw_image_from_file(img("KnobBackground.png"), scx - sR, scy - sR, 2 * sR, 2 * sR);

        const float frac = knob_frac(i);
        constexpr float kSweep = 135.0f;  // ±135° → 270° travel
        const float a0 = -kSweep * 3.14159265f / 180.0f;
        const float a1 = (-kSweep + frac * 2.0f * kSweep) * 3.14159265f / 180.0f;
        // Value arc: a thin, delicate ring hugging the cap. The reference's ring
        // is subtle — a near-invisible dark track with a fine accent sweep — so
        // keep the stroke light and the track very dim.
        const float arc_r = sR + ss(1.0f);
        stroke_arc_points(canvas, scx, scy, arc_r, a0,  kSweep * 3.14159265f / 180.0f,
                          colors_.arc_track, ss(1.7f));
        stroke_arc_points(canvas, scx, scy, arc_r, a0, a1, colors_.accent, ss(1.7f));

        // Indicator dot set inside the cap (not riding the rim), like NAM.
        const float ix = scx + std::sin(a1) * sR * 0.52f;
        const float iy = scy - std::cos(a1) * sR * 0.52f;
        canvas.set_fill_color(colors_.accent);
        canvas.fill_circle(ix, iy, ss(2.1f));

        // Value readout below.
        char buf[40];
        const float val = knob_value(i);
        if (k.unit[0]) std::snprintf(buf, sizeof buf, "%.1f %s", static_cast<double>(val), k.unit);
        else           std::snprintf(buf, sizeof buf, "%.1f", static_cast<double>(val));
        canvas.set_fill_color(colors_.text);
        canvas.set_font("Roboto", ss(10.5f));
        canvas.fill_text(buf, scx, sy(nam_geom::kKnobValueY));
        canvas.set_text_align(cv::TextAlign::left);
    }

    // Stroke a circular arc as a polyline. `a_start`/`a_end` are radians measured
    // from 12 o'clock, positive clockwise (screen coords). Robust across backends
    // (no arc-direction convention dependency).
    void stroke_arc_points(cv::Canvas& canvas, float cx, float cy, float r,
                           float a_start, float a_end, cv::Color color, float width) {
        constexpr int kSeg = 48;
        std::array<cv::Canvas::Point2D, kSeg + 1> pts{};
        for (int s = 0; s <= kSeg; ++s) {
            const float a = a_start + (a_end - a_start) * static_cast<float>(s) / kSeg;
            pts[static_cast<std::size_t>(s)] = {cx + std::sin(a) * r, cy - std::cos(a) * r};
        }
        canvas.set_stroke_color(color);
        canvas.set_line_width(width);
        canvas.stroke_path(pts.data(), pts.size());
    }

    void paint_toggles(cv::Canvas& canvas) {
        paint_toggle(canvas, 1, kNoiseGateActive, "Noise Gate");  // under Threshold
        paint_toggle(canvas, 3, kEQActive, "EQ");                 // under Middle
    }

    void paint_toggle(cv::Canvas& canvas, int under_knob, pulp::state::ParamID id,
                      const char* label) {
        const bool on = store_.get_value(id) >= 0.5f;
        const float cx = nam_geom::knob_cx(under_knob);
        const float w = nam_geom::kToggleW, h = nam_geom::kToggleH;
        const float x = cx - w * 0.5f, y = nam_geom::kToggleY;
        canvas.set_fill_color(on ? colors_.accent : colors_.toggle_off);
        canvas.fill_rounded_rect(sx(x), sy(y), ss(w), ss(h), ss(h * 0.5f));
        // Handle (reused bitmap) slides to the lit side.
        const float hd = h - 4.0f;
        const float hx = on ? (x + w - hd - 2.0f) : (x + 2.0f);
        canvas.draw_image_from_file(img("SlideSwitchHandle.png"),
                                    sx(hx), sy(y + 2.0f), ss(hd), ss(hd));
        // Label below the switch.
        canvas.set_fill_color(colors_.text_dim);
        canvas.set_font("Roboto", ss(10.5f));
        canvas.set_text_align(cv::TextAlign::center);
        canvas.fill_text(label, sx(cx), sy(nam_geom::kToggleLabelY));
        canvas.set_text_align(cv::TextAlign::left);
    }

    void paint_meters(cv::Canvas& canvas) {
        const auto [in_db, out_db] = proc_.meter_levels_db();
        paint_meter(canvas, nam_geom::kMeterInL, in_db);
        paint_meter(canvas, nam_geom::kMeterOutL, out_db);
    }

    void paint_meter(cv::Canvas& canvas, float x, float level_db) {
        const float y = nam_geom::kMeterT, w = nam_geom::kMeterW, h = nam_geom::kMeterH;
        canvas.draw_image_from_file(img("MeterBackground.png"), sx(x), sy(y), ss(w), ss(h));
        const float pad = 4.0f;
        const float bx = x + pad, bw = w - 2 * pad;
        // Map −48..0 dB → 0..1 of the bar height, filling from the bottom. The
        // −48 dB floor keeps the meter dark at rest (matching NAM's idle look).
        const float frac = std::clamp((level_db + 48.0f) / 48.0f, 0.0f, 1.0f);
        if (frac > 0.001f) {
            const float bar_h = (h - 2 * pad) * frac;
            const float by = y + h - pad - bar_h;
            // Green→amber→red toward the top, like NAM's meter.
            cv::Color c = level_db > -3.0f  ? cv::Color::rgba8(226, 96, 80)
                        : level_db > -12.0f ? cv::Color::rgba8(230, 184, 90)
                                            : colors_.accent;
            canvas.set_fill_color(c);
            canvas.fill_rounded_rect(sx(bx), sy(by), ss(bw), ss(bar_h), ss(2.0f));
        }
        // Accent baseline at the meter's foot — present even at rest, like NAM's
        // blue floor line under each meter.
        canvas.set_fill_color(colors_.accent);
        canvas.fill_rect(sx(bx), sy(y + h - pad - 1.5f), ss(bw), ss(1.5f));
    }

    void paint_file_slots(cv::Canvas& canvas) {
        // Show the model path only once a user picks one; the bundled default
        // reads as NAM's first-open prompt (faithful to the reference default).
        const std::string model_label = proc_.user_model_loaded()
                                            ? proc_.model_name()
                                            : "Select model directory...";
        model_slot_ = paint_file_slot(canvas, nam_geom::kModelT, "ModelIcon.svg",
                                      model_label, proc_.user_model_loaded());
        const bool ir_loaded = proc_.user_ir_loaded();
        const std::string ir_label = ir_loaded ? proc_.ir_name() : "Select IR directory...";
        ir_slot_ = paint_file_slot(canvas, nam_geom::kModelT + nam_geom::kIrDy,
                                   ir_loaded ? "IRIconOn.svg" : "IRIconOff.svg",
                                   ir_label, ir_loaded);
    }

    vw::Rect paint_file_slot(cv::Canvas& canvas, float top, const char* type_icon,
                             const std::string& label, bool loaded) {
        const float fx = nam_geom::kFileFieldL;
        const float w = nam_geom::kFileR - fx;
        const float h = nam_geom::kFileH;
        const float mid = top + h * 0.5f;
        auto draw_icon = [&](const char* name, float ix, float iy, float sw, float sh) {
            const auto& s = svg(name);
            if (!s.empty()) canvas.draw_svg(s, sx(ix), sy(iy), ss(sw), ss(sh));
        };

        // Type icon (model / IR) sits to the LEFT of the field, like NAM.
        draw_icon(type_icon, nam_geom::kFileIconL, mid - 8.0f, 20.0f, 16.0f);

        // Wide FileBackground field.
        canvas.draw_image_from_file(img("FileBackground.png"), sx(fx), sy(top), ss(w), ss(h));

        // Left-aligned browse cluster inside the field: folder · ‹ · ›. The arrow
        // glyphs fill only ~35% of their 800px viewBox, so draw them oversized to
        // read as visible chevrons at this scale.
        draw_icon("File.svg",       fx + 10.0f, mid - 7.5f, 15.0f, 15.0f);
        draw_icon("ArrowLeft.svg",  fx + 28.0f, mid - 12.0f, 24.0f, 24.0f);
        draw_icon("ArrowRight.svg", fx + 44.0f, mid - 12.0f, 24.0f, 24.0f);
        // Globe at the right edge.
        draw_icon("Globe.svg", nam_geom::kFileR - 22.0f, mid - 7.5f, 15.0f, 15.0f);

        // Centered path text (loaded model name, else NAM's placeholder prompt).
        canvas.set_fill_color(loaded ? colors_.text : colors_.text_dim);
        canvas.set_font("Roboto", ss(11.0f));
        canvas.set_text_align(cv::TextAlign::center);
        canvas.fill_text(label, sx(fx + w * 0.5f + 20.0f), sy(mid + 3.8f));
        canvas.set_text_align(cv::TextAlign::left);
        return {sx(fx), sy(top), ss(w), ss(h)};
    }

    // Native file pickers for the model / IR slots (modal on the UI thread on
    // macOS, so capturing the processor by reference is safe).
    void open_model_chooser() {
        vw::FileChooser chooser;
        chooser.set_title("Load NAM model")
               .add_extension_filter("NAM capture (.nam)", "nam");
        // Default to the installed sample models so the user lands on them.
        if (const std::string dir = gpu_nam_content_subdir("Models"); !dir.empty())
            chooser.set_initial_directory(dir);
        auto& proc = proc_;
        chooser.open([&proc](std::vector<std::string> paths) {
            if (!paths.empty()) proc.load_model(paths.front());
        });
    }
    void open_ir_chooser() {
        vw::FileChooser chooser;
        chooser.set_title("Load cabinet impulse response")
               .add_extension_filter("Impulse response (WAV/AIFF/FLAC)", "wav;aiff;aif;flac");
        if (const std::string dir = gpu_nam_content_subdir("Cabinets"); !dir.empty())
            chooser.set_initial_directory(dir);
        auto& proc = proc_;
        chooser.open([&proc](std::vector<std::string> paths) {
            if (!paths.empty()) proc.load_ir(paths.front());
        });
    }

    // ── settings page (this demo's GPU controls) ──
    // A full-panel page in the same structure as the reference's settings view:
    // a SETTINGS title, sectioned control rows with help text, and Model / About
    // blocks along the bottom. The reference's page carries output-mode +
    // input-calibration; ours carries the GPU demo's Engine + Bypass, since that
    // is what this demo adds.
    void paint_settings(cv::Canvas& canvas) {
        const float x = 34.0f, y = 34.0f;
        const float w = nam_geom::kW - 68.0f, h = nam_geom::kH - 68.0f;
        // Dim the face, then draw the page.
        canvas.set_fill_color(colors_.overlay.with_alpha(0.55f));
        canvas.fill_rect(sx(0), sy(0), ss(nam_geom::kW), ss(nam_geom::kH));
        canvas.set_fill_color(cv::Color::rgba8(20, 20, 22).with_alpha(0.985f));
        canvas.fill_rounded_rect(sx(x), sy(y), ss(w), ss(h), ss(8.0f));
        canvas.set_stroke_color(colors_.accent);
        canvas.set_line_width(ss(1.0f));
        canvas.stroke_rounded_rect(sx(x), sy(y), ss(w), ss(h), ss(8.0f));

        // Title (Michroma, like the reference's settings title).
        canvas.set_fill_color(colors_.text);
        canvas.set_font("Michroma", ss(22.0f));
        canvas.set_text_align(cv::TextAlign::center);
        canvas.fill_text("SETTINGS", sx(nam_geom::kW * 0.5f), sy(y + 42.0f));
        canvas.set_text_align(cv::TextAlign::left);

        // Close (×) top-right.
        settings_close_ = {sx(x + w - 34.0f), sy(y + 14.0f), ss(24.0f), ss(24.0f)};
        canvas.set_fill_color(colors_.text_dim);
        canvas.set_font("Roboto", ss(20.0f));
        canvas.set_text_align(cv::TextAlign::center);
        canvas.fill_text("\xC3\x97", sx(x + w - 22.0f), sy(y + 30.0f));  // ×
        canvas.set_text_align(cv::TextAlign::left);

        const float rowL = x + 40.0f, rowW = w - 80.0f;
        const float segW = (rowW - 14.0f) * 0.5f;
        float cy = y + 84.0f;

        // Audio engine — a two-option selector (parallels the reference's
        // Output-Mode radio).
        section_label(canvas, rowL, cy, "AUDIO ENGINE");
        cy += 12.0f;
        const bool gpu = store_.get_value(kEngine) >= 0.5f;
        settings_engine_cpu_ = seg(canvas, rowL, cy, segW, "CPU oracle", !gpu);
        settings_engine_gpu_ = seg(canvas, rowL + segW + 14.0f, cy, segW, "GPU engine", gpu);
        cy += 40.0f;
        help(canvas, rowL, cy,
             "Runs the neural amp on the CPU (always available) or the GPU");
        help(canvas, rowL, cy + 15.0f, "(opt-in, bit-exact against the CPU oracle).");
        cy += 44.0f;

        // Bypass — a switch row (parallels the reference's Calibrate-Input switch).
        section_label(canvas, rowL, cy, "BYPASS");
        cy += 12.0f;
        const bool byp = store_.get_value(kBypass) >= 0.5f;
        settings_bypass_ = seg(canvas, rowL, cy, segW, byp ? "Bypassed" : "Active", byp);
        cy += 40.0f;
        help(canvas, rowL, cy, "Passes the dry input through, unprocessed.");

        // Bottom: Model (left) + About (right), like the reference's Model-Info /
        // About blocks.
        const float by = y + h - 74.0f;
        section_label(canvas, rowL, by, "MODEL");
        canvas.set_fill_color(colors_.text);
        canvas.set_font("Roboto", ss(11.0f));
        canvas.fill_text(proc_.model_name(), sx(rowL), sy(by + 22.0f));
        canvas.set_fill_color(colors_.text_dim);
        canvas.set_font("Roboto", ss(10.0f));
        canvas.fill_text("WaveNet .nam capture", sx(rowL), sy(by + 38.0f));

        const float ax = x + w * 0.5f + 20.0f;
        section_label(canvas, ax, by, "ABOUT");
        canvas.set_fill_color(colors_.text);
        canvas.set_font("Roboto", ss(11.0f));
        canvas.fill_text("GPU NAM \xC2\xB7 Pulp GPU audio demo", sx(ax), sy(by + 22.0f));
        const auto g = proc_.gpu_status();
        char buf[96];
        if (g.active)
            std::snprintf(buf, sizeof buf, "GPU %s \xC2\xB7 %.0f%% RT",
                          g.backend.empty() ? "on" : g.backend.c_str(), g.rt_percent);
        else
            std::snprintf(buf, sizeof buf, "GPU idle (CPU oracle live)");
        canvas.set_fill_color(g.active ? colors_.accent : colors_.text_dim);
        canvas.set_font("Roboto", ss(10.0f));
        canvas.fill_text(buf, sx(ax), sy(by + 38.0f));
    }

    void section_label(cv::Canvas& canvas, float x, float y, const char* text) {
        canvas.set_fill_color(colors_.text_dim);
        canvas.set_font("Roboto", ss(9.5f));
        canvas.set_text_align(cv::TextAlign::left);
        canvas.fill_text(text, sx(x), sy(y));
    }
    void help(cv::Canvas& canvas, float x, float y, const char* text) {
        canvas.set_fill_color(colors_.text_dim);
        canvas.set_font("Roboto", ss(9.5f));
        canvas.set_text_align(cv::TextAlign::left);
        canvas.fill_text(text, sx(x), sy(y));
    }
    // A selectable segment: lit = accent fill, else dim outline. Returns its rect.
    vw::Rect seg(cv::Canvas& canvas, float x, float y, float w, const std::string& text, bool on) {
        const float h = 28.0f;
        canvas.set_fill_color(on ? colors_.accent : colors_.toggle_off);
        canvas.fill_rounded_rect(sx(x), sy(y), ss(w), ss(h), ss(6.0f));
        canvas.set_fill_color(on ? colors_.panel : colors_.text);
        canvas.set_font("Roboto", ss(12.0f));
        canvas.set_text_align(cv::TextAlign::center);
        canvas.fill_text(text, sx(x + w * 0.5f), sy(y + h * 0.64f));
        canvas.set_text_align(cv::TextAlign::left);
        return {sx(x), sy(y), ss(w), ss(h)};
    }

    // ── interaction ──
    static bool in_rect(vw::Point p, const vw::Rect& r) {
        return p.x >= r.x && p.x <= r.x + r.width && p.y >= r.y && p.y <= r.y + r.height;
    }
    static bool in_circle(vw::Point p, float cx, float cy, float r) {
        const float dx = p.x - cx, dy = p.y - cy;
        return dx * dx + dy * dy <= r * r;
    }

    void pointer_press(vw::Point p) {
        if (pointer_down_) return;
        compute_transform();
        pointer_down_ = true;

        if (show_settings_) {
            if (in_rect(p, settings_close_)) { show_settings_ = false; return; }
            if (in_rect(p, settings_engine_cpu_)) { set_param(kEngine, 0.0f); return; }
            if (in_rect(p, settings_engine_gpu_)) { set_param(kEngine, 1.0f); return; }
            if (in_rect(p, settings_bypass_)) { toggle_param(kBypass); return; }
            // A click outside the page closes it; clicks inside are inert.
            const vw::Rect page{sx(34.0f), sy(34.0f),
                                ss(nam_geom::kW - 68.0f), ss(nam_geom::kH - 68.0f)};
            if (!in_rect(p, page)) show_settings_ = false;
            return;
        }
        if (in_circle(p, sx(nam_geom::kGearX + nam_geom::kGearSz * 0.5f),
                      sy(nam_geom::kGearY + nam_geom::kGearSz * 0.5f), ss(18.0f))) {
            show_settings_ = true; return;
        }
        // File slots open the native pickers.
        if (in_rect(p, model_slot_)) { open_model_chooser(); return; }
        if (in_rect(p, ir_slot_)) { open_ir_chooser(); return; }
        // Toggles.
        if (hit_toggle(p, 1, kNoiseGateActive)) return;
        if (hit_toggle(p, 3, kEQActive)) return;
        // Knobs (vertical drag).
        for (int i = 0; i < nam_geom::kNumKnobs; ++i) {
            if (in_circle(p, sx(nam_geom::knob_cx(i)), sy(nam_geom::kKnobCy), ss(nam_geom::kKnobR + 6.0f))) {
                active_knob_ = i;
                drag_start_y_ = p.y;
                drag_start_frac_ = knob_frac(i);
                edit_.begin(kKnobs[static_cast<std::size_t>(i)].id);
                return;
            }
        }
    }

    bool hit_toggle(vw::Point p, int under_knob, pulp::state::ParamID id) {
        const float cx = nam_geom::knob_cx(under_knob);
        const vw::Rect r{sx(cx - nam_geom::kToggleW * 0.5f), sy(nam_geom::kToggleY),
                         ss(nam_geom::kToggleW), ss(nam_geom::kToggleH)};
        if (!in_rect(p, r)) return false;
        toggle_param(id);
        return true;
    }

    void pointer_move(vw::Point p) {
        if (active_knob_ < 0) return;
        // 200 px of vertical drag spans the full range (up = increase).
        const float dfrac = (drag_start_y_ - p.y) / (200.0f * scale_);
        const float frac = std::clamp(drag_start_frac_ + dfrac, 0.0f, 1.0f);
        const KnobSpec& k = kKnobs[static_cast<std::size_t>(active_knob_)];
        edit_.set(k.id, k.lo + frac * (k.hi - k.lo));
    }

    void pointer_release() {
        if (!pointer_down_) return;
        pointer_down_ = false;
        if (active_knob_ >= 0) { edit_.finish(); active_knob_ = -1; }
    }

    void set_param(pulp::state::ParamID id, float v) {
        pulp::state::ParameterEdit t(store_);
        t.begin(id); t.set(id, v); t.finish();
    }
    void toggle_param(pulp::state::ParamID id) {
        set_param(id, store_.get_value(id) >= 0.5f ? 0.0f : 1.0f);
    }

    float knob_value(int i) const {
        const KnobSpec& k = kKnobs[static_cast<std::size_t>(i)];
        if (i == active_knob_) return edit_.display_value(k.id, store_.get_value(k.id));
        return store_.get_value(k.id);
    }
    float knob_frac(int i) const {
        const KnobSpec& k = kKnobs[static_cast<std::size_t>(i)];
        return std::clamp((knob_value(i) - k.lo) / (k.hi - k.lo), 0.0f, 1.0f);
    }

    pulp::state::StateStore& store_;
    pulp::examples::GpuNamProcessor& proc_;
    pulp::state::ParameterEdit edit_;
    GnColors colors_;

    std::string asset_dir_;
    bool fonts_done_ = false;
    std::unordered_map<std::string, std::string> svg_cache_;

    float scale_ = 1.0f, ox_ = 0.0f, oy_ = 0.0f;
    int active_knob_ = -1;
    float drag_start_y_ = 0.0f, drag_start_frac_ = 0.0f;
    bool pointer_down_ = false;
    bool show_settings_ = false;
    vw::Rect settings_engine_cpu_{}, settings_engine_gpu_{}, settings_bypass_{}, settings_close_{};
    vw::Rect model_slot_{}, ir_slot_{};
};

} // namespace pulp::examples
