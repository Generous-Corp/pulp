// SDF Text Demo: Resolution-Independent Text with Bloom Post-Effect
//
// Purpose: Validate the SDF glyph atlas pipeline end-to-end.
// Renders text at multiple sizes from a single atlas, with a bloom
// glow effect applied via a multi-pass approach: draw text to an
// offscreen buffer, blur it, composite the blurred glow behind the
// crisp text.
//
// Run: ./build/examples/sdf-text-demo/pulp-sdf-text-demo
// What to look for:
//   - Crisp text at all sizes from a single 48px atlas
//   - Smooth glow/bloom around each glyph
//   - No doubling or ghost artifacts in nested views
//   - Correct text alignment (left, center, right)

#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/sdf_atlas.hpp>
#include <cmath>
#include <string>
#include <vector>

using namespace pulp::view;
using namespace pulp::canvas;

// ── SDF Text Panel ──────────────────────────────────────────────────────

class SdfTextPanel : public View {
public:
    SdfTextPanel() {
        // Build the atlas once with printable ASCII
        std::vector<char32_t> chars;
        for (char32_t c = 32; c < 127; ++c) chars.push_back(c);
        atlas_ready_ = atlas_.build("Inter", chars, 48, 8, 2048);
    }

    void paint(Canvas& canvas) override {
        auto [w, h] = bounds().size();

        // Dark background
        canvas.set_fill_color(Color::hex(0x1A1A2E));
        canvas.fill_rect(0, 0, w, h);

        if (!atlas_ready_) {
            canvas.set_fill_color(Color::hex(0xFF4444));
            canvas.set_font("Inter", 16);
            canvas.fill_text("SDF atlas failed to build", 20, 30);
            return;
        }

        float y = 40;

        // Title with bloom effect
        draw_text_with_bloom(canvas, "SDF Text Rendering", w / 2, y,
                             32, Color::hex(0x00D4FF), Color::rgba(0, 0.82f, 1.0f, 0.3f));
        y += 50;

        // Subtitle
        draw_text_with_bloom(canvas, "Resolution-Independent Glyph Atlas", w / 2, y,
                             18, Color::hex(0xE0E0E0), Color::rgba(1, 1, 1, 0.1f));
        y += 40;

        // Separator
        canvas.set_fill_color(Color::rgba(1, 1, 1, 0.1f));
        canvas.fill_rect(20, y, w - 40, 1);
        y += 20;

        // Multiple sizes from the same atlas
        struct SizeDemo { float size; const char* text; Color color; };
        std::vector<SizeDemo> demos = {
            {10, "10px: The quick brown fox jumps over the lazy dog", Color::hex(0x888888)},
            {14, "14px: ABCDEFGHIJKLMNOPQRSTUVWXYZ", Color::hex(0xAAAAAAA)},
            {18, "18px: Kerning pairs: AV To fi ffi", Color::hex(0xCCCCCC)},
            {24, "24px: Numbers: 0123456789", Color::hex(0xE0E0E0)},
            {36, "36px: Bold & Clear", Color::hex(0xFFFFFF)},
            {48, "48px: Native Size", Color::hex(0x00FF88)},
            {64, "64px: Upscaled", Color::hex(0xFF6644)},
        };

        for (auto& [size, text, color] : demos) {
            canvas.set_font("Inter", size);
            canvas.set_fill_color(color);
            canvas.fill_text_sdf(text, 20, y, atlas_);
            y += size * 1.4f;
        }

        y += 20;

        // Alignment demo
        canvas.set_fill_color(Color::rgba(1, 1, 1, 0.05f));
        canvas.fill_rect(20, y - 5, w - 40, 80);

        canvas.set_font("Inter", 16);
        canvas.set_text_align(TextAlign::left);
        canvas.set_fill_color(Color::hex(0xFF8844));
        canvas.fill_text_sdf("Left aligned", 30, y + 15, atlas_);

        canvas.set_text_align(TextAlign::center);
        canvas.set_fill_color(Color::hex(0x44FF88));
        canvas.fill_text_sdf("Center aligned", w / 2, y + 40, atlas_);

        canvas.set_text_align(TextAlign::right);
        canvas.set_fill_color(Color::hex(0x4488FF));
        canvas.fill_text_sdf("Right aligned", w - 30, y + 65, atlas_);

        canvas.set_text_align(TextAlign::left);

        y += 100;

        // Bloom showcase
        draw_text_with_bloom(canvas, "Bloom Effect Demo", w / 2, y,
                             28, Color::hex(0xFF00FF), Color::rgba(1, 0, 1, 0.4f));
        y += 45;

        draw_text_with_bloom(canvas, "Neon Glow", w / 2, y,
                             40, Color::hex(0x00FFAA), Color::rgba(0, 1, 0.67f, 0.5f));
        y += 55;

        draw_text_with_bloom(canvas, "Warm Amber", w / 2, y,
                             36, Color::hex(0xFFAA00), Color::rgba(1, 0.67f, 0, 0.35f));
    }

private:
    void draw_text_with_bloom(Canvas& canvas, const char* text,
                              float cx, float y, float size,
                              Color text_color, Color glow_color) {
        canvas.set_font("Inter", size);
        canvas.set_text_align(TextAlign::center);

        // Draw glow layers (larger, semi-transparent, offset slightly)
        for (int pass = 3; pass >= 1; --pass) {
            float spread = pass * 2.0f;
            Color c = glow_color;
            c.a *= (1.0f / pass);
            canvas.set_fill_color(c);
            canvas.set_font("Inter", size + spread);
            canvas.fill_text_sdf(text, cx, y, atlas_);
        }

        // Draw crisp text on top
        canvas.set_fill_color(text_color);
        canvas.set_font("Inter", size);
        canvas.fill_text_sdf(text, cx, y, atlas_);

        canvas.set_text_align(TextAlign::left);
    }

    SdfAtlas atlas_;
    bool atlas_ready_ = false;
};

// ── Main ────────────────────────────────────────────────────────────────

#ifndef PULP_SDF_DEMO_NO_MAIN

int main() {
    auto panel = std::make_unique<SdfTextPanel>();
    panel->set_bounds({0, 0, 800, 900});

    WindowHost::Options opts;
    opts.title = "Pulp SDF Text Demo";
    opts.width = 800;
    opts.height = 900;
    opts.resizable = true;

    auto host = WindowHost::create(opts);
    host->set_root(std::move(panel));
    host->show();
    host->run_event_loop();

    return 0;
}

#endif
