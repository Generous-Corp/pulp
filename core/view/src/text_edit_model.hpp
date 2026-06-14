#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace pulp::view::text_edit {

/// TextEditor stores public/host-facing positions as UTF-8 byte offsets.
/// This helper keeps those offsets on grapheme-cluster boundaries so editor
/// commands do not split emoji, combining marks, flags, or ZWJ sequences.
int clamp_boundary(const std::string& text, int byte_offset) noexcept;
int previous_cluster(const std::string& text, int byte_offset) noexcept;
int next_cluster(const std::string& text, int byte_offset) noexcept;
int move_clusters(const std::string& text, int byte_offset, int delta) noexcept;

std::vector<int> cluster_boundaries(const std::string& text);
int cluster_index_for_position(const std::vector<int>& boundaries, int byte_offset) noexcept;
int position_for_cluster_index(const std::vector<int>& boundaries, int index) noexcept;

int line_start(const std::string& text, int byte_offset) noexcept;
int line_end(const std::string& text, int byte_offset) noexcept;
int visual_line_position(const std::string& text, int caret, int direction,
                         int preferred_cluster_column) noexcept;
int cluster_column_in_line(const std::string& text, int line_start_offset,
                           int byte_offset) noexcept;
int position_at_cluster_column(const std::string& text, int line_start_offset,
                               int line_end_offset, int cluster_column) noexcept;

int previous_word_start(const std::string& text, int byte_offset) noexcept;
int next_word_start(const std::string& text, int byte_offset) noexcept;
std::pair<int, int> word_range_at(const std::string& text, int byte_offset) noexcept;
std::pair<int, int> line_range_at(const std::string& text, int byte_offset) noexcept;

enum class LineEndingMode {
    normalize,
    strip,
    preserve,
};

std::string normalize_insert_text(std::string text, bool multi_line,
                                  LineEndingMode mode = LineEndingMode::normalize);
std::string filter_numeric(std::string text);
std::string truncate_to_cluster_count(const std::string& existing,
                                      std::string insertion,
                                      std::size_t max_clusters);
std::size_t cluster_count(const std::string& text) noexcept;

} // namespace pulp::view::text_edit
