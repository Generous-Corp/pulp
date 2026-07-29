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

/// Copy an HTML entry and its explicit, contained relative dependency graph to
/// a private scratch directory. Parent traversal and absolute filesystem paths
/// are never authorized.
StagedHtmlProject stage_html_project(
    const std::filesystem::path& input_file,
    std::string_view input_content);

}  // namespace pulp::import_design
