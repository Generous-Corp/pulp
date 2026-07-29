// SPDX-License-Identifier: MIT
#pragma once

#include "browser_capture_workspace.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::import_design {

struct StagedHtmlProject {
    std::shared_ptr<BrowserCaptureWorkspace> workspace;
    std::filesystem::path root;
    std::filesystem::path entry;
    std::vector<std::filesystem::path> dependencies;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return workspace && !entry.empty() && error.empty();
    }
};

using HtmlDependencyRootDiscovery =
    std::vector<std::string> (*)(std::string_view);

struct HtmlProjectStageOptions {
    /// Optional provider-owned discovery for dependency roots that source code
    /// constructs dynamically. The generic stager still validates containment,
    /// file count, and byte limits before copying anything.
    HtmlDependencyRootDiscovery discover_additional_roots = nullptr;
};

/// Copy an HTML entry and its explicit, contained relative dependency graph to
/// a private scratch directory. Parent traversal and absolute filesystem paths
/// are never authorized.
StagedHtmlProject stage_html_project(
    const std::filesystem::path& input_file,
    std::string_view input_content,
    const HtmlProjectStageOptions& options = {});

}  // namespace pulp::import_design
