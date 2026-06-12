#include "text_edit_model.hpp"
#include <pulp/canvas/text_utf8.hpp>

#include <algorithm>
#include <cctype>

namespace pulp::view::text_edit {

namespace {

int clamp_byte(const std::string& text, int byte_offset) noexcept {
    return std::clamp(byte_offset, 0, static_cast<int>(text.size()));
}

bool cluster_is_space(const std::string& text, int cluster_start) noexcept {
    cluster_start = clamp_boundary(text, cluster_start);
    if (cluster_start >= static_cast<int>(text.size())) return false;
    const auto c = static_cast<unsigned char>(text[static_cast<std::size_t>(cluster_start)]);
    return std::isspace(c) != 0;
}

} // namespace

int clamp_boundary(const std::string& text, int byte_offset) noexcept {
    const int clamped = clamp_byte(text, byte_offset);
    if (clamped == 0 || clamped == static_cast<int>(text.size())) return clamped;

    int previous = 0;
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        const std::size_t next = pulp::canvas::cluster_step(text, cursor, true);
        if (next <= cursor) break;
        if (static_cast<int>(next) == clamped) return clamped;
        if (static_cast<int>(next) > clamped) {
            const int next_i = static_cast<int>(next);
            return (clamped - previous) <= (next_i - clamped) ? previous : next_i;
        }
        previous = static_cast<int>(next);
        cursor = next;
    }

    const auto safe = pulp::canvas::safe_utf8_prefix_size(text, static_cast<std::size_t>(clamped));
    return static_cast<int>(safe);
}

int previous_cluster(const std::string& text, int byte_offset) noexcept {
    const int pos = clamp_byte(text, byte_offset);
    return static_cast<int>(pulp::canvas::cluster_step(text, static_cast<std::size_t>(pos), false));
}

int next_cluster(const std::string& text, int byte_offset) noexcept {
    const int pos = clamp_byte(text, byte_offset);
    return static_cast<int>(pulp::canvas::cluster_step(text, static_cast<std::size_t>(pos), true));
}

int move_clusters(const std::string& text, int byte_offset, int delta) noexcept {
    int pos = clamp_boundary(text, byte_offset);
    if (delta > 0) {
        for (int i = 0; i < delta; ++i) pos = next_cluster(text, pos);
    } else {
        for (int i = 0; i > delta; --i) pos = previous_cluster(text, pos);
    }
    return pos;
}

std::vector<int> cluster_boundaries(const std::string& text) {
    std::vector<int> out;
    out.reserve(text.size() + 1);
    out.push_back(0);
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        std::size_t next = pulp::canvas::cluster_step(text, cursor, true);
        if (next <= cursor) next = cursor + 1;
        next = std::min(next, text.size());
        out.push_back(static_cast<int>(next));
        cursor = next;
    }
    return out;
}

int cluster_index_for_position(const std::vector<int>& boundaries, int byte_offset) noexcept {
    if (boundaries.empty()) return 0;
    auto it = std::lower_bound(boundaries.begin(), boundaries.end(), byte_offset);
    if (it == boundaries.end()) return static_cast<int>(boundaries.size()) - 1;
    if (*it == byte_offset) return static_cast<int>(it - boundaries.begin());
    if (it == boundaries.begin()) return 0;
    return static_cast<int>((it - boundaries.begin()) - 1);
}

int position_for_cluster_index(const std::vector<int>& boundaries, int index) noexcept {
    if (boundaries.empty()) return 0;
    index = std::clamp(index, 0, static_cast<int>(boundaries.size()) - 1);
    return boundaries[static_cast<std::size_t>(index)];
}

int line_start(const std::string& text, int byte_offset) noexcept {
    int pos = clamp_boundary(text, byte_offset);
    while (pos > 0 && text[static_cast<std::size_t>(pos - 1)] != '\n') {
        pos = previous_cluster(text, pos);
    }
    return pos;
}

int line_end(const std::string& text, int byte_offset) noexcept {
    int pos = clamp_boundary(text, byte_offset);
    const int len = static_cast<int>(text.size());
    while (pos < len && text[static_cast<std::size_t>(pos)] != '\n') {
        pos = next_cluster(text, pos);
    }
    return pos;
}

int cluster_column_in_line(const std::string& text, int line_start_offset,
                           int byte_offset) noexcept {
    int column = 0;
    int cursor = clamp_boundary(text, line_start_offset);
    const int target = clamp_boundary(text, byte_offset);
    while (cursor < target) {
        const int next = next_cluster(text, cursor);
        if (next <= cursor) break;
        cursor = next;
        ++column;
    }
    return column;
}

int position_at_cluster_column(const std::string& text, int line_start_offset,
                               int line_end_offset, int cluster_column) noexcept {
    int cursor = clamp_boundary(text, line_start_offset);
    const int end = clamp_boundary(text, line_end_offset);
    for (int i = 0; i < cluster_column && cursor < end; ++i) {
        const int next = next_cluster(text, cursor);
        if (next <= cursor || next > end) break;
        cursor = next;
    }
    return cursor;
}

int visual_line_position(const std::string& text, int caret, int direction,
                         int preferred_cluster_column) noexcept {
    caret = clamp_boundary(text, caret);
    const int current_start = line_start(text, caret);
    const int current_end = line_end(text, caret);
    const int column = std::max(0, preferred_cluster_column);

    if (direction < 0) {
        if (current_start == 0) return caret;
        const int previous_end = current_start - 1;
        const int previous_start = line_start(text, previous_end);
        return position_at_cluster_column(text, previous_start, previous_end, column);
    }

    if (current_end >= static_cast<int>(text.size())) return caret;
    const int next_start = current_end + 1;
    const int next_end = line_end(text, next_start);
    return position_at_cluster_column(text, next_start, next_end, column);
}

int previous_word_start(const std::string& text, int byte_offset) noexcept {
    int pos = clamp_boundary(text, byte_offset);
    while (pos > 0) {
        const int prev = previous_cluster(text, pos);
        if (!cluster_is_space(text, prev)) break;
        pos = prev;
    }
    while (pos > 0) {
        const int prev = previous_cluster(text, pos);
        if (cluster_is_space(text, prev)) break;
        pos = prev;
    }
    return pos;
}

int next_word_start(const std::string& text, int byte_offset) noexcept {
    int pos = clamp_boundary(text, byte_offset);
    const int len = static_cast<int>(text.size());
    while (pos < len && !cluster_is_space(text, pos)) pos = next_cluster(text, pos);
    while (pos < len && cluster_is_space(text, pos)) pos = next_cluster(text, pos);
    return pos;
}

std::pair<int, int> word_range_at(const std::string& text, int byte_offset) noexcept {
    if (text.empty()) return {0, 0};
    int pos = clamp_boundary(text, byte_offset);
    if (pos == static_cast<int>(text.size()) || cluster_is_space(text, pos)) {
        const int prev = previous_cluster(text, pos);
        if (prev != pos && !cluster_is_space(text, prev)) pos = prev;
    }
    if (pos < 0 || pos >= static_cast<int>(text.size()) || cluster_is_space(text, pos)) {
        pos = clamp_boundary(text, byte_offset);
        return {pos, pos};
    }

    int start = pos;
    while (start > 0) {
        const int prev = previous_cluster(text, start);
        if (cluster_is_space(text, prev)) break;
        start = prev;
    }

    int end = pos;
    while (end < static_cast<int>(text.size()) && !cluster_is_space(text, end)) {
        end = next_cluster(text, end);
    }
    return {start, end};
}

std::pair<int, int> line_range_at(const std::string& text, int byte_offset) noexcept {
    const int pos = clamp_boundary(text, byte_offset);
    return {line_start(text, pos), line_end(text, pos)};
}

std::string normalize_insert_text(std::string text, bool multi_line, LineEndingMode mode) {
    if (mode == LineEndingMode::preserve && multi_line) return text;

    std::string normalized;
    normalized.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
            if (mode == LineEndingMode::strip) continue;
            normalized += multi_line ? '\n' : ' ';
        } else if (c == '\n') {
            if (mode == LineEndingMode::strip) continue;
            normalized += multi_line ? '\n' : ' ';
        } else {
            normalized += c;
        }
    }
    return normalized;
}

std::string filter_numeric(std::string text) {
    std::string filtered;
    filtered.reserve(text.size());
    for (char c : text) {
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == '-')
            filtered += c;
    }
    return filtered;
}

std::size_t cluster_count(const std::string& text) noexcept {
    std::size_t count = 0;
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        std::size_t next = pulp::canvas::cluster_step(text, cursor, true);
        if (next <= cursor) next = cursor + 1;
        next = std::min(next, text.size());
        cursor = next;
        ++count;
    }
    return count;
}

std::string truncate_to_cluster_count(const std::string& existing,
                                      std::string insertion,
                                      std::size_t max_clusters) {
    if (max_clusters == 0) return insertion;
    const std::size_t used = cluster_count(existing);
    if (used >= max_clusters) return {};
    const std::size_t allowed = max_clusters - used;
    std::size_t cursor = 0;
    for (std::size_t count = 0; cursor < insertion.size() && count < allowed; ++count) {
        std::size_t next = pulp::canvas::cluster_step(insertion, cursor, true);
        if (next <= cursor) next = cursor + 1;
        cursor = std::min(next, insertion.size());
    }
    insertion.resize(cursor);
    return insertion;
}

} // namespace pulp::view::text_edit
