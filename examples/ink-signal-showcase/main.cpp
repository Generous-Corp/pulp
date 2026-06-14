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
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/gap_widgets.hpp>
#include <pulp/view/midi_keyboard.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/scroll_bar.hpp>
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

// Advance animated widgets each frame (hover glow, toggle slide, scroll easing).
void advance_anims(View* v, float dt) {
    if (!v) return;
    if (auto* k = dynamic_cast<Knob*>(v)) k->advance_animations(dt);
    else if (auto* t = dynamic_cast<Toggle*>(v)) t->advance_animations(dt);
    else if (auto* f = dynamic_cast<Fader*>(v)) f->advance_animations(dt);
    else if (auto* r = dynamic_cast<RangeSlider*>(v)) r->advance_animations(dt);
    else if (auto* p = dynamic_cast<PanControl*>(v)) p->advance_animations(dt);
    else if (auto* s = dynamic_cast<ScrollView*>(v)) s->advance_animations(dt);
    for (std::size_t i = 0; i < v->child_count(); ++i) advance_anims(v->child_at(i), dt);
}

std::unique_ptr<View> build_board(float& out_height) {
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
            add(std::move(k), x, y, 84.0f, 84.0f); x += 96.0f;
        }
        y += 104.0f;
    }

    // ── Knob modulation (Saturn rings) ─────────────────────────────────
    section("Knob modulation");
    {
        // Brand modulation-source colours (LFO blue, ENV amber, VEL pink,
        // MACRO violet) — match the Figma "Knob Modulation" set.
        const Color LFO = Color::hex(0x5E78FF), ENV = Color::hex(0xF6B847),
                    VEL = Color::hex(0xFF7AA8), MAC = Color::hex(0x8B6CF5);
        struct Spec { const char* name; float val; std::vector<Knob::ModulationRing> rings; };
        std::vector<Spec> specs = {
            {"Positive", 0.5f, {{0.5f, ENV}}},
            {"Negative", 0.6f, {{-0.4f, LFO}}},
            {"Bipolar", 0.5f, {{0.55f, MAC}}},
            {"2 sources", 0.45f, {{0.5f, LFO}, {-0.3f, ENV}}},
            {"3 sources", 0.5f, {{0.4f, LFO}, {0.6f, ENV}, {-0.5f, VEL}}},
        };
        float x = kMargin;
        for (auto& s : specs) {
            auto k = std::make_unique<Knob>(); k->set_value(s.val); k->set_label(s.name);
            k->set_modulation_rings(s.rings);
            add(std::move(k), x, y, 92.0f, 92.0f); x += 104.0f;
        }
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
        auto tabs = std::make_unique<TabPanel>();
        tabs->add_tab("Amp", std::make_unique<View>());
        tabs->add_tab("Filter", std::make_unique<View>());
        tabs->add_tab("FX", std::make_unique<View>());
        tabs->set_active_tab(0);
        add(std::move(tabs), kMargin, y, 280.0f, 60.0f);

        auto tb = std::make_unique<Toolbar>();
        tb->add_button("play", "Play", [] {});
        tb->add_toggle("loop", "Loop", [](bool) {});
        tb->add_separator();
        tb->add_button("rec", "Rec", [] {});
        add(std::move(tb), kMargin + 320.0f, y, 280.0f, 40.0f);

        auto bc = std::make_unique<Breadcrumb>();
        bc->set_items({{"Home", {}}, {"Synths", {}}, {"Bass", {}}});
        add(std::move(bc), kMargin + 320.0f, y + 48.0f, 280.0f, 24.0f);
        y += 72.0f;

        auto tree = std::make_unique<TreeView>();
        auto& root = tree->root();
        auto& synths = root.add_child("Synths");
        synths.add_child("Bass"); synths.add_child("Lead");
        auto& fx = root.add_child("Effects");
        fx.add_child("Reverb"); fx.add_child("Delay");
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

    // ── Containers / mixer (interactive faders) ────────────────────────
    section("Containers · Mixer");
    {
        auto panel = std::make_unique<Panel>();
        add(std::move(panel), kMargin, y, 200.0f, 150.0f);
        label("Panel", kMargin + 12.0f, y + 10.0f, 120.0f, 12.0f);

        // Real interactive faders (draggable), plus a display ChannelStrip.
        float fx = kMargin + 230.0f;
        const char* chans[] = {"Drums", "Bass", "Synth", "Keys"};
        const float fl[] = {0.7f, 0.5f, 0.82f, 0.6f};
        for (int i = 0; i < 4; ++i) {
            auto f = std::make_unique<Fader>(); f->set_value(fl[i]);
            add(std::move(f), fx, y + 6.0f, 26.0f, 120.0f);
            label(chans[i], fx - 6.0f, y + 132.0f, 40.0f, 11.0f);
            fx += 60.0f;
        }
        auto cs = std::make_unique<ChannelStrip>(); cs->set_label("Master"); cs->set_level(0.75f); cs->set_pan(0.0f);
        add(std::move(cs), fx + 20.0f, y, 84.0f, 150.0f);
        y += 168.0f;
    }

    out_height = y + 24.0f;
    // Size the board so the window's Yoga pass lays it out to the full content
    // extent (its absolute children are positioned within this box).
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
    bool fit = false;  // --fit: aspect-locked proportional resize instead of scroll
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--theme") && i + 1 < argc) theme_name = argv[++i];
        else if (!std::strcmp(argv[i], "--preset") && i + 1 < argc) preset = argv[++i];
        else if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot = argv[++i];
        else if (!std::strcmp(argv[i], "--fit")) fit = true;
    }

    const ThemePreset* p = find_preset(preset);
    if (!p) { std::fprintf(stderr, "unknown preset '%s'\n", preset.c_str()); return 1; }
    const bool dark = theme_name != "light";

    const uint32_t W = static_cast<uint32_t>(kContentW + 2.0f * kMargin);
    const uint32_t winH = 820;  // initial window height; content scrolls beyond it

    float content_h = 0.0f;
    auto board = build_board(content_h);
    board->set_theme(theme_from_preset(*p, dark));
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
    opts.min_width = 560.0f;    // never crop below a usable width
    opts.min_height = 420.0f;
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
        // (trackpad / wheel).
        scroll = std::make_unique<ScrollView>();
        scroll->set_theme(theme_from_preset(*p, dark));
        scroll->set_frame_clock(&clock);
        scroll->add_child(std::move(board));
        scroll->set_content_size({static_cast<float>(W), content_h});
        window = WindowHost::create(*scroll, opts);
        if (!window) { std::fprintf(stderr, "failed to create window host\n"); return 1; }
    }

    WindowHost* win = window.get();
    window->set_idle_callback([win, board_ptr]() {
        advance_anims(board_ptr, 1.0f / 60.0f);
        (void)win;
        board_ptr->request_repaint();
    });
    window->set_close_callback([]() {});

    std::printf("Ink & Signal showcase — %s, GPU window (%s). Close to exit.\n",
                dark ? "dark" : "light",
                fit ? "proportional fit" : "scroll for more");
    window->run_event_loop();
    return 0;
}
