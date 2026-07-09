#include <pulp/view/text_editor.hpp>
#include "text_edit_model.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>

namespace pulp::view {

namespace {

bool row_owns_caret(int caret, int start, int end, bool has_next, int next_start) {
    if (caret < start || caret > end) return false;
    if (caret < end) return true;
    if (start == end) return true;
    return !has_next || next_start != end;
}

} // namespace

// ── Painting ─────────────────────────────────────────────────────────────

void TextEditor::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();

    auto bg_color = has_background_color()
        ? background_color()
        : (has_focus()
            ? resolve_color("text_editor_focus_bg",
                            resolve_color("bg.elevated",
                                          resolve_color("bg.surface", canvas::Color::hex(0x2a2a4a))))
            : resolve_color("text_editor_bg",
                            resolve_color("bg.surface", canvas::Color::hex(0x1a1a2e))));
    float radius = corner_radius() > 0.0f ? corner_radius() : 6.0f;
    float max_radius = std::max(0.0f, std::min(b.width, b.height) * 0.5f - 0.5f);
    radius = std::min(radius, max_radius);

    // Border — use explicit per-view styling when present, otherwise theme defaults.
    auto stroke = has_border()
        ? border_color()
        : (has_focus()
            ? resolve_color("accent.primary", canvas::Color::rgba8(140, 120, 255, 255))
            : resolve_color("control.border",
                            resolve_color("border", canvas::Color::hex(0x3a3a5a))));
    float stroke_width = has_border() ? border_width() : (has_focus() ? 2.0f : 1.0f);
    if (stroke_width > 0.0f) {
        canvas.set_fill_color(stroke);
        canvas.fill_rounded_rect(b.x, b.y, b.width, b.height, radius);

        float inset = std::max(0.0f, stroke_width);
        float inner_w = std::max(0.0f, b.width - inset * 2.0f);
        float inner_h = std::max(0.0f, b.height - inset * 2.0f);
        float inner_max_radius = std::max(0.0f, std::min(inner_w, inner_h) * 0.5f - 0.5f);
        float inner_radius = std::min(inner_max_radius, std::max(0.0f, radius - inset - 0.5f));
        canvas.set_fill_color(bg_color);
        canvas.fill_rounded_rect(b.x + inset, b.y + inset, inner_w, inner_h, inner_radius);
    } else {
        canvas.set_fill_color(bg_color);
        canvas.fill_rounded_rect(b.x, b.y, b.width, b.height, radius);
    }

    canvas.set_font("Inter", font_size_);
    canvas.set_text_align(canvas::TextAlign::left);

    // Display text
    std::string display = text_;
    if (password_mode) {
        display = std::string(text_.size(), password_char);
    }

    if (multi_line) {
        canvas.set_font("Inter", font_size_);
        canvas.set_text_align(canvas::TextAlign::left);
        const float inner_x = b.x + 6.0f;
        const float inner_y = b.y + 4.0f;
        const float inner_w = std::max(20.0f, b.width - 12.0f);
        const float line_h = font_size_ * 1.35f;

        struct WrappedLine {
            std::string text;
            int start = 0;
            int end = 0;
            std::vector<int> byte_offsets;
        };

        std::vector<WrappedLine> lines;
        int current_start = 0;
        std::string current;
        std::vector<int> current_offsets{0};

        auto flush_line = [&](int end_index) {
            lines.push_back({current, current_start, end_index, current_offsets});
            current.clear();
            current_start = end_index;
            current_offsets.clear();
            current_offsets.push_back(current_start);
        };

        for (int i = 0; i < static_cast<int>(display.size());) {
            const int next = text_edit::next_cluster(display, i);
            char c = display[static_cast<size_t>(i)];
            if (c == '\n') {
                flush_line(i);
                current_start = next;
                current_offsets.clear();
                current_offsets.push_back(current_start);
                i = next;
                continue;
            }

            std::string segment = display.substr(static_cast<std::size_t>(i),
                                                 static_cast<std::size_t>(next - i));
            std::string candidate = current + segment;
            if (!current.empty() && canvas.measure_text(candidate) > inner_w) {
                flush_line(i);
                current_start = i;
                current_offsets.clear();
                current_offsets.push_back(current_start);
            }
            current += segment;
            current_offsets.push_back(next);
            i = next;
        }

        flush_line(static_cast<int>(display.size()));

        if (lines.empty()) lines.push_back({"", 0, 0, {0}});

        int caret_line = 0;
        int caret_column = 0;
        for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
            auto& line = lines[static_cast<size_t>(i)];
            const bool has_next = i + 1 < static_cast<int>(lines.size());
            const int next_start = has_next
                ? lines[static_cast<size_t>(i + 1)].start
                : line.end;
            if (row_owns_caret(caret_position_, line.start, line.end, has_next, next_start)) {
                caret_line = i;
                caret_column = text_edit::cluster_index_for_position(line.byte_offsets, caret_position_);
                break;
            }
        }

        float visible_h = std::max(line_h, b.height - 8.0f);
        float total_h = std::max(visible_h, static_cast<float>(lines.size()) * line_h);
        float caret_top = caret_line * line_h;
        float caret_bottom = caret_top + line_h;
        if (caret_bottom - scroll_offset_ > visible_h) {
            scroll_offset_ = caret_bottom - visible_h;
        } else if (caret_top - scroll_offset_ < 0) {
            scroll_offset_ = caret_top;
        }
        scroll_offset_ = std::clamp(scroll_offset_, 0.0f, std::max(0.0f, total_h - visible_h));

        // Cache-keyed snapshot: only rebuild the per-character offset
        // table when an input that affects layout changes (text, font,
        // bounds, scroll, mode). On a quiet 60Hz frame this collapses
        // to a key compare — `measure_text` is multi-millisecond on
        // the SkParagraph path and the legacy code rebuilt it every
        // paint, blowing the audio-UI thread budget.
        LayoutCacheKey key{
            std::hash<std::string>{}(display),
            font_size_,
            b.width,
            b.height,
            scroll_offset_,
            /*multi_line=*/true,
            password_mode,
            /*placeholder_visible=*/display.empty() && !placeholder.empty() && !has_focus(),
        };
        if (!(last_layout_key_ == key) || last_layout_.lines.size() != lines.size()) {
            last_layout_.multi_line = true;
            last_layout_.lines.clear();
            last_layout_.lines.reserve(lines.size());
            last_layout_.fallback_char_w = canvas.measure_text("M");
            for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
                const auto& src = lines[static_cast<size_t>(i)];
                LayoutSnapshot::Line dst;
                dst.start = src.start;
                dst.end = src.end;
                dst.top_y = inner_y + i * line_h - scroll_offset_;
                dst.baseline_y = dst.top_y + font_size_;
                dst.inner_x = inner_x;
                dst.line_height = line_h;
                dst.byte_offsets = src.byte_offsets.empty()
                    ? std::vector<int>{src.start}
                    : src.byte_offsets;
                dst.x_offsets.resize(dst.byte_offsets.size());
                for (size_t j = 0; j < dst.byte_offsets.size(); ++j) {
                    const int local_byte = std::clamp(dst.byte_offsets[j] - src.start,
                                                      0, static_cast<int>(src.text.size()));
                    dst.x_offsets[j] = canvas.text_x_for_byte(src.text,
                        static_cast<std::size_t>(local_byte));
                }
                last_layout_.lines.push_back(std::move(dst));
            }
            last_layout_key_ = key;
        } else {
            // Cache hit — refresh only the y/x baselines that depend on
            // scroll_offset_ + bounds origin (cheap; no measure calls).
            for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
                auto& dst = last_layout_.lines[static_cast<size_t>(i)];
                dst.top_y = inner_y + i * line_h - scroll_offset_;
                dst.baseline_y = dst.top_y + font_size_;
                dst.inner_x = inner_x;
            }
        }

        auto text_primary = resolve_color("text.primary", canvas::Color::hex(0xe0e0e0));
        auto text_secondary = resolve_color("text.secondary", canvas::Color::hex(0x808090));
        auto selection_fill = resolve_color("accent.primary", canvas::Color::rgba8(65, 105, 225, 255));
        selection_fill.a = 168;

        canvas.save();
        canvas.clip_rect(b.x + 2.0f, b.y + 2.0f, std::max(0.0f, b.width - 4.0f),
                         std::max(0.0f, b.height - 4.0f));

        if (has_selection()) {
            const int sel_start = std::min(selection_start_, selection_end_);
            const int sel_end = std::max(selection_start_, selection_end_);
            canvas.set_fill_color(selection_fill);
            for (const auto& row : last_layout_.lines) {
                const int start = std::max(sel_start, row.start);
                const int end = std::min(sel_end, row.end);
                if (end <= start || row.x_offsets.empty()) continue;
                const int start_idx = text_edit::cluster_index_for_position(row.byte_offsets, start);
                const int end_idx = text_edit::cluster_index_for_position(row.byte_offsets, end);
                const float sx = row.inner_x + row.x_offsets[static_cast<size_t>(start_idx)];
                const float ex = row.inner_x + row.x_offsets[static_cast<size_t>(end_idx)];
                if (ex > sx)
                    canvas.fill_rect(sx, row.top_y, ex - sx, row.line_height);
            }
        }

        if (display.empty() && !placeholder.empty() && !has_focus()) {
            canvas.set_fill_color(text_secondary);
            canvas.fill_text(placeholder, inner_x, inner_y + font_size_);
        } else {
            for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
                float baseline_y = inner_y + font_size_ + i * line_h - scroll_offset_;
                if (baseline_y < b.y - line_h || baseline_y > b.y + b.height + line_h) continue;
                canvas.set_fill_color(text_primary);
                canvas.fill_text(lines[static_cast<size_t>(i)].text, inner_x, baseline_y);
            }
        }
        canvas.restore();

        if (should_paint_caret()) {
            // Read caret x from the cached snapshot instead of
            // re-measuring a prefix substring every paint.
            const auto& snap_line =
                last_layout_.lines[static_cast<size_t>(caret_line)];
            const size_t last_col =
                snap_line.x_offsets.empty() ? 0 : snap_line.x_offsets.size() - 1;
            size_t col = std::clamp<size_t>(caret_column, 0, last_col);
            float caret_x = snap_line.inner_x + (snap_line.x_offsets.empty()
                ? 0.f : snap_line.x_offsets[col]);
            float caret_y = inner_y + caret_line * line_h - scroll_offset_;

            CaretMetrics m;
            m.x = caret_x;
            m.cell_top = caret_y;
            m.cell_height = line_h - 2.0f;
            m.baseline = inner_y + font_size_ + caret_line * line_h - scroll_offset_;
            // The next glyph on this row sizes the underline/block. At the row's
            // end there is none, so fall back to a nominal advance.
            m.advance = col < last_col ? snap_line.x_offsets[col + 1] - snap_line.x_offsets[col]
                                       : 0.0f;
            m.nominal_advance = canvas.measure_text("0");
            m.stroke = 1.5f;
            paint_caret_over_text(canvas, caret_style_, m,
                                  resolve_color("text.primary", canvas::Color::hex(0xe0e0e0)),
                                  bg_color, lines[static_cast<size_t>(caret_line)].text,
                                  snap_line.inner_x);
        }
        return;
    }

    const float text_pad_x = std::max(9.0f, border_width() + 7.0f);
    const float left_pad = std::max(text_pad_x, content_inset_left_);
    const float text_inner_x = b.x + left_pad;
    const float text_inner_w = std::max(0.0f, b.width - left_pad - text_pad_x);
    const auto metrics = canvas.measure_text_full(display.empty() ? std::string("Ag") : display);
    const float text_y = b.y + std::max(0.0f, (b.height - metrics.line_height) * 0.5f) + metrics.ascent;

    // Cache-keyed single-line snapshot. Skips the per-char measure
    // rebuild on quiet frames. See multi-line block above for the
    // identical pattern + rationale.
    LayoutCacheKey key{
        std::hash<std::string>{}(display),
        font_size_,
        b.width,
        b.height,
        scroll_offset_,
        /*multi_line=*/false,
        password_mode,
        /*placeholder_visible=*/display.empty() && !placeholder.empty() && !has_focus(),
    };
    // Store `inner_x` in scrolled (visual) coordinates so snapshot
    // readers (`char_index_at_point`, `caret_rect()`,
    // `firstRectForCharacterRange`) match the painted text — which
    // draws at `text_inner_x - scroll_offset_`. Cache key includes
    // `scroll_offset_`, so the snapshot rebuilds when scroll changes.
    const float visual_inner_x = text_inner_x - scroll_offset_;
    if (!(last_layout_key_ == key) || last_layout_.lines.size() != 1) {
        last_layout_.multi_line = false;
        last_layout_.fallback_char_w = canvas.measure_text("M");
        last_layout_.lines.clear();
        LayoutSnapshot::Line line_snap;
        line_snap.start = 0;
        line_snap.end = static_cast<int>(display.size());
        line_snap.top_y = b.y + std::max(0.0f, (b.height - metrics.line_height) * 0.5f);
        line_snap.baseline_y = text_y;
        line_snap.line_height = metrics.line_height > 0.f ? metrics.line_height : font_size_;
        line_snap.inner_x = visual_inner_x;
        // Caret/selection x from the SHAPED run (text_x_for_byte), not a sum of
        // isolated glyph widths — the per-char sum drifts on kerned/spaced runs,
        // which showed as extra space after the last char + padding around a
        // selection. text_x_for_byte queries the same shaping the painter draws.
        line_snap.byte_offsets = text_edit::cluster_boundaries(display);
        line_snap.x_offsets.resize(line_snap.byte_offsets.size());
        for (size_t j = 0; j < line_snap.byte_offsets.size(); ++j)
            line_snap.x_offsets[j] = canvas.text_x_for_byte(
                display, static_cast<std::size_t>(line_snap.byte_offsets[j]));
        last_layout_.lines.push_back(std::move(line_snap));
        last_layout_key_ = key;
    } else if (!last_layout_.lines.empty()) {
        auto& dst = last_layout_.lines.front();
        dst.top_y = b.y + std::max(0.0f, (b.height - metrics.line_height) * 0.5f);
        dst.baseline_y = text_y;
        dst.inner_x = visual_inner_x;
    }
    const float total_text_w = last_layout_.lines.front().x_offsets.back();
    auto x_for_position = [&](int position) -> float {
        const auto& line = last_layout_.lines.front();
        if (line.x_offsets.empty()) return 0.0f;
        const int idx = text_edit::cluster_index_for_position(line.byte_offsets, position);
        return line.x_offsets[static_cast<std::size_t>(idx)];
    };
    const float caret_w = x_for_position(caret_position_);

    if (has_focus() || has_selection()) {
        if (caret_w - scroll_offset_ > text_inner_w) {
            scroll_offset_ = caret_w - text_inner_w;
        } else if (caret_w - scroll_offset_ < 0.0f) {
            scroll_offset_ = caret_w;
        }
        scroll_offset_ = std::clamp(scroll_offset_, 0.0f, std::max(0.0f, total_text_w - text_inner_w));
    } else {
        scroll_offset_ = 0.0f;
    }

    float text_x = text_inner_x - scroll_offset_;
    // After the scroll-offset update, re-write the snapshot's
    // `inner_x` so readers (caret_rect, hit-test, IME) match the
    // painted origin even on the first paint where scroll_offset_
    // bumps from 0 to a non-zero value AFTER the snapshot was built.
    if (!last_layout_.lines.empty()) {
        last_layout_.lines.front().inner_x = text_x;
    }
    auto text_primary = resolve_color("text.primary", canvas::Color::hex(0xe0e0e0));
    auto text_secondary = resolve_color("text.secondary", canvas::Color::hex(0x808090));
    auto selection_fill = resolve_color("accent.primary", canvas::Color::rgba8(65, 105, 225, 255));
    selection_fill.a = 168;
    auto selected_text_color = resolve_color("bg.primary", bg_color);

    canvas.save();
    canvas.clip_rect(text_inner_x - 2.0f, b.y + 2.0f, text_inner_w + 4.0f, std::max(0.0f, b.height - 4.0f));

    // Reuse the cached single-line snapshot for all per-glyph x lookups
    // (selection start/width, before/selected/after text origins). The
    // legacy code called `canvas.measure_text(substr(...))` four times
    // per paint, each going through SkParagraph build+layout.
    const auto& xoff = last_layout_.lines.front().x_offsets;
    auto x_at = [&](int idx) -> float {
        if (xoff.empty()) return 0.0f;
        const int i = text_edit::cluster_index_for_position(last_layout_.lines.front().byte_offsets, idx);
        return xoff[static_cast<std::size_t>(i)];
    };

    // Selection rect + selected-glyph x-range, from the SHAPED full-string
    // offsets (the same the painter draws), so the highlight aligns exactly.
    float sel_x = 0.0f, sel_w = 0.0f;
    if (has_selection()) {
        const int start = std::min(selection_start_, selection_end_);
        const int end = std::max(selection_start_, selection_end_);
        sel_x = text_x + x_at(start);
        sel_w = x_at(end) - x_at(start);
        canvas.set_fill_color(selection_fill);
        canvas.fill_rect(sel_x, b.y + 2, sel_w, b.height - 4);
    }

    if (display.empty() && !placeholder.empty() && !has_focus()) {
        canvas.set_fill_color(text_secondary);
        canvas.fill_text(placeholder, text_x, text_y);
    } else if (has_selection() && sel_w > 0.0f) {
        // Paint the WHOLE string as ONE shaped run, re-colored per region by
        // clipping. The three clip rects tile the editor width edge-to-edge and
        // are disjoint, so every pixel is painted exactly once and a glyph never
        // moves just because part of it became selected — a glyph straddling the
        // selection boundary is simply split between the two colors at the same x.
        //
        // The previous painter re-shaped before/selected/after as three separate
        // runs positioned by full-string offsets; a piece shaped in isolation
        // lost its in-context kerning/side-bearing, so selected glyphs drifted —
        // the "gap between the letters" a user sees dragging a selection across a
        // space into a word.
        const float band_l = sel_x;
        const float band_r = sel_x + sel_w;
        auto paint_clipped = [&](float clip_x, float clip_w, canvas::Color color) {
            if (clip_w <= 0.0f) return;
            canvas.save();
            canvas.clip_rect(clip_x, b.y, clip_w, b.height);
            canvas.set_fill_color(color);
            canvas.fill_text(display, text_x, text_y);
            canvas.restore();
        };
        paint_clipped(b.x, band_l - b.x, text_primary);                       // before
        paint_clipped(band_l, band_r - band_l, selected_text_color);          // selected
        paint_clipped(band_r, (b.x + b.width) - band_r, text_primary);        // after
    } else {
        canvas.set_fill_color(text_primary);
        canvas.fill_text(display, text_x, text_y);
    }

    if (should_paint_caret()) {
        // Use the shaped offsets (same as the drawn text), not measure_text of
        // a prefix substring — keeps the caret exactly at the glyph boundary.
        const float caret_x = text_x + x_at(caret_position_);
        const int next = text_edit::move_clusters(text_, caret_position_, 1);

        CaretMetrics m;
        m.x = caret_x;
        m.cell_top = b.y + 4;
        m.cell_height = b.height - 8;
        m.baseline = text_y;
        // Zero at end of text: there is no glyph for the underline/block to cover.
        m.advance = next > caret_position_ ? (text_x + x_at(next)) - caret_x : 0.0f;
        m.nominal_advance = canvas.measure_text("0");
        m.stroke = 1.5f;
        paint_caret_over_text(canvas, caret_style_, m,
                              resolve_color("text.primary", canvas::Color::hex(0xe0e0e0)),
                              bg_color, display, text_x);
    }
    canvas.restore();
}

int TextEditor::char_index_at_x(float x) const {
    float char_w = font_size_ * 0.6f;
    float text_x = std::max(std::max(9.0f, border_width() + 7.0f),
                            content_inset_left_) - scroll_offset_;
    int index = static_cast<int>((x - text_x) / char_w + 0.5f);
    return text_edit::move_clusters(text_, 0, index);
}

int TextEditor::char_index_at_point(float x, float y) const {
    // If paint() has not yet populated a layout snapshot (e.g. the first
    // mouse event arrives before the first frame), fall back to the
    // legacy x-only routine — the y is just lost in that case. Once the
    // first paint runs, the snapshot is authoritative for both single-
    // and multi-line: single-line records one row whose x_offsets are
    // real measured advances, so the row-walk below collapses to that
    // single row and picks the right column.
    if (last_layout_.lines.empty()) {
        return char_index_at_x(x);
    }

    // Pick the row by y: clamp to first/last row when the click is
    // above/below the content, otherwise find the row whose vertical
    // band contains y. This keeps a click in the gap between rows from
    // collapsing to row 0.
    const auto& rows = last_layout_.lines;
    int row_index = 0;
    if (y <= rows.front().top_y) {
        row_index = 0;
    } else if (y >= rows.back().top_y + rows.back().line_height) {
        row_index = static_cast<int>(rows.size()) - 1;
    } else {
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            const auto& row = rows[static_cast<size_t>(i)];
            if (y >= row.top_y && y < row.top_y + row.line_height) {
                row_index = i;
                break;
            }
        }
    }

    const auto& row = rows[static_cast<size_t>(row_index)];
    const float local_x = x - row.inner_x;
    if (row.x_offsets.empty()) return row.start;

    // Nearest-edge hit-test: find the char boundary whose x is closest
    // to the click. Matches the half-glyph convention single-line uses.
    int best = 0;
    float best_dist = std::abs(local_x - row.x_offsets[0]);
    for (size_t j = 1; j < row.x_offsets.size(); ++j) {
        float d = std::abs(local_x - row.x_offsets[j]);
        if (d < best_dist) {
            best_dist = d;
            best = static_cast<int>(j);
        }
    }
    if (!row.byte_offsets.empty())
        return row.byte_offsets[static_cast<std::size_t>(best)];
    return text_edit::clamp_boundary(text_, row.start);
}

Rect TextEditor::caret_rect() const {
    // IME hosts can ask for candidate-window placement immediately after text
    // input invalidates the paint cache, so the fallback still follows the
    // current caret position instead of snapping back to the padding origin.
    // All coordinates returned by `caret_rect()` are in local view space,
    // matching what `paint()` records.
    if (last_layout_.lines.empty()) {
        Rect fallback;
        const auto b = local_bounds();
        const float text_pad_x = std::max(9.0f, border_width() + 7.0f);
        const float left_pad = std::max(text_pad_x, content_inset_left_);
        const float char_w = std::max(1.0f, font_size_ * 0.6f);
        const float line_h = std::max(font_size_, font_size_ * 1.35f);
        if (multi_line) {
            const int start = text_edit::line_start(text_, caret_position_);
            const int column = text_edit::cluster_column_in_line(text_, start, caret_position_);
            int hard_line = 0;
            for (int i = 0; i < start && i < static_cast<int>(text_.size()); ++i) {
                if (text_[static_cast<std::size_t>(i)] == '\n') ++hard_line;
            }
            fallback.x = left_pad + static_cast<float>(column) * char_w;
            fallback.y = 2.0f + static_cast<float>(hard_line) * line_h - scroll_offset_;
            fallback.height = line_h;
        } else {
            const auto boundaries = text_edit::cluster_boundaries(text_);
            const int column = text_edit::cluster_index_for_position(boundaries, caret_position_);
            const float caret_w = static_cast<float>(column) * char_w;
            const float text_inner_w = std::max(0.0f, b.width - left_pad - text_pad_x);
            const auto cluster_total = boundaries.empty() ? 0u : boundaries.size() - 1u;
            const float total_w = static_cast<float>(cluster_total) * char_w;
            float effective_scroll = scroll_offset_;
            if (has_focus() || has_selection()) {
                if (caret_w - effective_scroll > text_inner_w) {
                    effective_scroll = caret_w - text_inner_w;
                } else if (caret_w - effective_scroll < 0.0f) {
                    effective_scroll = caret_w;
                }
                effective_scroll = std::clamp(
                    effective_scroll, 0.0f, std::max(0.0f, total_w - text_inner_w));
            }
            fallback.x = left_pad - effective_scroll + caret_w;
            fallback.y = 2.0f;
            fallback.height = std::max(font_size_, b.height - 4.0f);
        }
        fallback.width = 1.5f;
        return fallback;
    }

    // Find the row that owns the caret. The wrap path puts the caret on
    // the row whose [start,end] band brackets the codepoint; the
    // single-line path always has exactly one row so this still works.
    int row_index = 0;
    for (int i = 0; i < static_cast<int>(last_layout_.lines.size()); ++i) {
        const auto& row = last_layout_.lines[static_cast<size_t>(i)];
        const bool has_next = i + 1 < static_cast<int>(last_layout_.lines.size());
        const int next_start = has_next
            ? last_layout_.lines[static_cast<size_t>(i + 1)].start
            : row.end;
        if (row_owns_caret(caret_position_, row.start, row.end, has_next, next_start)) {
            row_index = i;
            break;
        }
    }

    const auto& row = last_layout_.lines[static_cast<size_t>(row_index)];
    int col = text_edit::cluster_index_for_position(row.byte_offsets, caret_position_);
    float caret_x = row.inner_x + (row.x_offsets.empty() ? 0.f : row.x_offsets[static_cast<size_t>(col)]);

    Rect r;
    r.x = caret_x;
    r.y = row.top_y;
    r.width = 1.5f;
    r.height = row.line_height;
    return r;
}

} // namespace pulp::view
