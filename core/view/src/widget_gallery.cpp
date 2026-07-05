#include <pulp/view/widget_gallery.hpp>

#include <pulp/view/buttons.hpp>
#include <pulp/view/gap_widgets.hpp>
#include <pulp/view/midi_keyboard.hpp>
#include <pulp/view/text_editor.hpp>
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

    // ── Inputs ──
    header("Inputs");
    {
        auto in = std::make_unique<TextEditor>();
        in->placeholder = "Search presets…";
        in->set_text("Velvet Plate");
        add(std::move(in), M, y, 260.0f, 32.0f);

        auto combo = std::make_unique<ComboBox>();
        combo->set_items({"Sine", "Saw", "Square", "Triangle"});
        combo->set_selected_silent(1);
        add(std::move(combo), M + 280.0f, y, 160.0f, 32.0f);

        auto cb = std::make_unique<Checkbox>(); cb->set_checked(true);
        add(std::move(cb), M + 470.0f, y + 4.0f, 22.0f, 22.0f);
        auto cbl = std::make_unique<Label>("Sync"); cbl->set_font_size(12.0f);
        add(std::move(cbl), M + 498.0f, y + 6.0f, 60.0f, 18.0f);

        auto sw = std::make_unique<Toggle>(); sw->set_on(true, false); sw->set_label("Mono");
        add(std::move(sw), M + 580.0f, y, 100.0f, 32.0f);
    }
    y += 44.0f;
    {
        auto memo = std::make_unique<TextEditor>();
        memo->multi_line = true;
        memo->set_text("Multi-line text input.\nBinds to plugin state.");
        add(std::move(memo), M, y, 440.0f, 64.0f);
    }
    y += 84.0f;

    // ── XY pad & segmented ──
    header("XY pad & segmented");
    {
        auto xy = std::make_unique<XYPad>();
        xy->set_x(0.62f); xy->set_y(0.40f);
        xy->set_x_label("Cutoff"); xy->set_y_label("Reso");
        add(std::move(xy), M, y, 150.0f, 150.0f);

        auto seg = std::make_unique<SegmentedControl>();
        seg->set_segments({"LP", "BP", "HP"}); seg->set_selected_silent(1);
        add(std::move(seg), M + 180.0f, y + 8.0f, 220.0f, 32.0f);

        auto seg2 = std::make_unique<SegmentedControl>();
        seg2->set_segments({"1x", "2x", "4x", "8x"}); seg2->set_selected_silent(2);
        add(std::move(seg2), M + 180.0f, y + 52.0f, 220.0f, 32.0f);
    }
    y += 168.0f;

    // ── Meters & keyboard ──
    header("Meters & keyboard");
    {
        auto m1 = std::make_unique<Meter>();
        m1->set_orientation(Meter::Orientation::vertical); m1->set_level(0.55f, 0.82f);
        add(std::move(m1), M, y, 22.0f, 130.0f);
        auto m2 = std::make_unique<Meter>();
        m2->set_orientation(Meter::Orientation::vertical); m2->set_level(0.38f, 0.64f);
        add(std::move(m2), M + 32.0f, y, 22.0f, 130.0f);

        auto kb = std::make_unique<MidiKeyboard>();
        kb->set_range(48, 72);
        add(std::move(kb), M + 84.0f, y + 46.0f, 700.0f, 84.0f);
    }
    y += 150.0f;

    root->set_bounds({0, 0, W, y});
    return root;
}

std::unique_ptr<View> build_scrolling_widget_gallery(const Theme& theme,
                                                     float viewport_w,
                                                     float viewport_h) {
    auto board = build_widget_gallery(theme);
    const auto content = board->bounds();

    auto scroll = std::make_unique<ScrollView>();
    scroll->set_theme(theme);
    scroll->set_direction(ScrollView::Direction::both);
    scroll->set_content_size({content.width, content.height});
    scroll->set_bounds({0, 0, viewport_w, viewport_h});
    scroll->add_child(std::move(board));
    return scroll;
}

}  // namespace pulp::view
