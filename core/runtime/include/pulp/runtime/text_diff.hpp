#pragma once

// Simple text diff using LCS (Longest Common Subsequence)

#include <string>
#include <string_view>
#include <vector>

namespace pulp::runtime {

enum class DiffOp { Equal, Insert, Delete };

struct DiffEntry {
    DiffOp op;
    std::string text;
};

/// Compute a line-based diff between two strings.
/// Returns a sequence of operations to transform `from` into `to`.
std::vector<DiffEntry> text_diff(std::string_view from, std::string_view to);

/// Format a diff as a unified diff string (with +/- prefixes).
std::string format_diff(const std::vector<DiffEntry>& diff);

}  // namespace pulp::runtime
