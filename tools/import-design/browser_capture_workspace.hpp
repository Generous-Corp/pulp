// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <memory>
#include <string_view>

namespace pulp::import_design {

class BrowserCaptureWorkspace {
public:
    static std::shared_ptr<BrowserCaptureWorkspace> create(
        std::string_view prefix, std::string& error);

    BrowserCaptureWorkspace(const BrowserCaptureWorkspace&) = delete;
    BrowserCaptureWorkspace& operator=(const BrowserCaptureWorkspace&) = delete;
    ~BrowserCaptureWorkspace();

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }

private:
    explicit BrowserCaptureWorkspace(std::filesystem::path root)
        : root_(std::move(root)) {}

    std::filesystem::path root_;
};

bool commit_browser_capture_directory(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    std::string& error);

bool preflight_browser_capture_directory(
    const std::filesystem::path& destination,
    std::string& error);

}  // namespace pulp::import_design
