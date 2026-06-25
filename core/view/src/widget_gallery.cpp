#include <pulp/view/widget_gallery.hpp>

#include <pulp/view/buttons.hpp>
#include <pulp/view/gap_widgets.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/canvas/canvas.hpp>

#include <memory>

namespace pulp::view {

namespace {
using canvas::Color;

// Root that paints the app background + section dividers so the headless render
// isn't a transparent frame.
class GalleryRoot : public View {
public:
    void paint(canvas::Canvas& canvas) override {
        canvas.set_fill_color(resolve_color("bg.primary", Color::rgba8(22, 26, 33)));
        canvas.fill_rect(0, 0, bounds().width, bounds().height);
    }
    // The gallery is a hand-laid-out board: children carry explicit bounds, so
    // suppress the flex/Yoga pass (render_to_png calls layout_children()) which
    // would otherwise stretch every widget to the full board width.
    void layout_children() override {}
};

}  // namespace

std::unique_ptr<View> build_widget_gallery(const Theme& theme) {
    auto root = std::make_unique<GalleryRoot>();
    root->set_theme(theme);

    const float W = GALLERY_WIDTH, M = 40.0f;
    float y = 24.0f;

    auto add = [&](std::unique_ptr<View> v, float x, float yy, float w, float h) -> View* {
        v->set_bounds({x, yy, w, h});
        View* p = v.get();
        root->add_child(std::move(v));
        return p;
    };
    auto header = [&](const std::string& text) {
        auto l = std::make_unique<Label>(text);
        l->set_font_size(13.0f);
        add(std::move(l), M, y, W - 2 * M, 18.0f);
        y += 26.0f;
    };

    // Title
    {
        auto t = std::make_unique<Label>("Ink & Signal — Widget Gallery");
        t->set_font_size(26.0f);
        add(std::move(t), M, y, W - 2 * M, 34.0f);
        y += 52.0f;
    }

    // ── Buttons ──
    header("Buttons");
    add(std::make_unique<TextButton>("Render"), M, y, 96.0f, 36.0f);
    add(std::make_unique<TextButton>("Export"), M + 110.0f, y, 96.0f, 36.0f);
    add(std::make_unique<ArrowButton>(ArrowDirection::right), M + 220.0f, y + 6.0f, 24.0f, 24.0f);
    {
        auto tog = std::make_unique<ToggleButton>(); tog->set_on(true); tog->set_label("Loop");
        add(std::move(tog), M + 268.0f, y, 90.0f, 36.0f);
    }
    y += 56.0f;

    // ── Controls ──
    header("Controls");
    {
        float kx = M;
        for (float v : {0.18f, 0.5f, 0.86f}) {
            auto k = std::make_unique<Knob>(); k->set_value(v);
            add(std::move(k), kx, y, 96.0f, 96.0f); kx += 116.0f;
        }
        auto f = std::make_unique<Fader>(); f->set_value(0.62f);
        add(std::move(f), kx, y, 26.0f, 96.0f); kx += 56.0f;
        auto rs = std::make_unique<RangeSlider>(); rs->set_min(0.0f); rs->set_max(1.0f);
        add(std::move(rs), kx, y + 40.0f, 220.0f, 18.0f);
    }
    y += 112.0f;
    {
        auto st = std::make_unique<Stepper>(); st->set_range(-24, 24); st->set_value(2); st->set_suffix("st");
        add(std::move(st), M, y, 140.0f, 36.0f);
        auto pan = std::make_unique<PanControl>(); pan->set_value(-0.4f);
        add(std::move(pan), M + 160.0f, y + 9.0f, 200.0f, 18.0f);
        auto pb = std::make_unique<ProgressBar>(); pb->set_progress(0.6f);
        add(std::move(pb), M + 390.0f, y + 13.0f, 240.0f, 10.0f);
    }
    y += 56.0f;

    // ── Status & feedback ──
    header("Status & feedback");
    {
        float bx = M;
        const Tone tones[] = {Tone::neutral, Tone::info, Tone::success, Tone::warning, Tone::danger};
        const char* labels[] = {"VST3", "Info", "Active", "48 kHz", "Peak"};
        for (int i = 0; i < 5; ++i) {
            add(std::make_unique<Badge>(labels[i], tones[i]), bx, y, 66.0f, 22.0f); bx += 78.0f;
        }
    }
    y += 38.0f;
    {
        auto b1 = std::make_unique<InlineBanner>(); b1->set_tone(Tone::success);
        b1->set_label("Build succeeded."); b1->set_message("VST3 · AU · CLAP signed in 4.8s.");
        add(std::move(b1), M, y, 420.0f, 46.0f);
        auto b2 = std::make_unique<InlineBanner>(); b2->set_tone(Tone::danger);
        b2->set_label("Render failed."); b2->set_message("Output device unavailable.");
        add(std::move(b2), M + 440.0f, y, 420.0f, 46.0f);
    }
    y += 58.0f;
    {
        auto toast = std::make_unique<Toast>(); toast->set_title("Preset saved");
        toast->set_subtitle("Velvet Plate → User library"); toast->set_action("Undo");
        add(std::move(toast), M, y, 420.0f, 64.0f);
        auto empty = std::make_unique<EmptyState>(); empty->set_message("No presets yet —"); empty->set_action("create one");
        add(std::move(empty), M + 440.0f, y, 420.0f, 64.0f);
    }
    y += 80.0f;

    // ── Mixer ──
    header("Mixer");
    {
        float cx = M;
        const char* names[] = {"Drums", "Bass", "Synth"};
        const float lvls[] = {0.7f, 0.55f, 0.82f};
        const float pans[] = {-0.3f, 0.0f, 0.4f};
        for (int i = 0; i < 3; ++i) {
            auto cs = std::make_unique<ChannelStrip>();
            cs->set_label(names[i]); cs->set_level(lvls[i]); cs->set_pan(pans[i]);
            add(std::move(cs), cx, y, 84.0f, 220.0f); cx += 100.0f;
        }
        // Popover specimen alongside the strips
        auto po = std::make_unique<Popover>(); po->set_title("Quantize");
        add(std::move(po), cx + 40.0f, y, 240.0f, 120.0f);
    }
    y += 244.0f;

    root->set_bounds({0, 0, W, y});
    return root;
}

}  // namespace pulp::view
