#include <pulp/runtime/text_diff.hpp>
#include <sstream>
#include <algorithm>

namespace pulp::runtime {

static std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::istringstream stream{std::string(text)};
    std::string line;
    while (std::getline(stream, line))
        lines.push_back(line);
    return lines;
}

std::vector<DiffEntry> text_diff(std::string_view from, std::string_view to) {
    auto a = split_lines(from);
    auto b = split_lines(to);

    const size_t m = a.size();
    const size_t n = b.size();

    // LCS table
    std::vector<std::vector<size_t>> dp(m + 1, std::vector<size_t>(n + 1, 0));
    for (size_t i = 1; i <= m; ++i)
        for (size_t j = 1; j <= n; ++j) {
            if (a[i - 1] == b[j - 1])
                dp[i][j] = dp[i - 1][j - 1] + 1;
            else
                dp[i][j] = std::max(dp[i - 1][j], dp[i][j - 1]);
        }

    // Backtrack to build diff
    std::vector<DiffEntry> result;
    size_t i = m, j = n;
    while (i > 0 || j > 0) {
        if (i > 0 && j > 0 && a[i - 1] == b[j - 1]) {
            result.push_back({DiffOp::Equal, a[i - 1]});
            --i;
            --j;
        } else if (j > 0 && (i == 0 || dp[i][j - 1] >= dp[i - 1][j])) {
            result.push_back({DiffOp::Insert, b[j - 1]});
            --j;
        } else {
            result.push_back({DiffOp::Delete, a[i - 1]});
            --i;
        }
    }

    std::reverse(result.begin(), result.end());
    return result;
}

std::string format_diff(const std::vector<DiffEntry>& diff) {
    std::ostringstream ss;
    for (auto& entry : diff) {
        switch (entry.op) {
            case DiffOp::Equal:  ss << "  " << entry.text << '\n'; break;
            case DiffOp::Insert: ss << "+ " << entry.text << '\n'; break;
            case DiffOp::Delete: ss << "- " << entry.text << '\n'; break;
        }
    }
    return ss.str();
}

}  // namespace pulp::runtime
