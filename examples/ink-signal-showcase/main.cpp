// Ink & Signal Showcase — a live, GPU-backed, scrollable window of (nearly)
// every design-system primitive, organized like the Figma Overview, so the
// widgets can be seen and *felt*: drag knobs/faders/sliders/pan/XY, click
// toggles/checkboxes/buttons, step the stepper, switch tabs, play the keyboard.
//
//   pulp-ink-signal-showcase                       # live GPU window (dark)
//   pulp-ink-signal-showcase --theme light
//   pulp-ink-signal-showcase --screenshot out.png  # headless GPU/Skia render
//
// GPU requires a Skia-enabled build (configure with -DSKIA_DIR=<skia-build>);
// without it the window host falls back to the CPU raster path. The content
// scrolls (trackpad / wheel) and the window has a minimum size so content is
// never cropped.

#include <pulp/view/breadcrumb.hpp>
#include <pulp/view/buttons.hpp>
#include <pulp/view/caret.hpp>
#include <pulp/view/context_menu.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/gap_widgets.hpp>
#include <pulp/view/midi_keyboard.hpp>
#include <pulp/view/design_frame_view.hpp>  // FaithfulOverview (--overview reference)
#include <pulp/view/musical_typing_keyboard.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/scroll_bar.hpp>
#include <pulp/view/side_panel.hpp>
#include <pulp/view/table.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/theme_presets.hpp>
#include <pulp/view/toolbar.hpp>
#include <pulp/view/tree_view.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/window_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace pulp::view;
using pulp::canvas::Color;
namespace canvas = pulp::canvas;

namespace {

constexpr float kContentW = 940.0f;
constexpr float kMargin = 32.0f;

// Content board: paints the app background; children carry explicit bounds
// (suppress the flex pass). Rendered directly for the headless screenshot and
// wrapped in a ScrollView for the live window (so it scrolls).
class Board : public View {
public:
    void paint(canvas::Canvas& c) override {
        c.set_fill_color(resolve_color("bg.primary", Color::rgba8(22, 26, 33)));
        c.fill_rect(0, 0, bounds().width, bounds().height);
    }
    // No layout_children override: children are position:absolute, so the Yoga
    // pass (used by both render_to_png and the live window's ScrollView) places
    // each at its left()/top() with preferred size — same result on both paths.
};

std::vector<float> demo_wave(size_t n = 1024) {
    std::vector<float> s(n);
    for (size_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n);
        s[i] = std::sin(t * 48.0f) * std::exp(-3.0f * t);
    }
    return s;
}

std::vector<float> demo_spectrum(size_t n = 48) {
    std::vector<float> m(n);
    for (size_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n);
        m[i] = -6.0f - 36.0f * t + 8.0f * std::sin(t * 18.0f);
    }
    return m;
}

// Global modulation LFO phase in [-1,1], advanced once per frame in main()'s
// idle callback and read here to drive every modulation knob's live indicator.
float g_mod_lfo = 0.0f;

// Advance animated widgets each frame (hover glow, toggle slide, scroll easing).
void advance_anims(View* v, float dt) {
    if (!v) return;
    if (auto* k = dynamic_cast<Knob*>(v)) {
        k->advance_animations(dt);
        // Modulation knobs: ride the shared LFO so the live indicator moves
        // continuously, showing the real-time modulated value on the arc.
        if (!k->modulation_rings().empty()) k->set_modulation_phase(g_mod_lfo);
    }
    else if (auto* t = dynamic_cast<Toggle*>(v)) t->advance_animations(dt);
    else if (auto* f = dynamic_cast<Fader*>(v)) f->advance_animations(dt);
    else if (auto* r = dynamic_cast<RangeSlider*>(v)) r->advance_animations(dt);
    else if (auto* p = dynamic_cast<PanControl*>(v)) p->advance_animations(dt);
    else if (auto* sp = dynamic_cast<Spinner*>(v)) sp->advance_animations(dt);
    else if (auto* tip = dynamic_cast<Tooltip*>(v)) tip->advance_animations(dt);
    else if (auto* s = dynamic_cast<ScrollView*>(v)) s->advance_animations(dt);
    for (std::size_t i = 0; i < v->child_count(); ++i) advance_anims(v->child_at(i), dt);
}

// Legacy hand-built native-widget gallery — kept for the future "interactive"
// hybrid view, but no longer the default (the showcase now mirrors the Figma
// Overview 1:1; see build_overview_board).
[[maybe_unused]] std::unique_ptr<View> build_board(float& out_height, ThemeModeControl*& out_theme_ctl) {
    auto board = std::make_unique<Board>();
    Board* b = board.get();
    float y = 24.0f;

    auto add = [&](std::unique_ptr<View> v, float x, float yy, float w, float h) -> View* {
        // Position absolutely so the layout survives the window's full-subtree
        // Yoga pass (a ScrollView wraps the board in the live window, and Yoga
        // would otherwise stretch flex children to full width). Yoga reads
        // left()/top() + preferred_width/height for absolute nodes.
        v->set_bounds({x, yy, w, h});
        v->set_position(View::Position::absolute);
        v->set_left(x);
        v->set_top(yy);
        v->flex().preferred_width = w;
        v->flex().preferred_height = h;
        View* p = v.get();
        b->add_child(std::move(v));
        return p;
    };
    auto label = [&](const std::string& t, float x, float yy, float w, float fs) {
        auto l = std::make_unique<Label>(t);
        l->set_font_size(fs);
        add(std::move(l), x, yy, w, fs + 6.0f);
    };
    auto title = [&](const std::string& t) { label(t, kMargin, y, kContentW, 24.0f); y += 42.0f; };
    auto section = [&](const std::string& t) { y += 6.0f; label(t, kMargin, y, kContentW, 13.0f); y += 26.0f; };

    title("Ink & Signal — Widget Gallery");

    // Theme-mode control, top-right: system / light / dark. Wired to a
    // ThemeManager in main() so it follows the OS live (system) or pins the
    // theme (light/dark). A developer who wants only one mode just omits it.
    {
        auto ctl = std::make_unique<ThemeModeControl>();
        out_theme_ctl = static_cast<ThemeModeControl*>(
            add(std::move(ctl), kMargin + kContentW - 104.0f, 24.0f, 104.0f, 28.0f));
    }

    // ── Buttons ────────────────────────────────────────────────────────
    section("Buttons");
    {
        float x = kMargin;
        auto mk = [&](const char* t, TextButton::Style s) {
            auto bt = std::make_unique<TextButton>(t); bt->set_style(s);
            add(std::move(bt), x, y, 110.0f, 36.0f); x += 122.0f;
        };
        mk("Primary", TextButton::Style::primary);
        mk("Secondary", TextButton::Style::secondary);
        mk("Ghost", TextButton::Style::ghost);
        add(std::make_unique<ArrowButton>(ArrowDirection::right), x, y + 6.0f, 24.0f, 24.0f); x += 36.0f;
        auto tb = std::make_unique<ToggleButton>(); tb->set_on(true); tb->set_label("Loop");
        add(std::move(tb), x, y, 90.0f, 36.0f);
        y += 56.0f;
    }

    // ── Knobs ──────────────────────────────────────────────────────────
    section("Knobs");
    {
        float x = kMargin;
        const char* names[] = {"Cutoff", "Reso", "Drive", "Mix", "Tone"};
        const float vals[] = {0.2f, 0.5f, 0.8f, 0.35f, 0.65f};
        for (int i = 0; i < 5; ++i) {
            auto k = std::make_unique<Knob>(); k->set_value(vals[i]); k->set_label(names[i]);
            // Cutoff is a frequency control → logarithmic law (skew), so the
            // dial spends most of its travel in the musically-useful low range.
            if (i == 0) {
                k->set_skew(0.35f);
                k->set_format([](float v) {
                    float hz = 20.0f * std::pow(1000.0f, v);  // 20 Hz … 20 kHz
                    char b[16];
                    if (hz >= 1000.0f) std::snprintf(b, sizeof b, "%.1fk", hz / 1000.0f);
                    else               std::snprintf(b, sizeof b, "%.0f", hz);
                    return std::string(b);
                });
            }
            add(std::move(k), x, y, 84.0f, 84.0f); x += 96.0f;
        }
        y += 104.0f;
    }

    // ── Knob modulation (Saturn rings) ─────────────────────────────────
    section("Knob modulation");
    {
        // Brand modulation-source colors (LFO blue, ENV amber, VEL pink,
        // MACRO violet) — match the Figma "Knob Modulation" set.
        const Color LFO = Color::hex(0x5E78FF), ENV = Color::hex(0xF6B847),
                    VEL = Color::hex(0xFF7AA8), MAC = Color::hex(0x8B6CF5);
        // Each ring is {lo, hi, color} — independent signed offsets from base,
        // so the labels are now literal: Positive extends up only (lo=0), Negative
        // down only (hi=0), Bipolar both ways. Each handle drags on its own.
        struct Spec { const char* name; float val; std::vector<Knob::ModulationRing> rings; };
        std::vector<Spec> specs = {
            {"Positive", 0.5f, {{0.0f, 0.5f, ENV}}},
            {"Negative", 0.6f, {{-0.4f, 0.0f, LFO}}},
            {"Bipolar", 0.5f, {{-0.55f, 0.55f, MAC}}},
            {"2 sources", 0.45f, {{0.0f, 0.5f, LFO}, {-0.3f, 0.0f, ENV}}},
            {"3 sources", 0.5f, {{0.0f, 0.4f, LFO}, {0.0f, 0.6f, ENV}, {-0.5f, 0.0f, VEL}}},
        };
        float x = kMargin;
        std::vector<Knob*> mod_targets;
        for (auto& s : specs) {
            auto k = std::make_unique<Knob>(); k->set_value(s.val); k->set_label(s.name);
            k->set_modulation_rings(s.rings);
            mod_targets.push_back(static_cast<Knob*>(add(std::move(k), x, y, 92.0f, 92.0f)));
            x += 104.0f;
        }
        // Master / macro control: dragging it drives the modulation DEPTH (the
        // range width) of the other knobs — the macro-routing interaction. The
        // live indicator dots ride the arcs continuously (LFO-driven in
        // advance_anims), so widening the range here visibly enlarges where each
        // indicator can travel.
        auto macro = std::make_unique<Knob>();
        macro->set_value(0.5f); macro->set_label("Macro");
        macro->set_format([](float v) { char b[8]; std::snprintf(b, sizeof b, "%.0f%%", v * 100); return std::string(b); });
        Knob* m0 = mod_targets[0];
        Knob* m2 = mod_targets[2];
        macro->on_change = [m0, m2](float v) {
            // Map 0..1 → bipolar half-width 0..0.6, applied to two target knobs
            // (widens both ends symmetrically — the macro-routing demo).
            m0->set_modulation_rings({{-v * 0.6f, v * 0.6f, Color::hex(0xF6B847)}});
            m2->set_modulation_rings({{-v * 0.6f, v * 0.6f, Color::hex(0x8B6CF5)}});
        };
        add(std::move(macro), x + 12.0f, y, 92.0f, 92.0f);
        y += 112.0f;
    }

    // ── Sliders, faders, pan, stepper ──────────────────────────────────
    section("Sliders · Faders · Pan · Stepper");
    {
        auto sl = std::make_unique<RangeSlider>(); sl->set_min(0); sl->set_max(1); sl->set_value(0.4f);
        add(std::move(sl), kMargin, y + 8.0f, 240.0f, 18.0f);
        auto f1 = std::make_unique<Fader>(); f1->set_value(0.62f);
        add(std::move(f1), kMargin + 280.0f, y, 26.0f, 96.0f);
        auto f2 = std::make_unique<Fader>(); f2->set_value(0.4f);
        add(std::move(f2), kMargin + 320.0f, y, 26.0f, 96.0f);
        auto pan = std::make_unique<PanControl>(); pan->set_value(-0.3f);
        add(std::move(pan), kMargin + 380.0f, y + 8.0f, 220.0f, 18.0f);
        auto st = std::make_unique<Stepper>(); st->set_range(-24, 24); st->set_value(7); st->set_suffix("st");
        add(std::move(st), kMargin + 380.0f, y + 48.0f, 150.0f, 36.0f);
        y += 116.0f;
    }

    // ── Toggles, checkboxes, inputs ────────────────────────────────────
    section("Toggles · Checkboxes · Inputs");
    {
        auto tg = std::make_unique<Toggle>(); tg->set_on(true);
        add(std::move(tg), kMargin, y + 4.0f, 52.0f, 30.0f);
        auto cb1 = std::make_unique<Checkbox>(); cb1->set_checked(true);
        add(std::move(cb1), kMargin + 80.0f, y + 6.0f, 22.0f, 22.0f);
        auto cb2 = std::make_unique<Checkbox>(); cb2->set_checked(false);
        add(std::move(cb2), kMargin + 112.0f, y + 6.0f, 22.0f, 22.0f);
        auto in = std::make_unique<TextEditor>(); in->set_text("Velvet Plate");
        add(std::move(in), kMargin + 170.0f, y, 200.0f, 32.0f);
        auto combo = std::make_unique<ComboBox>(); combo->set_items({"Sine", "Saw", "Square"}); combo->set_selected(1);
        add(std::move(combo), kMargin + 390.0f, y, 180.0f, 32.0f);
        y += 52.0f;
    }

    // ── Caret — shape picker + blink rate ──────────────────────────────
    section("Caret");
    {
        // A caret paints only while its widget is focused, so one field plus an
        // exclusive picker A/Bs the shapes without the field ever losing focus.
        auto ted = std::make_unique<TextEditor>(); ted->set_text("Velvet Plate");
        auto* ed = static_cast<TextEditor*>(add(std::move(ted), kMargin, y, 200.0f, 32.0f));

        // 3-way exclusive shape picker driving the single editor above.
        const CaretStyle styles[] = {CaretStyle::ibeam, CaretStyle::underline, CaretStyle::block};
        auto seg = std::make_unique<SegmentedControl>();
        seg->set_segments({"ibeam", "underline", "block"});
        seg->set_selected(0);  // ibeam is the default
        seg->on_change = [ed, styles](int i) { ed->set_caret_style(styles[i]); };
        add(std::move(seg), kMargin + 220.0f, y + 1.0f, 240.0f, 30.0f);

        // Blink-period readout, updated live by the slider below.
        auto rate = std::make_unique<Label>("1.06 s"); rate->set_font_size(12.0f);
        auto* rl = static_cast<Label*>(add(std::move(rate), kMargin + 252.0f, y + 44.0f, 60.0f, 16.0f));

        // Maps to CaretBlinkConfig::period_seconds; the other blink fields carry over.
        auto sl = std::make_unique<RangeSlider>();
        sl->set_min(0.4f); sl->set_max(2.0f); sl->set_value(1.06f);
        sl->on_change = [ed, rl](float v) {
            CaretBlinkConfig cfg = ed->caret_blink();
            cfg.period_seconds = v;
            ed->set_caret_blink(cfg);
            char b[16]; std::snprintf(b, sizeof b, "%.2f s", v);
            rl->set_text(b);
        };
        add(std::move(sl), kMargin, y + 46.0f, 240.0f, 18.0f);

        label("Click the field, pick a shape, drag to set blink rate", kMargin, y + 70.0f, 420.0f, 11.0f);
        y += 90.0f;
    }

    // ── Meters & progress ──────────────────────────────────────────────
    section("Meters · Progress");
    {
        const float lvls[] = {0.45f, 0.8f, 0.97f};
        float my = y;
        for (int i = 0; i < 3; ++i) {
            auto m = std::make_unique<Meter>(); m->set_orientation(Meter::Orientation::horizontal);
            m->set_level(lvls[i], lvls[i]);
            add(std::move(m), kMargin, my, 240.0f, 14.0f); my += 22.0f;
        }
        auto mv = std::make_unique<Meter>(); mv->set_level(0.7f, 0.9f);
        add(std::move(mv), kMargin + 280.0f, y, 16.0f, 70.0f);
        auto pb = std::make_unique<ProgressBar>(); pb->set_progress(0.6f);
        add(std::move(pb), kMargin + 330.0f, y + 28.0f, 260.0f, 10.0f);
        y += 84.0f;
    }

    // ── Status badges ──────────────────────────────────────────────────
    section("Status badges");
    {
        float x = kMargin;
        const char* labels[] = {"VST3", "Info", "Active", "48 kHz", "Peak"};
        const Tone tones[] = {Tone::neutral, Tone::info, Tone::success, Tone::warning, Tone::danger};
        for (int i = 0; i < 5; ++i) { add(std::make_unique<Badge>(labels[i], tones[i]), x, y, 70.0f, 24.0f); x += 82.0f; }
        y += 40.0f;
    }

    // ── Banners, toast, empty state, callout ───────────────────────────
    section("Banners · Toast · Empty state · Callout");
    {
        auto b1 = std::make_unique<InlineBanner>(); b1->set_tone(Tone::success);
        b1->set_label("Build succeeded."); b1->set_message("VST3 · AU · CLAP signed in 4.8s.");
        add(std::move(b1), kMargin, y, 440.0f, 46.0f);
        auto b2 = std::make_unique<InlineBanner>(); b2->set_tone(Tone::danger);
        b2->set_label("Render failed."); b2->set_message("Output device unavailable.");
        add(std::move(b2), kMargin + 460.0f, y, 440.0f, 46.0f);
        y += 58.0f;
        auto toast = std::make_unique<Toast>(); toast->set_title("Preset saved");
        toast->set_subtitle("Velvet Plate · User library"); toast->set_action("Undo");
        add(std::move(toast), kMargin, y, 440.0f, 64.0f);
        auto empty = std::make_unique<EmptyState>(); empty->set_message("No presets yet"); empty->set_action("create one");
        add(std::move(empty), kMargin + 460.0f, y, 440.0f, 64.0f);
        y += 76.0f;
        auto callout = std::make_unique<CallOutBox>(); callout->set_message("Quantize snaps notes to the grid.");
        add(std::move(callout), kMargin, y, 440.0f, 48.0f);
        y += 64.0f;
    }

    // ── Tabs · Toolbar · Breadcrumb · Tree · Scrollbar ─────────────────
    section("Navigation");
    {
        const float y0 = y;
        // Left column: underline tabs (Figma 227:1763) + segmented control.
        auto tabs = std::make_unique<TabPanel>();
        tabs->set_tab_bar_style(TabPanel::TabBarStyle::underline);
        tabs->add_tab("Amp", std::make_unique<View>());
        tabs->add_tab("Filter", std::make_unique<View>());
        tabs->add_tab("FX", std::make_unique<View>());
        tabs->set_active_tab(0);
        add(std::move(tabs), kMargin, y0, 280.0f, 44.0f);

        auto seg = std::make_unique<SegmentedControl>();
        seg->set_segments({"Amp", "EQ", "Comp", "Reverb"});
        seg->set_selected(0);
        add(std::move(seg), kMargin, y0 + 52.0f, 280.0f, 30.0f);

        // Right column: transport toolbar (loop active = teal) + flat breadcrumb.
        auto tb = std::make_unique<Toolbar>();
        tb->add_button("play", "Play", [] {});
        tb->add_toggle("loop", "Loop", [](bool) {});
        tb->set_toggled("loop", true);
        tb->add_separator();
        tb->add_button("rec", "Rec", [] {});
        add(std::move(tb), kMargin + 320.0f, y0, 280.0f, 40.0f);

        auto bc = std::make_unique<Breadcrumb>();
        bc->set_show_background(false);   // flat — Figma 227:1830
        bc->set_items({{"Library", {}}, {"Reverb", {}}, {"Halls", {}}});
        add(std::move(bc), kMargin + 320.0f, y0 + 52.0f, 280.0f, 24.0f);
        y = y0 + 92.0f;

        // Tree with accent selection + leaf icons (Figma 227:4196). Folders use
        // the disclosure chevron; presets carry a ♪.
        auto tree = std::make_unique<TreeView>();
        tree->set_selection_style(TreeView::SelectionStyle::accent);
        auto& root = tree->root();
        auto& synths = root.add_child("Synths"); synths.expanded = true;
        synths.add_child("Bass").icon = "\xe2\x99\xaa";
        synths.add_child("Lead").icon = "\xe2\x99\xaa";
        auto& fx = root.add_child("Effects"); fx.expanded = true;
        auto& rev = fx.add_child("Reverb"); rev.icon = "\xe2\x99\xaa";
        fx.add_child("Delay").icon = "\xe2\x99\xaa";
        tree->set_selected_node(&rev);
        add(std::move(tree), kMargin, y, 280.0f, 130.0f);

        auto sb = std::make_unique<ScrollBar>();
        sb->set_orientation(ScrollBar::Orientation::vertical);
        sb->set_range(0, 100); sb->set_page_size(30);
        add(std::move(sb), kMargin + 300.0f, y, 12.0f, 130.0f);
        y += 150.0f;
    }

    // ── Audio ──────────────────────────────────────────────────────────
    section("Audio");
    {
        auto wave = std::make_unique<WaveformView>(); wave->set_data(demo_wave());
        add(std::move(wave), kMargin, y, 440.0f, 110.0f);
        auto spec = std::make_unique<SpectrumView>(); spec->set_spectrum(demo_spectrum());
        add(std::move(spec), kMargin + 460.0f, y, 440.0f, 110.0f);
        y += 124.0f;
        auto xy = std::make_unique<XYPad>();
        add(std::move(xy), kMargin, y, 130.0f, 130.0f);
        auto kbd = std::make_unique<MidiKeyboard>(); kbd->set_range(48, 72);
        add(std::move(kbd), kMargin + 160.0f, y + 40.0f, 740.0f, 90.0f);
        y += 150.0f;
    }

    // ── Musical typing keyboard — playable faithful-import primitive ──────
    // The faithful Figma frame rendered 1:1 (DesignFrameView), with each
    // typing key a `momentary` element: click/drag plays it, lighting the key
    // via a native overlay (the SVG is never recolored). Wired here to a live
    // "last note" readout to prove the gesture callbacks reach a consumer —
    // the same hooks a sampler binds to MusicalTypingController. The 🎹/⌨
    // toggle (top-left) swaps to the piano mode frame (and resizes the panel).
    section("Musical typing keyboard — playable, toggleable (faithful import)");
    {
        const float kw = kContentW;
        const float kh = kw * 266.0f / 732.0f;  // typing-frame aspect (no letterbox)
        auto* readout = static_cast<Label*>(nullptr);
        auto note_label = std::make_unique<Label>("Play a key \xe2\x80\x94 last note: \xe2\x80\x94");
        note_label->set_font_size(13.0f);
        readout = static_cast<Label*>(
            add(std::move(note_label), kMargin, y, kContentW, 18.0f));
        y += 24.0f;

        auto kb = std::make_unique<MusicalTypingKeyboard>();
        MusicalTypingKeyboard* kbp = kb.get();
        // The keyboard emits absolute MIDI notes (typing keys via base C2 + the
        // z/x octave; piano keys absolute). Name them off one MIDI map (C2 = 48,
        // so octave = midi/12 - 2) for the live "last note" readout.
        auto midi_name = [](int midi) {
            static const char* n[] = {"C", "C#", "D", "D#", "E", "F",
                                      "F#", "G", "G#", "A", "A#", "B"};
            return std::string(n[midi % 12]) + std::to_string(midi / 12 - 2);
        };
        kbp->on_note_on = [readout, midi_name](int note, float) {
            if (readout) readout->set_text("Play a key \xe2\x80\x94 last note: " + midi_name(note));
        };
        // Default display mirrors the design's "selected keys shown" illustration
        // for the typing mode — the held chord A·W·G·U (relative semitones
        // 0·1·7·10 → MIDI 48·49·55·58 at base C2). The baked chord is neutralized
        // in the embed, so the LIVE overlay owns this display; the keyboard stays
        // interactive (clicking plays/lights others) and the toggle reveals piano.
        const int kTypingHeld[] = {48, 49, 55, 58};   // A W G U at base C2
        kbp->set_active_notes(kTypingHeld);
        add(std::move(kb), kMargin, y, kw, kh);
        y += kh + 20.0f;
    }

    // ── Containers — Property panel · Channel strip (native, interactive) ──
    section("Containers — Property panel · Channel strip");
    {
        // Property panel composed from native primitives — labeled rows, each a
        // LIVE control (slider / choice / toggle / text / button), like a real
        // plugin builds it. (pulp::view::PropertyPanel is a backing data model,
        // not a rendered widget, so the visual panel is composed here.)
        const float pw = 460.0f, rowH = 44.0f, padX = 16.0f;
        const float ctlX = kMargin + pw - 196.0f, ctlW = 180.0f;
        const float panelTop = y;
        const float panelH = 5 * rowH + 16.0f;
        add(std::make_unique<Panel>(), kMargin, panelTop, pw, panelH);
        auto row_label = [&](const char* t, float ry) {
            auto l = std::make_unique<Label>(t); l->set_font_size(13.0f);
            add(std::move(l), kMargin + padX, ry + 14.0f, 120.0f, 18.0f);
        };
        float ry = panelTop + 8.0f;
        row_label("Cutoff", ry);
        { auto s = std::make_unique<RangeSlider>(); s->set_min(20); s->set_max(20000); s->set_value(1200);
          add(std::move(s), ctlX, ry + 18.0f, ctlW - 44.0f, 16.0f);
          auto v = std::make_unique<Label>("1.2k"); v->set_font_size(12.0f);
          add(std::move(v), ctlX + ctlW - 38.0f, ry + 14.0f, 40.0f, 16.0f); }
        ry += rowH;
        row_label("Shape", ry);
        { auto c = std::make_unique<ComboBox>(); c->set_items({"Low-pass", "Band-pass", "High-pass"}); c->set_selected(0);
          add(std::move(c), ctlX, ry + 6.0f, ctlW, 32.0f); }
        ry += rowH;
        row_label("Key sync", ry);
        { auto t = std::make_unique<Toggle>(); t->set_on(true);
          add(std::move(t), ctlX + ctlW - 52.0f, ry + 8.0f, 52.0f, 30.0f); }
        ry += rowH;
        row_label("Name", ry);
        { auto e = std::make_unique<TextEditor>(); e->set_text("Pad 02");
          add(std::move(e), ctlX, ry + 6.0f, ctlW, 32.0f); }
        ry += rowH;
        row_label("Preset", ry);
        { auto b = std::make_unique<TextButton>("Load\xe2\x80\xa6"); b->set_style(TextButton::Style::secondary);
          add(std::move(b), ctlX + ctlW - 90.0f, ry + 6.0f, 90.0f, 32.0f); }

        // Native ChannelStrip: draggable fader + pan, live meter.
        auto cs = std::make_unique<ChannelStrip>();
        cs->set_label("Master"); cs->set_level(0.75f); cs->set_pan(0.0f);
        add(std::move(cs), kMargin + pw + 40.0f, panelTop, 96.0f, panelH);
        y = panelTop + panelH + 20.0f;
    }

    // ── Group box (titled container) — default / collapsible / empty ──────
    section("Group box — titled container");
    {
        // Add a child control into a GroupBox at the group's local coords, so
        // collapsing the group hides it (GroupBox toggles its children).
        auto add_to = [](GroupBox* g, std::unique_ptr<View> v, float x, float yy, float w, float h) {
            v->set_bounds({x, yy, w, h});
            v->set_position(View::Position::absolute);
            v->set_left(x); v->set_top(yy);
            v->flex().preferred_width = w; v->flex().preferred_height = h;
            g->add_child(std::move(v));
        };
        auto filter_rows = [&](GroupBox* g) {
            const float ct = g->content_top();
            auto l1 = std::make_unique<Label>("CUTOFF"); l1->set_font_size(11.0f);
            add_to(g, std::move(l1), 16.0f, ct + 4.0f, 70.0f, 16.0f);
            auto s1 = std::make_unique<RangeSlider>(); s1->set_min(0); s1->set_max(1); s1->set_value(0.62f);
            add_to(g, std::move(s1), 96.0f, ct + 6.0f, 156.0f, 14.0f);
            auto l2 = std::make_unique<Label>("RESO"); l2->set_font_size(11.0f);
            add_to(g, std::move(l2), 16.0f, ct + 34.0f, 70.0f, 16.0f);
            auto s2 = std::make_unique<RangeSlider>(); s2->set_min(0); s2->set_max(1); s2->set_value(0.3f);
            add_to(g, std::move(s2), 96.0f, ct + 36.0f, 156.0f, 14.0f);
        };
        const float gw = 280.0f, gh = 120.0f, gx2 = kMargin + 320.0f;

        auto* gb1 = static_cast<GroupBox*>(add(std::make_unique<GroupBox>(), kMargin, y, gw, gh));
        gb1->set_title("Filter"); filter_rows(gb1);
        label("DEFAULT", kMargin, y + gh + 4.0f, 120.0f, 11.0f);

        auto* gb2 = static_cast<GroupBox*>(add(std::make_unique<GroupBox>(), gx2, y, gw, gh));
        gb2->set_title("Filter"); gb2->set_collapsible(true); filter_rows(gb2);
        label("COLLAPSIBLE · EXPANDED", gx2, y + gh + 4.0f, 200.0f, 11.0f);
        y += gh + 28.0f;

        // Collapsible, starts collapsed. Full bounds (gh) are reserved so that
        // clicking to expand fills its own slot instead of overlapping the rows
        // below (the absolute showcase layout has no reflow).
        auto* gb3 = static_cast<GroupBox*>(add(std::make_unique<GroupBox>(), kMargin, y, gw, gh));
        gb3->set_title("Filter"); gb3->set_collapsible(true); filter_rows(gb3); gb3->set_collapsed(true);
        label("COLLAPSIBLE · COLLAPSED (click to expand)", kMargin, y + gh + 4.0f, 320.0f, 11.0f);

        auto* gb4 = static_cast<GroupBox*>(add(std::make_unique<GroupBox>(), gx2, y, gw, gh));
        gb4->set_title("Envelope");
        { auto e = std::make_unique<Label>("NO CONTROLS YET"); e->set_font_size(11.0f);
          add_to(gb4, std::move(e), 16.0f, gb4->content_top() + 6.0f, 200.0f, 16.0f); }
        label("EMPTY · TITLE ONLY", gx2, y + gh + 4.0f, 200.0f, 11.0f);
        y += gh + 28.0f;
    }

    // ── Range slider — dual-handle (min–max) ──────────────────────────────
    section("Range slider — dual-handle (min–max)");
    {
        { auto d = std::make_unique<DualRangeSlider>(); d->set_low(0.25f); d->set_high(0.70f);
          add(std::move(d), kMargin, y + 8.0f, 360.0f, 18.0f); }
        label("DEFAULT · 25–70%", kMargin, y + 30.0f, 200.0f, 11.0f);
        { auto d = std::make_unique<DualRangeSlider>(); d->set_low(0.0f); d->set_high(1.0f);
          add(std::move(d), kMargin + 400.0f, y + 8.0f, 360.0f, 18.0f); }
        label("FULL RANGE · 0–100%", kMargin + 400.0f, y + 30.0f, 200.0f, 11.0f);
        y += 56.0f;
        { auto d = std::make_unique<DualRangeSlider>(); d->set_low(0.3f); d->set_high(0.8f); d->set_enabled(false);
          add(std::move(d), kMargin, y + 8.0f, 360.0f, 18.0f); }
        label("DISABLED", kMargin, y + 30.0f, 120.0f, 11.0f);
        { auto v = std::make_unique<DualRangeSlider>();
          v->set_orientation(DualRangeSlider::Orientation::vertical);
          v->set_low(0.2f); v->set_high(0.75f);
          add(std::move(v), kMargin + 440.0f, y - 6.0f, 18.0f, 100.0f); }
        label("VERTICAL", kMargin + 410.0f, y + 100.0f, 120.0f, 11.0f);
        { auto cl = std::make_unique<DualRangeSlider>();
          cl->set_low(0.35f); cl->set_high(0.65f); cl->set_no_cross(true);
          add(std::move(cl), kMargin, y + 44.0f, 360.0f, 18.0f); }
        label("CLAMPED \xc2\xb7 thumbs can\xe2\x80\x99t cross", kMargin, y + 66.0f, 260.0f, 11.0f);
        y += 150.0f;
    }

    // ── Inline value editor — click a readout to type ─────────────────────
    section("Inline value editor — click a readout to type");
    {
        // Knob + editable readout, wired both ways: drag the knob and the
        // readout updates; click the readout, type, Enter → the knob moves.
        auto* kp = static_cast<Knob*>(add(std::make_unique<Knob>(), kMargin, y, 84.0f, 84.0f));
        kp->set_value(0.62f); kp->set_label("Level");
        auto ied = std::make_unique<InlineValueEditor>();
        ied->set_range(0, 100); ied->set_decimals(0); ied->set_suffix("%"); ied->set_value(62);
        auto* ep = static_cast<InlineValueEditor*>(add(std::move(ied), kMargin, y + 90.0f, 84.0f, 26.0f));
        kp->on_change = [ep](float v) { ep->set_value(v * 100.0); };
        ep->on_change = [kp](double v) { kp->set_value(static_cast<float>(v / 100.0)); };
        label("UNDER KNOB", kMargin, y + 120.0f, 120.0f, 11.0f);

        // Fader + editable dB readout beside it.
        auto* fp = static_cast<Fader*>(add(std::make_unique<Fader>(), kMargin + 200.0f, y, 26.0f, 100.0f));
        fp->set_value(0.7f);
        auto fied = std::make_unique<InlineValueEditor>();
        fied->set_range(-60, 0); fied->set_decimals(1); fied->set_suffix(" dB");
        fied->set_value(-60.0 + 60.0 * 0.7);
        auto* fep = static_cast<InlineValueEditor*>(add(std::move(fied), kMargin + 240.0f, y + 36.0f, 96.0f, 26.0f));
        fp->on_change = [fep](float v) { fep->set_value(-60.0 + 60.0 * v); };
        fep->on_change = [fp](double v) { fp->set_value(static_cast<float>((v + 60.0) / 60.0)); };
        label("BESIDE FADER", kMargin + 200.0f, y + 120.0f, 140.0f, 11.0f);

        // Disabled readout.
        { auto de = std::make_unique<InlineValueEditor>();
          de->set_value(-3.5); de->set_decimals(1); de->set_suffix(" dB"); de->set_enabled(false);
          add(std::move(de), kMargin + 420.0f, y + 36.0f, 96.0f, 26.0f); }
        label("DISABLED", kMargin + 420.0f, y + 120.0f, 120.0f, 11.0f);

        label("Click a readout, type, Enter commits \xc2\xb7 Esc cancels \xc2\xb7 out-of-range shows a danger ring",
              kMargin, y + 140.0f, 760.0f, 11.0f);
        y += 162.0f;
    }

    // ── Waveform recorder — armed · recording · captured ──────────────────
    section("Waveform recorder — armed \xc2\xb7 recording \xc2\xb7 captured");
    {
        const char* labels[] = {"ARMED \xc2\xb7 threshold-armed", "RECORDING \xc2\xb7 press to stop",
                                "CAPTURED \xc2\xb7 ready to play"};
        WaveformRecorder::State states[] = {WaveformRecorder::State::armed,
                                            WaveformRecorder::State::recording,
                                            WaveformRecorder::State::captured};
        for (int i = 0; i < 3; ++i) {
            auto wr = std::make_unique<WaveformRecorder>();
            wr->set_waveform(demo_wave());
            wr->set_state(states[i]);
            wr->set_level(i == 1 ? 0.82f : 0.4f);
            wr->set_threshold(0.3f);
            add(std::move(wr), kMargin, y, kContentW, 130.0f);
            label(labels[i], kMargin, y + 134.0f, 280.0f, 11.0f);
            y += 158.0f;
        }
    }

    // ── Buttons & inputs — Search · TextArea · NumberBox ───────────────
    section("Buttons & inputs — Search · Number · TextArea");
    {
        auto search = std::make_unique<TextEditor>();
        search->placeholder = "Filter devices…";          // Search = placeholder input
        add(std::move(search), kMargin, y, 240.0f, 32.0f);

        auto num = std::make_unique<NumberBox>();
        num->set_range(-24, 24); num->set_step(0.5); num->set_value(-3.5); num->set_suffix("st");
        add(std::move(num), kMargin + 260.0f, y, 150.0f, 32.0f);

        auto ta = std::make_unique<TextEditor>();
        ta->multi_line = true;                              // TextArea = multi-line input
        ta->set_text("Warm, glassy tail.\nLong reverb decay.");
        add(std::move(ta), kMargin + 430.0f, y, 260.0f, 64.0f);
        y += 84.0f;
    }

    // ── Status — Spinner · Tooltip ─────────────────────────────────────
    section("Status — Spinner · Tooltip");
    {
        auto sp1 = std::make_unique<Spinner>();                       // indeterminate
        add(std::move(sp1), kMargin, y, 28.0f, 28.0f);
        auto sp2 = std::make_unique<Spinner>(); sp2->set_progress(0.65f);  // determinate
        add(std::move(sp2), kMargin + 44.0f, y, 28.0f, 28.0f);

        auto tip = std::make_unique<Tooltip>("Bypass (\xe2\x8c\xa5""B)");
        Tooltip* tp = tip.get();
        add(std::move(tip), kMargin + 120.0f, y + 2.0f, 120.0f, 24.0f);
        tp->show_at({kMargin + 120.0f, y + 26.0f});      // make it visible (fade settles below)
        for (int i = 0; i < 20; ++i) tp->advance_animations(0.05f);
        y += 44.0f;
    }

    // ── Data — Table ───────────────────────────────────────────────────
    section("Data — Table");
    {
        // Model is non-owning from the table's perspective; a function-static
        // keeps it alive for the single showcase instance's lifetime.
        // Columns + data mirror the Figma "Table · TableListBox" (NAME / TYPE /
        // CPU) with a teal-selected row, not the old NAME/NOTE/GAIN placeholder.
        static SimpleTableModel table_model;
        table_model.set_data({{"E-Piano", "Instrument", "4.2%"},
                              {"Reverb", "FX", "1.8%"},
                              {"Arp", "MIDI FX", "0.9%"},
                              {"Master", "Bus", "2.6%"}});
        auto table = std::make_unique<TableListBox>();
        table->add_column({"NAME", 150.0f});
        table->add_column({"TYPE", 110.0f});
        table->add_column({"CPU", 70.0f});
        table->set_model(&table_model);
        table->set_selected_row(1);  // teal-highlighted row (matches Figma)
        add(std::move(table), kMargin, y, 340.0f, 150.0f);
        y += 170.0f;
    }

    // ── Navigation — Sidebar · PopupMenu ───────────────────────────────
    section("Navigation — Sidebar · PopupMenu");
    {
        // Sidebar == SidePanel (a chrome-less slide container), so to read as a
        // sidebar it hosts a visible icon-rail Panel. Children are absolute so
        // the board's Yoga pass places them deterministically.
        auto side = std::make_unique<Sidebar>();
        side->set_edge(Sidebar::Edge::left);
        side->set_extent(72.0f);
        side->open();
        for (int i = 0; i < 30; ++i) side->advance_animations(0.05f);  // settle open
        auto rail = std::make_unique<Panel>();
        rail->set_position(View::Position::absolute);
        rail->set_left(0); rail->set_top(0);
        rail->flex().preferred_width = 72.0f; rail->flex().preferred_height = 150.0f;
        rail->set_bounds({0, 0, 72.0f, 150.0f});
        // Full-width, center-aligned icon labels, evenly spaced so glyphs of
        // differing widths line up and never overlap.
        const char* icons[] = {"\xe2\x99\xaa", "\xe2\x96\xa4", "\xe2\x9a\x99", "\xe2\x98\xb0"};
        const float row_h = 28.0f, gap = 6.0f, first_top = 12.0f;
        for (int i = 0; i < 4; ++i) {
            float top = first_top + static_cast<float>(i) * (row_h + gap);
            auto ic = std::make_unique<Label>(icons[i]);
            ic->set_font_size(17.0f);
            ic->set_text_align(LabelAlign::center);
            ic->set_position(View::Position::absolute);
            ic->set_left(0.0f); ic->set_top(top);
            ic->flex().preferred_width = 72.0f; ic->flex().preferred_height = row_h;
            ic->set_bounds({0.0f, top, 72.0f, row_h});
            rail->add_child(std::move(ic));
        }
        side->add_child(std::move(rail));
        add(std::move(side), kMargin, y, 72.0f, 150.0f);

        auto menu = std::make_unique<PopupMenu>();  // == ContextMenu (Figma name)
        menu->set_items({{1, "Init Patch"}, {2, "Randomize"},
                         PopupMenu::Item::make_separator(), {3, "Save As\xe2\x80\xa6"}});
        menu->set_anchor({0, 0});
        add(std::move(menu), kMargin + 110.0f, y, 180.0f, 104.0f);

        // Labeled sidebar nav — the Figma "sidebar" (227:1830) reads as a
        // labeled list with a teal-tinted selected row. Rendered with ListBox's
        // opt-in accent selection + leading icons (the SidePanel above is the
        // chrome-less slide container; this is the list it would host).
        auto nav = std::make_unique<ListBox>();
        nav->set_items({"Oscillators", "Filter", "Envelopes", "Effects"});
        nav->set_icons({"\xe2\x99\xaa", "\xe2\x96\xa4", "\xe2\x9a\x99", "\xe2\x98\xb0"});
        nav->set_selection_style(ListBox::SelectionStyle::accent);
        nav->set_row_height(32.0f);
        nav->set_selected(1);
        add(std::move(nav), kMargin + 320.0f, y, 200.0f, 150.0f);
        y += 170.0f;
    }

    // ── Overlays — Dialog · Popover ────────────────────────────────────
    section("Overlays — Dialog · Popover");
    {
        auto pop = std::make_unique<Popover>();
        pop->set_title("Quantize");
        add(std::move(pop), kMargin, y, 220.0f, 120.0f);

        auto dlg = std::make_unique<InCanvasDialog>();   // == Dialog (Figma name)
        dlg->set_title("Discard changes?");
        dlg->set_message("Your edits to Velvet Plate will be lost.");
        dlg->set_confirm_label("Discard");
        dlg->set_cancel_label("Cancel");
        dlg->set_destructive(true);
        add(std::move(dlg), kMargin + 260.0f, y, 380.0f, 180.0f);
        y += 200.0f;
    }

    // ── Interaction states — expanded · remove · editing ──────────────────
    section("Interaction states");
    {
        // Output routing — a ComboBox shown EXPANDED (opened via a simulated
        // click since open_dropdown() is internal).
        label("OUTPUT ROUTING \xc2\xb7 EXPANDED", kMargin, y, 260.0f, 11.0f);
        auto* cb = static_cast<ComboBox*>(add(std::make_unique<ComboBox>(), kMargin, y + 16.0f, 200.0f, 32.0f));
        cb->set_items({"Stereo Out", "Mono Sum", "Bus 1 \xc2\xb7 Drums", "Bus 2 \xc2\xb7 Vox", "No Output"});
        cb->set_selected(0);
        { MouseEvent ev{}; ev.is_down = true; ev.position = {100.0f, 16.0f}; cb->on_mouse_event(ev); }

        // Insert — a row with its remove/bypass menu shown.
        label("INSERT \xc2\xb7 REMOVE MENU", kMargin + 280.0f, y, 240.0f, 11.0f);
        add(std::make_unique<Badge>("EQ", Tone::info), kMargin + 280.0f, y + 16.0f, 70.0f, 28.0f);
        { auto menu = std::make_unique<PopupMenu>();
          menu->set_items({{1, "Bypass"}, {2, "Replace\xe2\x80\xa6"},
                           PopupMenu::Item::make_separator(), {3, "Remove"}});
          menu->set_anchor({0, 0});
          add(std::move(menu), kMargin + 280.0f, y + 54.0f, 180.0f, 110.0f); }

        // Value field — selected & editing (caret + accent ring).
        label("VALUE FIELD \xc2\xb7 EDITING", kMargin + 540.0f, y, 240.0f, 11.0f);
        auto* ve = static_cast<InlineValueEditor*>(add(std::make_unique<InlineValueEditor>(), kMargin + 540.0f, y + 16.0f, 110.0f, 30.0f));
        ve->set_range(-60, 0); ve->set_decimals(1); ve->set_suffix(" dB"); ve->set_value(-12.0);
        ve->begin_edit();
        y += 200.0f;
    }

    out_height = y + 24.0f;
    // Size the board so the window's Yoga pass lays it out to the full content
    // extent (its absolute children are positioned within this box).
    b->flex().preferred_width = kContentW + 2.0f * kMargin;
    b->flex().preferred_height = out_height;
    b->flex().flex_shrink = 0.0f;
    return board;
}

// ── Faithful Overview ────────────────────────────────────────────────────
// The showcase body: the design's Overview rendered 1:1 via DesignFrameView
// (SkSVGDOM), swapping the dark/light SVG export to match the active theme.
// This makes the app match the Figma Overview page, with only a showcase title
// and the system/light/dark toggle added on top.
class FaithfulOverview : public View {
public:
    FaithfulOverview(std::string dark_svg, std::string light_svg, float w, float h) {
        auto mk = [&](std::string svg) -> DesignFrameView* {
            auto v = std::make_unique<DesignFrameView>(std::move(svg),
                                                       std::vector<DesignFrameElement>{});
            v->set_bounds({0.0f, 0.0f, w, h});
            DesignFrameView* p = v.get();
            add_child(std::move(v));
            return p;
        };
        dark_ = mk(std::move(dark_svg));
        light_ = mk(std::move(light_svg));
        set_dark(true);
    }
    void set_dark(bool d) {
        if (dark_) dark_->set_visible(d);
        if (light_) light_->set_visible(!d);
        request_repaint();
    }
private:
    DesignFrameView* dark_ = nullptr;
    DesignFrameView* light_ = nullptr;
};

std::string read_showcase_asset(const char* name) {
    std::string path = std::string(SHOWCASE_ASSET_DIR) + "/" + name;
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::fprintf(stderr, "warning: cannot read asset %s\n", path.c_str()); return {}; }
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

View* add_abs(Board* b, std::unique_ptr<View> v, float x, float y, float w, float h) {
    v->set_bounds({x, y, w, h});
    v->set_position(View::Position::absolute);
    v->set_left(x);
    v->set_top(y);
    v->flex().preferred_width = w;
    v->flex().preferred_height = h;
    View* p = v.get();
    b->add_child(std::move(v));
    return p;
}

// Build the showcase as the faithful Figma Overview + a showcase title and the
// theme toggle (the only two deliberate departures from the Overview).
std::unique_ptr<View> build_overview_board(float& out_height,
                                           ThemeModeControl*& out_theme_ctl,
                                           FaithfulOverview*& out_overview) {
    auto board = std::make_unique<Board>();
    Board* b = board.get();

    auto title = std::make_unique<Label>("Ink & Signal — Showcase");
    title->set_font_size(24.0f);
    add_abs(b, std::move(title), kMargin, 24.0f, kContentW - 130.0f, 30.0f);

    auto ctl = std::make_unique<ThemeModeControl>();
    out_theme_ctl = static_cast<ThemeModeControl*>(
        add_abs(b, std::move(ctl), kMargin + kContentW - 104.0f, 24.0f, 104.0f, 28.0f));

    // The Overview SVG panel is ~1400×11011; fit its width to the content area
    // (DesignFrameView preserves aspect).
    const float ovY = 72.0f;
    const float ovW = kContentW;
    const float ovH = ovW * (11011.0f / 1400.0f);
    auto ov = std::make_unique<FaithfulOverview>(read_showcase_asset("overview-dark.svg"),
                                                 read_showcase_asset("overview-light.svg"),
                                                 ovW, ovH);
    out_overview = ov.get();
    add_abs(b, std::move(ov), kMargin, ovY, ovW, ovH);

    out_height = ovY + ovH + 24.0f;
    b->flex().preferred_width = kContentW + 2.0f * kMargin;
    b->flex().preferred_height = out_height;
    b->flex().flex_shrink = 0.0f;
    return board;
}

}  // namespace

int main(int argc, char** argv) {
    std::string theme_name = "dark";
    std::string preset = "ink-signal";
    std::string screenshot;
    bool fit = false;       // --fit: aspect-locked proportional resize instead of scroll
    bool overview = false;  // --overview: faithful 1:1 Figma render (static reference)
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--theme") && i + 1 < argc) theme_name = argv[++i];
        else if (!std::strcmp(argv[i], "--preset") && i + 1 < argc) preset = argv[++i];
        else if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot = argv[++i];
        else if (!std::strcmp(argv[i], "--fit")) fit = true;
        else if (!std::strcmp(argv[i], "--overview")) overview = true;
    }

    const ThemePreset* p = find_preset(preset);
    if (!p) { std::fprintf(stderr, "unknown preset '%s'\n", preset.c_str()); return 1; }

    // Theme manager: the Ink & Signal light/dark pair, with the mode chosen by
    // --theme (system follows the OS live; light/dark pin it). This is the
    // smart SDK surface — set_mode maps onto OS-follow vs pinned.
    ThemeManager theme_mgr;
    theme_mgr.set_theme_pair(theme_from_preset(*p, /*dark=*/false),
                             theme_from_preset(*p, /*dark=*/true));
    ThemeMode init_mode = theme_name == "system" ? ThemeMode::system
                        : theme_name == "light"  ? ThemeMode::light
                                                 : ThemeMode::dark;
    theme_mgr.set_mode(init_mode);

    const uint32_t W = static_cast<uint32_t>(kContentW + 2.0f * kMargin);
    const uint32_t winH = 820;  // initial window height; content scrolls beyond it

    float content_h = 0.0f;
    ThemeModeControl* theme_ctl = nullptr;
    // Default: the interactive native-widget gallery (real, draggable, theme-
    // inheriting reusable components). `--overview` instead renders the faithful
    // 1:1 Figma Overview as a static reference (a fidelity proof, not the app).
    FaithfulOverview* overview_view = nullptr;
    auto board = overview ? build_overview_board(content_h, theme_ctl, overview_view)
                          : build_board(content_h, theme_ctl);
    if (theme_ctl) {
        theme_ctl->set_mode(theme_mgr.mode());
        theme_ctl->on_mode_change = [&theme_mgr](ThemeMode m) { theme_mgr.set_mode(m); };
    }

    // Pick the faithful-Overview SVG variant (dark/light) that matches the
    // resolved theme: light/dark pin it, system follows the OS appearance.
    auto effective_dark = [&theme_mgr]() -> bool {
        switch (theme_mgr.mode()) {
            case ThemeMode::light: return false;
            case ThemeMode::dark:  return true;
            default: return theme_mgr.tracker().current_appearance() == Appearance::dark;
        }
    };
    if (overview_view) overview_view->set_dark(effective_dark());

    board->set_theme(theme_mgr.active_theme());
    board->set_bounds({0, 0, static_cast<float>(W), content_h});

    // Headless GPU/Skia render — render the board directly (full content height)
    // so the screenshot shows every widget.
    if (!screenshot.empty()) {
        auto png = render_to_png(*board, W, static_cast<uint32_t>(content_h), 2.0f,
                                 ScreenshotBackend::skia);
        if (png.empty()) { std::fprintf(stderr, "render produced no PNG (no Skia/GPU backend?)\n"); return 1; }
        std::ofstream out(screenshot, std::ios::binary);
        out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        std::printf("wrote %s (%ux%u, %zu bytes, Skia)\n", screenshot.c_str(), W,
                    static_cast<uint32_t>(content_h), png.size());
        return 0;
    }

    FrameClock clock;
    WindowOptions opts;
    opts.title = "Ink & Signal — Showcase";
    opts.width = static_cast<float>(W);
    opts.height = fit ? content_h : static_cast<float>(winH);
    // The board is a fixed kContentW-wide design. Rather than pin the window to
    // the full content width (which forced a wide minimum), the ScrollView below
    // scrolls BOTH axes — so a window narrower than the content pans horizontally
    // instead of cropping the right-side chrome (Master strip, breadcrumb). That
    // lets the window shrink to a small min in both dimensions with the content
    // fully reachable by scroll.
    opts.min_width = 480.0f;
    opts.min_height = 480.0f;
    opts.use_gpu = true;        // GPU (Skia Graphite / Dawn) when available

    View* board_ptr = board.get();
    std::unique_ptr<WindowHost> window;
    std::unique_ptr<ScrollView> scroll;  // kept alive for the scroll path

    if (fit) {
        // Aspect-locked proportional resize: the design viewport pins the board
        // to its design size and scales it to fit the window (letterboxed),
        // with inverse input mapping — no scroll, content never crops.
        board->set_frame_clock(&clock);
        window = WindowHost::create(*board, opts);
        if (!window) { std::fprintf(stderr, "failed to create window host\n"); return 1; }
        window->set_design_viewport(static_cast<float>(W), content_h);
    } else {
        // Default: wrap the board in a ScrollView so content scrolls
        // (trackpad / wheel). Scroll BOTH axes — vertical for the tall gallery,
        // horizontal so a window narrower than the fixed-width board pans
        // instead of cropping the right-side chrome (see min_width above).
        scroll = std::make_unique<ScrollView>();
        scroll->set_direction(ScrollView::Direction::both);
        scroll->set_theme(theme_mgr.active_theme());
        scroll->set_frame_clock(&clock);
        scroll->add_child(std::move(board));
        scroll->set_content_size({static_cast<float>(W), content_h});
        window = WindowHost::create(*scroll, opts);
        if (!window) { std::fprintf(stderr, "failed to create window host\n"); return 1; }
    }

    WindowHost* win = window.get();
    ScrollView* scroll_ptr = scroll.get();

    // Re-apply the active theme to the whole tree whenever it changes — from a
    // manual toggle (ThemeModeControl → set_mode) or a live OS appearance flip
    // (theme_mgr.poll() below). resolve_color walks the parent chain, so
    // setting the theme on the root + board restyles every widget.
    auto apply_theme = [board_ptr, scroll_ptr, overview_view, effective_dark](const Theme& t) {
        if (scroll_ptr) scroll_ptr->set_theme(t);
        board_ptr->set_theme(t);
        // Swap the faithful-Overview SVG variant to match the new appearance.
        if (overview_view) overview_view->set_dark(effective_dark());
        board_ptr->request_repaint();
    };
    theme_mgr.on_theme_changed([&apply_theme](const Theme& t) { apply_theme(t); });

    window->set_idle_callback([win, board_ptr, theme_ctl, overview_view, &theme_mgr,
                               effective_dark]() {
        constexpr float dt = 1.0f / 60.0f;
        // Live OS-appearance follow: in system mode this swaps the theme when
        // the OS toggles light/dark (fires on_theme_changed → apply_theme).
        theme_mgr.poll();
        if (theme_ctl) theme_ctl->set_mode(theme_mgr.mode());
        // Keep the Overview SVG variant in sync (covers the system-mode OS flip
        // even if no Theme object changed identity).
        if (overview_view) overview_view->set_dark(effective_dark());
        // Tick widget animations (the title/toggle chrome); the Overview render
        // itself is a static faithful vector.
        static float t = 0.0f;
        t += dt;
        g_mod_lfo = std::sin(t * 1.5f);
        advance_anims(board_ptr, dt);
        (void)win;
        board_ptr->request_repaint();
    });
    window->set_close_callback([]() {});

    std::printf("Ink & Signal showcase — %s, GPU window (%s). Close to exit.\n",
                theme_name.c_str(),
                fit ? "proportional fit" : "scroll for more");
    window->run_event_loop();
    return 0;
}
