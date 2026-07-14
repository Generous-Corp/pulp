// Component shots — renders each core Ink & Signal primitive in isolation, in
// light and dark, to PNG cells, for a Figma-vs-Pulp comparison contact sheet.
// One-off verification tool (not a shipped feature surface).
//
//   pulp-component-shots --out /tmp/shots/native
//
// Writes <out>/<name>-light.png and <out>/<name>-dark.png per component.

#include <pulp/design/design_system.hpp>
#include <pulp/view/musical_typing_keyboard.hpp>
#include <pulp/view/screenshot.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace pulp::design;
using namespace pulp::view;
using pulp::canvas::Color;
namespace canvas = pulp::canvas;

namespace {

// A fixed-size cell that paints the theme app background; children carry
// explicit bounds (suppress the flex pass).
class Cell : public View {
public:
    void paint(canvas::Canvas& c) override {
        c.set_fill_color(resolve_color("bg.primary", Color::rgba8(22, 26, 33)));
        c.fill_rect(0, 0, bounds().width, bounds().height);
    }
    void layout_children() override {}
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

struct Entry {
    std::string name;
    float w, h;
    std::function<void(Cell&)> build;   // add widget(s) with explicit bounds
};

std::vector<Entry> entries() {
    std::vector<Entry> e;
    auto add = [&](std::string n, float w, float h, std::function<void(Cell&)> b) {
        e.push_back({std::move(n), w, h, std::move(b)});
    };

    add("Knob", 140, 140, [](Cell& c) {
        auto k = std::make_unique<Knob>(); k->set_value(0.62f); k->set_label("Cutoff");
        k->set_bounds({22, 16, 96, 96}); c.add_child(std::move(k));
    });
    add("Fader", 90, 160, [](Cell& c) {
        auto f = std::make_unique<Fader>(); f->set_value(0.62f);
        f->set_bounds({32, 20, 26, 120}); c.add_child(std::move(f));
    });
    add("Slider", 260, 70, [](Cell& c) {
        auto s = std::make_unique<RangeSlider>(); s->set_min(0); s->set_max(1); s->set_value(0.4f);
        s->set_bounds({20, 26, 220, 18}); c.add_child(std::move(s));
    });
    add("Toggle", 110, 70, [](Cell& c) {
        auto t = std::make_unique<Toggle>(); t->set_on(true);
        t->advance_animations(1.0f);   // settle thumb to target for static render
        t->set_bounds({29, 20, 52, 30}); c.add_child(std::move(t));
    });
    add("Checkbox", 90, 70, [](Cell& c) {
        auto cb = std::make_unique<Checkbox>(); cb->set_checked(true);
        cb->set_bounds({34, 24, 22, 22}); c.add_child(std::move(cb));
    });
    add("Button", 170, 70, [](Cell& c) {
        auto b = std::make_unique<TextButton>("Render");
        b->set_style(TextButton::Style::primary);
        b->set_bounds({30, 17, 110, 36}); c.add_child(std::move(b));
    });
    add("Badge", 150, 210, [](Cell& c) {
        const char* labels[] = {"Neutral", "Info", "Active", "48 kHz", "Peak"};
        const Tone tones[] = {Tone::neutral, Tone::info, Tone::success,
                              Tone::warning, Tone::danger};
        float y = 16;
        for (int i = 0; i < 5; ++i) {
            auto bd = std::make_unique<Badge>(labels[i], tones[i]);
            bd->set_bounds({34, y, 82, 24}); c.add_child(std::move(bd));
            y += 36;
        }
    });
    add("Meter", 300, 130, [](Cell& c) {
        // Three horizontal meters at Low/Mid/Peak levels (green/yellow/red
        // zones), matching the Figma Meter board.
        const float lvls[] = {0.45f, 0.8f, 0.97f};
        float y = 24;
        for (int i = 0; i < 3; ++i) {
            auto m = std::make_unique<Meter>();
            m->set_orientation(Meter::Orientation::horizontal);
            m->set_level(lvls[i], lvls[i]);
            m->set_bounds({24, y, 250, 14}); c.add_child(std::move(m));
            y += 34;
        }
    });
    add("ProgressBar", 260, 60, [](Cell& c) {
        auto p = std::make_unique<ProgressBar>(); p->set_progress(0.6f);
        p->set_bounds({20, 25, 220, 10}); c.add_child(std::move(p));
    });
    add("ComboBox", 220, 70, [](Cell& c) {
        auto cb = std::make_unique<ComboBox>();
        cb->set_items({"Sine", "Saw", "Square"}); cb->set_selected(1);
        cb->set_bounds({20, 19, 180, 32}); c.add_child(std::move(cb));
    });
    add("Stepper", 200, 70, [](Cell& c) {
        auto s = std::make_unique<Stepper>(); s->set_range(-24, 24); s->set_value(2); s->set_suffix("st");
        s->set_bounds({30, 17, 140, 36}); c.add_child(std::move(s));
    });
    add("Pan", 260, 60, [](Cell& c) {
        auto p = std::make_unique<PanControl>(); p->set_value(-0.4f);
        p->set_bounds({30, 21, 200, 18}); c.add_child(std::move(p));
    });
    add("Tab", 300, 90, [](Cell& c) {
        auto t = std::make_unique<TabPanel>();
        // Underline treatment — the Ink & Signal navigation tabs (Figma
        // 227:1763): no filled strip, teal underline + full-width divider.
        t->set_tab_bar_style(TabPanel::TabBarStyle::underline);
        t->add_tab("Amp", std::make_unique<View>());
        t->add_tab("Filter", std::make_unique<View>());
        t->add_tab("FX", std::make_unique<View>());
        t->set_active_tab(0);
        t->set_bounds({20, 16, 260, 58}); c.add_child(std::move(t));
    });
    add("Segmented", 300, 70, [](Cell& c) {
        auto s = std::make_unique<SegmentedControl>();
        s->set_segments({"Amp", "EQ", "Comp", "Reverb"});
        s->set_selected(0);
        s->set_bounds({20, 20, 260, 30}); c.add_child(std::move(s));
    });
    add("Toolbar", 300, 70, [](Cell& c) {
        auto tb = std::make_unique<Toolbar>();
        tb->add_button("prev", "\xe2\x8f\xae", [] {});  // ⏮
        tb->add_button("play", "\xe2\x96\xb6", [] {});  // ▶
        tb->add_button("rec",  "\xe2\x97\x8f", [] {});  // ●
        tb->add_toggle("loop", "\xe2\x86\xba", [](bool) {});  // ↺
        tb->set_toggled("loop", true);                  // active (teal) — Figma
        tb->add_separator();
        tb->add_custom("bpm", [] {
            auto l = std::make_unique<Label>("120 BPM");
            l->set_text_align(LabelAlign::center);
            l->flex().preferred_width = 70.0f;  // wide readout, not a square icon
            return l;
        }());
        tb->set_bounds({16, 18, 268, 36}); c.add_child(std::move(tb));
    });
    add("Breadcrumb", 300, 56, [](Cell& c) {
        auto bc = std::make_unique<Breadcrumb>();
        bc->set_show_background(false);                  // flat — Figma 227:1830
        bc->set_items({{"Library", {}}, {"Reverb", {}}, {"Halls", {}}});
        bc->set_bounds({16, 14, 268, 24}); c.add_child(std::move(bc));
    });
    add("Sidebar", 220, 200, [](Cell& c) {
        // The Figma "sidebar" is a labeled vertical nav list with a teal-tinted
        // selected row — rendered here with ListBox's opt-in accent selection +
        // leading icons (Pulp's Sidebar class is a slide container, not a list).
        auto nav = std::make_unique<ListBox>();
        nav->set_items({"Oscillators", "Filter", "Envelopes", "Effects", "Macros"});
        nav->set_icons({"\xe2\x99\xaa", "\xe2\x96\xa4", "\xe2\x9a\x99",
                        "\xe2\x98\xb0", "\xe2\x97\x88"});
        nav->set_selection_style(ListBox::SelectionStyle::accent);
        nav->set_row_height(32.0f);
        nav->set_selected(1);
        nav->set_bounds({16, 16, 188, 168}); c.add_child(std::move(nav));
    });
    add("Tree", 260, 200, [](Cell& c) {
        auto tree = std::make_unique<TreeView>();
        tree->set_selection_style(TreeView::SelectionStyle::accent);
        // Folders are signalled by the disclosure chevron + indentation;
        // leaves (presets) carry a ♪ icon. (Geometric "folder" glyphs fall
        // back to a shapes font whose descent misaligns at small row heights,
        // so the chevron is the cleaner folder cue here.)
        auto& root = tree->root();
        auto& factory = root.add_child("Factory");
        factory.expanded = true;
        auto& reverb = factory.add_child("Reverb");
        reverb.expanded = true;
        reverb.add_child("Concert Hall").icon = "\xe2\x99\xaa";
        reverb.add_child("Velvet Plate").icon = "\xe2\x99\xaa";
        auto& delay = factory.add_child("Delay");     // collapsed folder
        delay.add_child("Tape Echo").icon = "\xe2\x99\xaa";
        auto& user = root.add_child("User");          // collapsed folder
        user.add_child("My Plate").icon = "\xe2\x99\xaa";
        tree->set_selected_node(&reverb);
        tree->set_bounds({16, 14, 228, 172}); c.add_child(std::move(tree));
    });
    add("Spectrum", 300, 140, [](Cell& c) {
        auto s = std::make_unique<SpectrumView>(); s->set_spectrum(demo_spectrum());
        s->set_bounds({20, 18, 260, 104}); c.add_child(std::move(s));
    });
    add("Waveform", 300, 140, [](Cell& c) {
        auto w = std::make_unique<WaveformView>(); w->set_data(demo_wave());
        w->set_bounds({20, 18, 260, 104}); c.add_child(std::move(w));
    });
    add("XYPad", 180, 150, [](Cell& c) {
        auto p = std::make_unique<XYPad>();
        p->set_bounds({30, 18, 120, 114}); c.add_child(std::move(p));
    });
    add("InlineBanner", 420, 80, [](Cell& c) {
        auto b = std::make_unique<InlineBanner>(); b->set_tone(Tone::success);
        b->set_label("Build succeeded."); b->set_message("VST3 · AU · CLAP signed in 4.8s.");
        b->set_bounds({20, 17, 380, 46}); c.add_child(std::move(b));
    });
    add("Toast", 420, 100, [](Cell& c) {
        auto t = std::make_unique<Toast>(); t->set_title("Preset saved");
        t->set_subtitle("Velvet Plate → User library"); t->set_action("Undo");
        t->set_bounds({20, 18, 380, 64}); c.add_child(std::move(t));
    });
    add("EmptyState", 420, 100, [](Cell& c) {
        auto es = std::make_unique<EmptyState>(); es->set_message("No presets yet —"); es->set_action("create one");
        es->set_bounds({20, 18, 380, 64}); c.add_child(std::move(es));
    });
    add("Input", 240, 70, [](Cell& c) {
        auto ed = std::make_unique<TextEditor>(); ed->set_text("Cutoff");
        ed->set_bounds({20, 19, 200, 32}); c.add_child(std::move(ed));
    });
    // The Musical Typing Keyboard ships TWO mode frames; capture each so the
    // contact sheet shows both the typing (732×266) and piano (732×176) faces
    // the 🎹/⌨ toggle swaps between. A small held chord exercises the lit state.
    add("MusicalTyping-Typing", 732, 266, [](Cell& c) {
        auto kb = std::make_unique<MusicalTypingKeyboard>();
        kb->set_mode(MusicalTypingKeyboard::Mode::typing);
        const int held[] = {48, 51, 55};   // C2 D#2 G2 typed
        kb->set_active_notes(held);
        kb->set_bounds({0, 0, 732, 266}); c.add_child(std::move(kb));
    });
    add("MusicalTyping-Piano", 732, 176, [](Cell& c) {
        auto kb = std::make_unique<MusicalTypingKeyboard>();
        kb->set_mode(MusicalTypingKeyboard::Mode::piano);
        const int held[] = {60, 64, 67};   // C4 E4 G4 chord
        kb->set_active_notes(held);
        kb->set_bounds({0, 0, 732, 176}); c.add_child(std::move(kb));
    });
    return e;
}

}  // namespace

int main(int argc, char** argv) {
    std::string out = "shots";
    auto backend = ScreenshotBackend::default_backend;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!std::strcmp(argv[i], "--backend") && i + 1 < argc) {
            std::string b = argv[++i];
            if (b == "skia") backend = ScreenshotBackend::skia;
            else if (b == "coregraphics") backend = ScreenshotBackend::coregraphics;
        }
    }

    // Create the output directory up front. Without this, render_to_file's
    // ofstream silently fails to open and every cell reports "FAILED" — which
    // reads as a render/Skia failure when it's just a missing directory.
    std::error_code ec;
    std::filesystem::create_directories(out, ec);
    if (ec) {
        std::fprintf(stderr, "could not create output dir '%s': %s\n",
                     out.c_str(), ec.message().c_str());
        return 1;
    }

    bool ok = true;
    for (const auto& entry : entries()) {
        for (bool dark : {false, true}) {
            auto cell = std::make_unique<Cell>();
            cell->set_theme(ink_signal_theme(dark));
            cell->set_bounds({0, 0, entry.w, entry.h});
            entry.build(*cell);
            const auto W = static_cast<uint32_t>(entry.w);
            const auto H = static_cast<uint32_t>(entry.h);
            const std::string path = out + "/" + entry.name + (dark ? "-dark.png" : "-light.png");
            const bool wrote = render_to_file(*cell, W, H, path, 2.0f,
                                              backend);
            std::printf("%s %s\n", wrote ? "wrote" : "FAILED", path.c_str());
            ok &= wrote;
        }
    }
    return ok ? 0 : 1;
}
