// SPDX-License-Identifier: MIT
#include "html_project_stager.hpp"

#include <algorithm>
#include <cctype>
#include <deque>
#include <fstream>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <system_error>

namespace pulp::import_design {

namespace fs = std::filesystem;

namespace {

constexpr std::uintmax_t kMaximumDependencyBytes = 256ull * 1024ull * 1024ull;
constexpr std::uintmax_t kMaximumFileBytes = 32ull * 1024ull * 1024ull;
constexpr std::size_t kMaximumDependencyFiles = 4096;

bool is_within(const fs::path& root, const fs::path& candidate) {
    const auto relative = candidate.lexically_relative(root);
    if (relative.empty()) return candidate == root;
    return !relative.is_absolute() && *relative.begin() != "..";
}

std::string decode_url_path(std::string value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size() &&
            std::isxdigit(static_cast<unsigned char>(value[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(value[i + 2]))) {
            const auto hex = value.substr(i + 1, 2);
            decoded.push_back(static_cast<char>(std::stoi(hex, nullptr, 16)));
            i += 2;
        } else {
            decoded.push_back(value[i]);
        }
    }
    return decoded;
}

struct DependencyReference {
    fs::path path;
    bool root_relative = false;
    bool scan_as_text = false;
};

std::optional<DependencyReference> safe_relative_reference(
    std::string value) {
    const auto hash = value.find('#');
    if (hash != std::string::npos) value.resize(hash);
    const auto query = value.find('?');
    if (query != std::string::npos) value.resize(query);
    value = decode_url_path(std::move(value));
    while (!value.empty() && std::isspace(
               static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && std::isspace(
               static_cast<unsigned char>(value.back())))
        value.pop_back();
    const bool root_relative = !value.empty() && value.front() == '/';
    if (root_relative)
        value.erase(value.begin());  // browser origin root == staged project root
    if (value.empty() || value.front() == '\\' ||
        value.find("://") != std::string::npos ||
        value.starts_with("data:") || value.starts_with("blob:") ||
        value.starts_with("javascript:"))
        return std::nullopt;
    fs::path relative(value);
    if (relative.is_absolute()) return std::nullopt;
    return DependencyReference{
        relative.lexically_normal(), root_relative, false};
}

std::vector<DependencyReference> referenced_paths(std::string_view content) {
    static const std::regex kReference{
        R"((?:src|href|poster)\s*=\s*["']([^"']+)["']|url\(\s*["']?([^"')]+)|@import\s+(?:url\(\s*)?["']([^"']+)["']|(?:import|fetch)\s*\(\s*["']([^"']+)["']|import\s*["']([^"']+)["']|from\s*["']([^"']+)["'])",
        std::regex::icase};
    // Extensionless scripts and styles still need transitive dependency scans.
    // Keep the generic reference matcher above for the complete copy graph, then
    // mark references whose syntax establishes a text/executable context.
    static const std::regex kScannableReference{
        R"(<script\b[^>]*\bsrc\s*=\s*["']([^"']+)["']|<link\b[^>]*\bhref\s*=\s*["']([^"']+)["']|@import\s+(?:url\(\s*)?["']([^"']+)["']|import\s*\(\s*["']([^"']+)["']|import\s*["']([^"']+)["']|from\s*["']([^"']+)["'])",
        std::regex::icase};
    static const std::regex kSrcset{
        R"(\bsrcset\s*=\s*["']([^"']+)["'])",
        std::regex::icase};
    std::vector<DependencyReference> result;
    const std::string text(content);
    auto append = [&](std::string value, bool scan_as_text) {
        auto reference = safe_relative_reference(std::move(value));
        if (!reference) return;
        const auto existing = std::find_if(
            result.begin(), result.end(), [&](const auto& item) {
                return item.path == reference->path &&
                       item.root_relative == reference->root_relative;
            });
        if (existing != result.end()) {
            existing->scan_as_text |= scan_as_text;
            return;
        }
        reference->scan_as_text = scan_as_text;
        result.push_back(std::move(*reference));
    };
    for (std::sregex_iterator it(text.begin(), text.end(), kReference), end;
         it != end; ++it) {
        for (std::size_t group = 1; group < it->size(); ++group) {
            if (!(*it)[group].matched) continue;
            append((*it)[group].str(), false);
            break;
        }
    }
    for (std::sregex_iterator it(text.begin(), text.end(), kSrcset), end;
         it != end; ++it) {
        std::istringstream candidates((*it)[1].str());
        for (std::string candidate; std::getline(candidates, candidate, ',');) {
            std::istringstream fields(candidate);
            std::string url;
            fields >> url;
            if (!url.empty()) append(std::move(url), false);
        }
    }
    for (std::sregex_iterator it(
             text.begin(), text.end(), kScannableReference), end;
         it != end; ++it) {
        for (std::size_t group = 1; group < it->size(); ++group) {
            if (!(*it)[group].matched) continue;
            append((*it)[group].str(), true);
            break;
        }
    }
    return result;
}

bool should_scan(const fs::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return extension == ".html" || extension == ".htm" ||
           extension == ".css" || extension == ".js" ||
           extension == ".mjs" || extension == ".jsx" ||
           extension == ".tsx";
}

std::string read_text(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream bytes;
    bytes << input.rdbuf();
    return bytes.str();
}

}  // namespace

StagedHtmlProject stage_html_project(
    const fs::path& input_file,
    std::string_view input_content,
    const HtmlProjectStageOptions& options) {
    StagedHtmlProject result;
    std::string workspace_error;
    result.workspace =
        BrowserCaptureWorkspace::create("pulp-html-project", workspace_error);
    if (!result.workspace) {
        result.error = std::move(workspace_error);
        return result;
    }
    std::error_code ec;
    const auto source_entry = fs::canonical(input_file, ec);
    if (ec || !fs::is_regular_file(source_entry, ec) || ec) {
        result.error = "could not resolve HTML entry for staging";
        return result;
    }
    const auto entry_bytes = fs::file_size(source_entry, ec);
    if (ec || entry_bytes > kMaximumFileBytes ||
        entry_bytes > kMaximumDependencyBytes) {
        result.error =
            "HTML entry exceeds capture staging limits";
        return result;
    }
    const auto source_root = source_entry.parent_path();
    result.root = result.workspace->root() / "project";
    fs::create_directory(result.root, ec);
    if (ec) {
        result.error = "could not create staged project root: " + ec.message();
        return result;
    }
    result.entry = result.root / source_entry.filename();
    fs::copy_file(
        source_entry, result.entry, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        result.error = "could not stage HTML entry: " + ec.message();
        return result;
    }

    std::deque<std::pair<fs::path, std::string>> pending;
    pending.emplace_back(fs::path{}, std::string(input_content));
    std::set<fs::path> visited;
    std::set<fs::path> scanned;
    std::uintmax_t total_bytes = entry_bytes;

    while (!pending.empty()) {
        auto [base, content] = std::move(pending.front());
        pending.pop_front();
        auto references = referenced_paths(content);
        const auto additional_roots =
            options.discover_additional_roots
                ? options.discover_additional_roots(content)
                : std::vector<std::string>{};
        for (const auto& root : additional_roots) {
            const auto dependency_root = safe_relative_reference(root);
            if (!dependency_root) continue;
            const auto relative_root =
                (dependency_root->root_relative
                     ? dependency_root->path
                     : base / dependency_root->path).lexically_normal();
            if (relative_root.empty() || relative_root.is_absolute() ||
                *relative_root.begin() == "..") {
                continue;
            }
            const auto unresolved_root = source_root / relative_root;
            const auto canonical_root =
                fs::weakly_canonical(unresolved_root, ec);
            if (ec || !is_within(source_root, canonical_root) ||
                !fs::is_directory(canonical_root, ec) || ec) {
                ec.clear();
                continue;
            }
            for (fs::recursive_directory_iterator it(canonical_root, ec), end;
                 !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec) || ec) {
                    ec.clear();
                    continue;
                }
                const auto relative =
                    it->path().lexically_relative(source_root);
                references.push_back(DependencyReference{
                    relative.lexically_relative(base), false, false});
            }
            if (ec) {
                result.error =
                    "could not enumerate bound design-system dependencies: " +
                    ec.message();
                return result;
            }
        }
        for (const auto& reference : references) {
            const auto relative =
                (reference.root_relative
                     ? reference.path
                     : base / reference.path).lexically_normal();
            if (relative.empty() || relative.is_absolute() ||
                *relative.begin() == "..")
                continue;
            if (!visited.insert(relative).second) {
                // A non-scannable reference (for example fetch("module")) may
                // discover an extensionless file before a later static import
                // establishes that it is executable text. Upgrade that already
                // staged dependency exactly once instead of letting traversal
                // order decide whether its transitive graph is copied.
                if (reference.scan_as_text &&
                    scanned.insert(relative).second) {
                    pending.emplace_back(
                        relative.parent_path(),
                        read_text(result.root / relative));
                }
                continue;
            }
            if (visited.size() > kMaximumDependencyFiles) {
                result.error = "HTML dependency graph exceeds 4096 files";
                return result;
            }
            const auto unresolved = source_root / relative;
            const auto source = fs::weakly_canonical(unresolved, ec);
            if (ec || !is_within(source_root, source) ||
                !fs::is_regular_file(source, ec) || ec) {
                ec.clear();
                continue;
            }
            const auto size = fs::file_size(source, ec);
            if (ec || size > kMaximumFileBytes ||
                total_bytes > kMaximumDependencyBytes - size) {
                result.error =
                    "HTML dependency graph exceeds capture staging limits";
                return result;
            }
            total_bytes += size;
            const auto destination = result.root / relative;
            fs::create_directories(destination.parent_path(), ec);
            if (ec) {
                result.error =
                    "could not create staged dependency directory: " +
                    ec.message();
                return result;
            }
            fs::copy_file(
                source, destination, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                result.error =
                    "could not stage HTML dependency: " + ec.message();
                return result;
            }
            result.dependencies.push_back(relative);
            if (should_scan(source) || reference.scan_as_text) {
                scanned.insert(relative);
                pending.emplace_back(relative.parent_path(), read_text(source));
            }
        }
    }
    return result;
}

}  // namespace pulp::import_design
