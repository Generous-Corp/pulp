// GPU Validation Demo: Modulation Matrix with Animated Patcher Cables
//
// Purpose: Prove the GPU front end (acquire → wrap → render → submit → present)
// works correctly with continuous animation, vector drawing, and interaction.
//
// Run: ./build/examples/gpu-demo/pulp-gpu-demo
// What to look for:
//   - Smooth cable animation at display refresh rate
//   - No flicker, stale frames, or lost-present behavior
//   - Correct resize/DPI handling
//   - Stable performance with 20+ animated cables

#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/canvas/canvas.hpp>
#include <cmath>
#include <vector>
#include <string>
#include <chrono>
#include <random>

using namespace pulp::view;
using namespace pulp::canvas;

// ── Modulation connection model ──────────────────────────────────────────

struct ModConnection {
    int source_idx;
    int dest_idx;
    float amount;       // -1 to 1
    bool active;
    float phase;        // animation phase
    float hue;          // cable color hue
};

static const std::vector<std::string> sources = {
    "LFO 1", "LFO 2", "Env 1", "Env 2", "Macro 1", "Macro 2",
    "Velocity", "Aftertouch"
};

static const std::vector<std::string> destinations = {
    "Cutoff", "Resonance", "Pan", "Pitch", "Shape", "Mix",
    "Drive", "Feedback"
};

// ── ModMatrixView — the main demo view ───────────────────────────────────

class ModMatrixView : public View {
public:
    ModMatrixView() {
        // Generate random connections
        std::mt19937 rng(42);
        std::uniform_real_distribution<float> amount_dist(-1.0f, 1.0f);
        std::uniform_real_distribution<float> phase_dist(0.0f, 6.28f);

        for (int i = 0; i < static_cast<int>(sources.size()); ++i) {
            for (int j = 0; j < static_cast<int>(destinations.size()); ++j) {
                if (rng() % 3 == 0) { // ~33% connected
                    float hue = static_cast<float>(connections_.size() * 37 % 360);
                    connections_.push_back({
                        i, j,
                        amount_dist(rng),
                        true,
                        phase_dist(rng),
                        hue
                    });
                }
            }
        }

        start_time_ = std::chrono::steady_clock::now();
    }

    void paint(Canvas& c) override {
        auto b = bounds();
        float w = b.width, h = b.height;

        // Elapsed time for animation
        auto now = std::chrono::steady_clock::now();
        float t = std::chrono::duration<float>(now - start_time_).count();

        // Layout — use proportional positioning to handle any window size
        float col_w = w * 0.15f;                    // 15% for each column
        float left_x = w * 0.02f;                   // 2% margin
        float right_x = w - col_w - w * 0.02f;      // right column
        float cable_left = left_x + col_w + w * 0.01f;
        float cable_right = right_x - w * 0.01f;
        float row_h = h * 0.045f;                   // ~4.5% per row
        float top_y = h * 0.08f;                    // 8% top margin

        // Background
        c.set_fill_color(Color::rgba(22, 22, 35));
        c.fill_rect(0, 0, w, h);

        // Title
        c.set_fill_color(Color::rgba(200, 200, 220));
        c.fill_text("GPU Modulation Matrix", w * 0.3f, h * 0.04f);

        // FPS counter
        frame_count_++;
        float fps_elapsed = std::chrono::duration<float>(now - fps_time_).count();
        if (fps_elapsed >= 1.0f) {
            fps_ = static_cast<float>(frame_count_) / fps_elapsed;
            frame_count_ = 0;
            fps_time_ = now;
        }
        char fps_buf[32];
        snprintf(fps_buf, sizeof(fps_buf), "%.0f FPS", fps_);
        c.set_fill_color(Color::rgba(100, 255, 100));
        c.fill_text(fps_buf, w - 80.0f, 30.0f);

        // Cable count
        char cable_buf[32];
        snprintf(cable_buf, sizeof(cable_buf), "%zu cables", connections_.size());
        c.set_fill_color(Color::rgba(180, 180, 200));
        c.fill_text(cable_buf, w - 80.0f, 14.0f);

        // Source column
        c.set_fill_color(Color::rgba(40, 40, 60));
        c.fill_rect(left_x, top_y, col_w, row_h * sources.size() + 10.0f);

        for (size_t i = 0; i < sources.size(); ++i) {
            float y = top_y + 8.0f + i * row_h;
            bool hovered = (hover_source_ == static_cast<int>(i));

            // Source node
            c.set_fill_color(hovered ? Color::rgba(80, 180, 255) : Color::rgba(60, 120, 200));
            c.fill_rect(left_x + 4.0f, y, col_w - 8.0f, row_h - 6.0f);

            // Label
            c.set_fill_color(Color::rgba(220, 220, 240));
            c.fill_text(sources[i], left_x + 10.0f, y + row_h / 2.0f + 4.0f);

            // Socket
            float socket_x = left_x + col_w;
            float socket_y = y + (row_h - 6.0f) / 2.0f;
            c.set_fill_color(Color::rgba(100, 200, 255));
            c.fill_rect(socket_x - 4.0f, socket_y - 4.0f, 8.0f, 8.0f);
        }

        // Destination column
        c.set_fill_color(Color::rgba(40, 40, 60));
        c.fill_rect(right_x, top_y, col_w, row_h * destinations.size() + 10.0f);

        for (size_t i = 0; i < destinations.size(); ++i) {
            float y = top_y + 8.0f + i * row_h;
            bool hovered = (hover_dest_ == static_cast<int>(i));

            c.set_fill_color(hovered ? Color::rgba(255, 140, 80) : Color::rgba(200, 100, 60));
            c.fill_rect(right_x + 4.0f, y, col_w - 8.0f, row_h - 6.0f);

            c.set_fill_color(Color::rgba(220, 220, 240));
            c.fill_text(destinations[i], right_x + 10.0f, y + row_h / 2.0f + 4.0f);

            // Socket
            float socket_x = right_x;
            float socket_y = y + (row_h - 6.0f) / 2.0f;
            c.set_fill_color(Color::rgba(255, 160, 100));
            c.fill_rect(socket_x - 4.0f, socket_y - 4.0f, 8.0f, 8.0f);
        }

        // Draw cables with animation
        for (auto& conn : connections_) {
            if (!conn.active) continue;

            float src_y = top_y + 8.0f + conn.source_idx * row_h + (row_h - 6.0f) / 2.0f;
            float dst_y = top_y + 8.0f + conn.dest_idx * row_h + (row_h - 6.0f) / 2.0f;

            float x1 = cable_left;
            float y1 = src_y;
            float x2 = cable_right;
            float y2 = dst_y;

            // Animated cable using segmented Bézier approximation
            float abs_amount = std::abs(conn.amount);
            float alpha = 0.3f + abs_amount * 0.7f;

            // Cable color from hue
            float r, g, b_val;
            hsv_to_rgb(conn.hue, 0.7f, 0.9f, r, g, b_val);

            bool highlighted = (hover_source_ == conn.source_idx ||
                               hover_dest_ == conn.dest_idx ||
                               hover_cable_ == static_cast<int>(&conn - connections_.data()));

            float thickness = highlighted ? 3.0f : (1.0f + abs_amount * 1.5f);

            // Draw cable as segmented curve
            int segments = 30;
            float cx1 = x1 + (x2 - x1) * 0.4f;
            float cx2 = x2 - (x2 - x1) * 0.4f;

            float prev_x = x1, prev_y = y1;
            for (int s = 1; s <= segments; ++s) {
                float frac = static_cast<float>(s) / segments;

                // Cubic Bézier with horizontal control points (patcher cable style)
                float one_minus = 1.0f - frac;
                float px = one_minus * one_minus * one_minus * x1
                         + 3.0f * one_minus * one_minus * frac * cx1
                         + 3.0f * one_minus * frac * frac * cx2
                         + frac * frac * frac * x2;
                // Y control points stay at source/dest Y — creates horizontal S-curve
                float cy1 = y1;  // control point 1 at source Y
                float cy2 = y2;  // control point 2 at dest Y
                float py = one_minus * one_minus * one_minus * y1
                         + 3.0f * one_minus * one_minus * frac * cy1
                         + 3.0f * one_minus * frac * frac * cy2
                         + frac * frac * frac * y2;

                // Animated pulse traveling along cable
                float pulse = std::sin((frac * 6.28f * 3.0f) - t * 4.0f + conn.phase);
                float pulse_alpha = alpha * (0.6f + 0.4f * std::max(0.0f, pulse));

                uint8_t cr = static_cast<uint8_t>(r * 255.0f * pulse_alpha);
                uint8_t cg = static_cast<uint8_t>(g * 255.0f * pulse_alpha);
                uint8_t cb = static_cast<uint8_t>(b_val * 255.0f * pulse_alpha);

                c.set_stroke_color(Color::rgba(cr, cg, cb, static_cast<uint8_t>(pulse_alpha * 255)));
                c.set_line_width(thickness);
                c.stroke_line(prev_x, prev_y, px, py);

                prev_x = px;
                prev_y = py;
            }
        }

        // Cable amount labels on hover
        if (hover_cable_ >= 0 && hover_cable_ < static_cast<int>(connections_.size())) {
            auto& conn = connections_[hover_cable_];
            float src_y = top_y + 8.0f + conn.source_idx * row_h + (row_h - 6.0f) / 2.0f;
            float dst_y = top_y + 8.0f + conn.dest_idx * row_h + (row_h - 6.0f) / 2.0f;
            float mid_x = (cable_left + cable_right) / 2.0f;
            float mid_y = (src_y + dst_y) / 2.0f;

            char label_buf[64];
            snprintf(label_buf, sizeof(label_buf), "%s → %s: %.0f%%",
                sources[conn.source_idx].c_str(),
                destinations[conn.dest_idx].c_str(),
                conn.amount * 100.0f);

            // Label background
            c.set_fill_color(Color::rgba(20, 20, 40, 220));
            c.fill_rect(mid_x - 80.0f, mid_y - 20.0f, 160.0f, 22.0f);

            // Label text
            c.set_fill_color(Color::rgba(255, 255, 255));
            c.fill_text(label_buf, mid_x - 75.0f, mid_y - 4.0f);
        }

        // Connection matrix (bottom panel)
        float matrix_y = top_y + row_h * std::max(sources.size(), destinations.size()) + 30.0f;
        c.set_fill_color(Color::rgba(30, 30, 50));
        c.fill_rect(20.0f, matrix_y, w - 40.0f, 160.0f);

        c.set_fill_color(Color::rgba(180, 180, 200));
        c.fill_text("Connection Matrix", 30.0f, matrix_y + 18.0f);

        float row_y = matrix_y + 30.0f;
        for (size_t i = 0; i < connections_.size() && i < 8; ++i) {
            auto& conn = connections_[i];
            char buf[64];
            snprintf(buf, sizeof(buf), "%s → %s: %.0f%%",
                sources[conn.source_idx].c_str(),
                destinations[conn.dest_idx].c_str(),
                conn.amount * 100.0f);

            c.set_fill_color(Color::rgba(160, 160, 180));
            c.fill_text(buf, 30.0f, row_y + 14.0f);

            // Amount bar
            float bar_x = 280.0f;
            float bar_w = 100.0f;
            c.set_fill_color(Color::rgba(50, 50, 70));
            c.fill_rect(bar_x, row_y + 2.0f, bar_w, 12.0f);
            float fill_w = std::abs(conn.amount) * bar_w;
            float fill_x = conn.amount >= 0 ? bar_x + bar_w / 2.0f : bar_x + bar_w / 2.0f - fill_w;
            c.set_fill_color(conn.amount >= 0 ? Color::rgba(80, 200, 120) : Color::rgba(200, 80, 80));
            c.fill_rect(fill_x, row_y + 2.0f, fill_w, 12.0f);

            row_y += 16.0f;
        }

    }

    void on_mouse_event(const MouseEvent& e) override {
        auto b = bounds();
        float w = b.width, h = b.height;

        // Use same proportional layout as paint()
        float col_w = w * 0.15f;
        float left_x = w * 0.02f;
        float right_x = w - col_w - w * 0.02f;
        float row_h = h * 0.045f;
        float top_y = h * 0.08f;

        hover_source_ = -1;
        hover_dest_ = -1;
        hover_cable_ = -1;

        // Check source column
        if (e.position.x >= left_x && e.position.x <= left_x + col_w) {
            int idx = static_cast<int>((e.position.y - top_y - 8.0f) / row_h);
            if (idx >= 0 && idx < static_cast<int>(sources.size()))
                hover_source_ = idx;
        }

        // Check dest column
        if (e.position.x >= right_x && e.position.x <= right_x + col_w) {
            int idx = static_cast<int>((e.position.y - top_y - 8.0f) / row_h);
            if (idx >= 0 && idx < static_cast<int>(destinations.size()))
                hover_dest_ = idx;
        }

        // Check cable proximity (hit test within 6px of midpoint)
        float cable_left = left_x + col_w + w * 0.01f;
        float cable_right = right_x - w * 0.01f;
        float cable_mid_x = (cable_left + cable_right) / 2.0f;
        for (size_t i = 0; i < connections_.size(); ++i) {
            auto& conn = connections_[i];
            if (!conn.active) continue;
            float src_y = top_y + 8.0f + conn.source_idx * row_h + (row_h - 6.0f) / 2.0f;
            float dst_y = top_y + 8.0f + conn.dest_idx * row_h + (row_h - 6.0f) / 2.0f;
            float mid_y = (src_y + dst_y) / 2.0f;
            float dx = e.position.x - cable_mid_x;
            float dy = e.position.y - mid_y;
            if (dx * dx + dy * dy < 36.0f * 36.0f) {
                hover_cable_ = static_cast<int>(i);
                break;
            }
        }

        // Toggle cable on click
        if (e.is_down && hover_cable_ >= 0) {
            connections_[hover_cable_].active = !connections_[hover_cable_].active;
        }
    }

private:
    std::vector<ModConnection> connections_;
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point fps_time_ = std::chrono::steady_clock::now();
    int frame_count_ = 0;
    float fps_ = 0.0f;
    int hover_source_ = -1;
    int hover_dest_ = -1;
    int hover_cable_ = -1;

    static void hsv_to_rgb(float h, float s, float v, float& r, float& g, float& b) {
        float c = v * s;
        float x = c * (1.0f - std::abs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
        float m = v - c;
        if (h < 60)       { r = c; g = x; b = 0; }
        else if (h < 120) { r = x; g = c; b = 0; }
        else if (h < 180) { r = 0; g = c; b = x; }
        else if (h < 240) { r = 0; g = x; b = c; }
        else if (h < 300) { r = x; g = 0; b = c; }
        else              { r = c; g = 0; b = x; }
        r += m; g += m; b += m;
    }
};

// ── Main ─────────────────────────────────────────────────────────────────

int main() {
    ModMatrixView root;
    root.set_theme(Theme::dark());

    WindowOptions opts;
    opts.title = "Pulp GPU Demo — Modulation Matrix";
    opts.width = 800;
    opts.height = 550;
    opts.resizable = true;
    opts.use_gpu = true;  // Use the GPU render path

    auto host = WindowHost::create(root, opts);
    host->run_event_loop();

    return 0;
}
