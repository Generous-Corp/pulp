// CSS Grid layout adapter — parses grid templates/areas and places children
// into a resolved column/row track grid. View::layout_children() dispatches
// here when a container's layout_mode_ is LayoutMode::grid.

#include <pulp/view/view.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace pulp::view {

// ── Grid template parsing ────────────────────────────────────────────────────

std::vector<GridTrack> GridStyle::parse_template(const std::string& tmpl, int depth) {
    std::vector<GridTrack> tracks;
    // A grid-template string is semi-trusted (design-tool exports). Each
    // nested repeat() body recurses one level; cap the depth so a
    // pathologically nested "repeat(2, repeat(2, repeat(2, …)))" cannot
    // overflow the stack. Real templates nest at most a level or two.
    static constexpr int kMaxTemplateDepth = 8;
    if (depth > kMaxTemplateDepth) return tracks;
    std::vector<std::string> tokens;
    std::string token;
    int paren_depth = 0;
    for (const char ch : tmpl) {
        if (std::isspace(static_cast<unsigned char>(ch)) && paren_depth == 0) {
            if (!token.empty()) {
                tokens.push_back(token);
                token.clear();
            }
            continue;
        }
        if (ch == '(') ++paren_depth;
        if (ch == ')' && paren_depth > 0) --paren_depth;
        token.push_back(ch);
    }
    if (!token.empty()) tokens.push_back(token);

    // Parse a numeric prefix without throwing. Non-numeric tokens (e.g. the
    // CSS initial value `none`) are skipped instead of propagating an exception.
    auto try_parse = [](const std::string& s, float& out) -> bool {
        try {
            size_t consumed = 0;
            out = std::stof(s, &consumed);
            return consumed > 0;
        } catch (...) {
            return false;
        }
    };
    auto trim = [](std::string s) {
        auto first = s.find_first_not_of(" \t\r\n");
        auto last = s.find_last_not_of(" \t\r\n");
        if (first == std::string::npos) return std::string{};
        return s.substr(first, last - first + 1);
    };
    for (const auto& raw_token : tokens) {
        token = raw_token;
        // `none` is the CSS initial value for grid-template-* — no explicit
        // tracks. Skip it (and any other non-track keyword) rather than throw.
        if (token == "none") continue;
        if (token.rfind("repeat(", 0) == 0 && token.size() > 8 && token.back() == ')') {
            const auto inner = token.substr(7, token.size() - 8);
            const auto comma = inner.find(',');
            if (comma != std::string::npos) {
                float count_value = 0.0f;
                const auto count_token = trim(inner.substr(0, comma));
                const auto repeated_template = trim(inner.substr(comma + 1));
                if (try_parse(count_token, count_value) && count_value > 0.0f) {
                    const auto repeated_tracks = parse_template(repeated_template, depth + 1);
                    const int count = std::min(64, static_cast<int>(std::floor(count_value)));
                    for (int i = 0; i < count; ++i)
                        tracks.insert(tracks.end(), repeated_tracks.begin(), repeated_tracks.end());
                }
            }
        } else if (token.back() == 'r' && token.size() > 2 && token[token.size()-2] == 'f') {
            // "1fr", "2.5fr"
            float val = 0.0f;
            if (try_parse(token.substr(0, token.size() - 2), val))
                tracks.push_back(GridTrack::fractional(val));
        } else if (token == "auto") {
            tracks.push_back(GridTrack::auto_size());
        } else {
            // "100px" or "100" — treat as fixed pixels; skip unparseable tokens.
            float val = 0.0f;
            if (try_parse(token, val))
                tracks.push_back(GridTrack::fixed_px(val));
        }
    }
    return tracks;
}

std::vector<GridStyle::NamedArea> GridStyle::parse_template_areas(const std::string& css) {
    // Parse CSS grid-template-areas:
    //   "'header header header' 'main side side' 'footer footer footer'"
    // Each single-quoted segment is one row; cells are space-separated.
    // Adjacent cells with the same name (in the same row OR across
    // adjacent rows in the same column) merge into one rectangle.
    // `'.'` is the CSS spec spacer — skipped entirely.
    std::vector<std::vector<std::string>> rows;
    {
        std::string s = css;
        // Trim whitespace.
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(0, 1);
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        // Walk single-quoted runs.
        size_t i = 0;
        while (i < s.size()) {
            if (s[i] == '\'') {
                size_t end = s.find('\'', i + 1);
                if (end == std::string::npos) break;
                std::string row_str = s.substr(i + 1, end - i - 1);
                std::vector<std::string> cells;
                std::istringstream iss(row_str);
                std::string tok;
                while (iss >> tok) cells.push_back(tok);
                rows.push_back(std::move(cells));
                i = end + 1;
            } else {
                ++i;
            }
        }
    }
    if (rows.empty()) return {};

    // Build a name → bounding-rect map. Each cell contributes to the
    // rectangle if it shares the name. CSS spec requires the area to
    // be rectangular; non-rectangular shapes are technically invalid
    // but we accept them as the bounding rect (lenient at the IR layer).
    std::vector<NamedArea> out;
    auto find = [&](const std::string& name) -> NamedArea* {
        for (auto& a : out) if (a.name == name) return &a;
        return nullptr;
    };
    for (size_t r = 0; r < rows.size(); ++r) {
        for (size_t c = 0; c < rows[r].size(); ++c) {
            const std::string& name = rows[r][c];
            if (name == "." || name.empty()) continue;
            int row1 = static_cast<int>(r) + 1; // CSS line numbers are 1-based
            int col1 = static_cast<int>(c) + 1;
            int row2 = row1 + 1;
            int col2 = col1 + 1;
            if (auto* existing = find(name)) {
                existing->col_start = std::min(existing->col_start, col1);
                existing->row_start = std::min(existing->row_start, row1);
                existing->col_end   = std::max(existing->col_end,   col2);
                existing->row_end   = std::max(existing->row_end,   row2);
            } else {
                out.push_back({name, col1, col2, row1, row2});
            }
        }
    }
    return out;
}

// ── Grid layout algorithm ───────────────────────────────────────────────────

constexpr float kDefaultGridAutoRowHeight = 30.0f;

static float content_height_for_grid_auto_row(const View& view) {
    const auto& fs = view.flex();

    if (view.child_count() == 0)
        return 0.0f;

    if (fs.preferred_height > 0.0f)
        return fs.preferred_height;

    float height = view.intrinsic_height();
    if (height > 0.0f)
        return height;

    float child_height = 0.0f;
    for (std::size_t i = 0; i < view.child_count(); ++i) {
        const auto* child = view.child_at(i);
        if (!child->visible()) continue;

        const auto& cf = child->flex();
        float h = cf.preferred_height;
        if (h <= 0.0f)
            h = child->intrinsic_height();
        if (h <= 0.0f)
            h = content_height_for_grid_auto_row(*child);

        if (h > 0.0f)
            child_height = std::max(child_height, h + cf.margin_t() + cf.margin_b());
    }

    if (child_height <= 0.0f)
        return 0.0f;

    const float pt = fs.padding_top >= 0 ? fs.padding_top : fs.padding;
    const float pb = fs.padding_bottom >= 0 ? fs.padding_bottom : fs.padding;
    return child_height + pt + pb;
}

static bool grid_row_uses_auto_content_height(const std::vector<GridTrack>& rows, int row) {
    if (row < 0)
        return false;
    if (row >= static_cast<int>(rows.size()))
        return true;
    return rows[static_cast<std::size_t>(row)].type == GridTrack::Type::auto_;
}

void layout_grid(View& parent) {
    auto area = parent.local_bounds();
    auto& gs = parent.grid();
    auto& fs = parent.flex();

    // Padding
    float pt = fs.padding_top >= 0 ? fs.padding_top : fs.padding;
    float pr = fs.padding_right >= 0 ? fs.padding_right : fs.padding;
    float pb = fs.padding_bottom >= 0 ? fs.padding_bottom : fs.padding;
    float pl = fs.padding_left >= 0 ? fs.padding_left : fs.padding;
    area = {area.x + pl, area.y + pt, area.width - pl - pr, area.height - pt - pb};

    auto& cols = gs.template_columns;
    auto& rows = gs.template_rows;
    float col_gap = gs.column_gap;
    float row_gap = gs.row_gap;

    if (cols.empty()) return;  // No grid definition

    // Resolve column widths
    int num_cols = static_cast<int>(cols.size());
    std::vector<float> col_widths(static_cast<size_t>(num_cols), 0);
    float total_fixed_w = 0;
    float total_fr_w = 0;
    float total_col_gaps = num_cols > 1 ? col_gap * (num_cols - 1) : 0;

    for (int i = 0; i < num_cols; ++i) {
        if (cols[static_cast<size_t>(i)].type == GridTrack::Type::fixed) {
            col_widths[static_cast<size_t>(i)] = cols[static_cast<size_t>(i)].value;
            total_fixed_w += cols[static_cast<size_t>(i)].value;
        } else if (cols[static_cast<size_t>(i)].type == GridTrack::Type::fr) {
            total_fr_w += cols[static_cast<size_t>(i)].value;
        }
    }

    float remaining_w = area.width - total_fixed_w - total_col_gaps;
    if (remaining_w < 0) remaining_w = 0;

    for (int i = 0; i < num_cols; ++i) {
        auto& t = cols[static_cast<size_t>(i)];
        if (t.type == GridTrack::Type::fr && total_fr_w > 0) {
            col_widths[static_cast<size_t>(i)] = remaining_w * (t.value / total_fr_w);
        } else if (t.type == GridTrack::Type::auto_) {
            // Auto: share remaining width using the current total column count.
            col_widths[static_cast<size_t>(i)] = remaining_w / std::max(1.0f, static_cast<float>(num_cols));
        }
    }

    // Collect visible children
    std::vector<View*> children;
    for (size_t i = 0; i < parent.child_count(); ++i) {
        auto* child = parent.child_at(i);
        if (child->visible()) children.push_back(child);
    }

    // Auto-place children in grid cells
    int num_rows_needed = rows.empty()
        ? static_cast<int>((children.size() + static_cast<size_t>(num_cols) - 1) / static_cast<size_t>(num_cols))
        : static_cast<int>(rows.size());

    auto child_grid_position = [num_cols, num_rows_needed](size_t child_index, const View& child) {
        const auto& child_grid = child.grid();

        int col = child_grid.grid_column_start > 0
            ? child_grid.grid_column_start - 1
            : static_cast<int>(child_index) % num_cols;
        int row = child_grid.grid_row_start > 0
            ? child_grid.grid_row_start - 1
            : static_cast<int>(child_index) / num_cols;

        if (col >= num_cols) col = num_cols - 1;
        if (row >= num_rows_needed) row = num_rows_needed - 1;
        if (col < 0) col = 0;
        if (row < 0) row = 0;
        return std::pair<int, int>{col, row};
    };

    std::vector<float> auto_row_min_heights(static_cast<size_t>(num_rows_needed), 0.0f);
    for (size_t ci = 0; ci < children.size(); ++ci) {
        auto* child = children[ci];
        auto [col, row_idx] = child_grid_position(ci, *child);
        (void)col;

        const float content_h = content_height_for_grid_auto_row(*child);
        if (content_h <= 0.0f)
            continue;

        const auto& child_grid = child->grid();
        int row_end = child_grid.grid_row_end > 0 ? child_grid.grid_row_end - 1 : row_idx + 1;
        row_end = std::clamp(row_end, row_idx + 1, num_rows_needed);
        const int row_span = std::max(1, row_end - row_idx);
        const float spanned_gaps = row_span > 1 ? row_gap * static_cast<float>(row_span - 1) : 0.0f;
        const float per_row_h = std::max(0.0f, (content_h - spanned_gaps) / static_cast<float>(row_span));

        for (int row = row_idx; row < row_end; ++row) {
            if (!grid_row_uses_auto_content_height(rows, row))
                continue;
            auto& min_h = auto_row_min_heights[static_cast<size_t>(row)];
            min_h = std::max(min_h, per_row_h);
        }
    }

    // Resolve row heights
    std::vector<float> row_heights(static_cast<size_t>(num_rows_needed), 0);
    float total_fixed_h = 0;
    float total_fr_h = 0;
    float total_row_gaps = num_rows_needed > 1 ? row_gap * (num_rows_needed - 1) : 0;

    for (int i = 0; i < num_rows_needed; ++i) {
        if (i < static_cast<int>(rows.size())) {
            auto& t = rows[static_cast<size_t>(i)];
            if (t.type == GridTrack::Type::fixed) {
                row_heights[static_cast<size_t>(i)] = t.value;
                total_fixed_h += t.value;
            } else if (t.type == GridTrack::Type::fr) {
                total_fr_h += t.value;
            }
        }
    }

    float remaining_h = area.height - total_fixed_h - total_row_gaps;
    if (remaining_h < 0) remaining_h = 0;

    for (int i = 0; i < num_rows_needed; ++i) {
        if (i < static_cast<int>(rows.size())) {
            auto& t = rows[static_cast<size_t>(i)];
            if (t.type == GridTrack::Type::fr && total_fr_h > 0) {
                row_heights[static_cast<size_t>(i)] = remaining_h * (t.value / total_fr_h);
            } else if (t.type == GridTrack::Type::auto_) {
                row_heights[static_cast<size_t>(i)] = std::max(
                    kDefaultGridAutoRowHeight,
                    auto_row_min_heights[static_cast<size_t>(i)]
                );
            }
        } else {
            // Implicit rows (auto-generated) — use auto height
            row_heights[static_cast<size_t>(i)] = std::max(
                kDefaultGridAutoRowHeight,
                auto_row_min_heights[static_cast<size_t>(i)]
            );
        }
    }

    // Position children in cells
    for (size_t ci = 0; ci < children.size(); ++ci) {
        auto* child = children[ci];
        auto& child_grid = child->grid();

        auto [col, row_idx] = child_grid_position(ci, *child);

        // Compute position from column/row offsets
        float x = area.x;
        for (int c = 0; c < col; ++c)
            x += col_widths[static_cast<size_t>(c)] + col_gap;

        float y = area.y;
        for (int r = 0; r < row_idx; ++r)
            y += row_heights[static_cast<size_t>(r)] + row_gap;

        float w = col_widths[static_cast<size_t>(col)];
        float h = row_heights[static_cast<size_t>(row_idx)];

        // Handle column/row span
        int col_end = child_grid.grid_column_end > 0 ? child_grid.grid_column_end - 1 : col + 1;
        int row_end = child_grid.grid_row_end > 0 ? child_grid.grid_row_end - 1 : row_idx + 1;
        for (int c = col + 1; c < col_end && c < num_cols; ++c)
            w += col_widths[static_cast<size_t>(c)] + col_gap;
        for (int r = row_idx + 1; r < row_end && r < num_rows_needed; ++r)
            h += row_heights[static_cast<size_t>(r)] + row_gap;

        child->set_bounds({x, y, w, h});
        child->layout_children();
    }
}

} // namespace pulp::view
