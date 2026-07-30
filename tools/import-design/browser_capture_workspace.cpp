// SPDX-License-Identifier: MIT
#include "browser_capture_workspace.hpp"

#include <chrono>
#include <fstream>
#include <system_error>

namespace pulp::import_design {

namespace fs = std::filesystem;

std::shared_ptr<BrowserCaptureWorkspace> BrowserCaptureWorkspace::create(
    std::string_view prefix, std::string& error) {
    std::error_code ec;
    const auto temporary_root = fs::temp_directory_path(ec);
    if (ec) {
        error = "could not resolve temporary directory: " + ec.message();
        return {};
    }
    const auto seed = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    for (unsigned int attempt = 0; attempt < 128; ++attempt) {
        const auto candidate =
            temporary_root /
            (std::string(prefix) + "-" + std::to_string(seed) + "-" +
             std::to_string(attempt));
        if (fs::create_directory(candidate, ec)) {
            fs::permissions(
                candidate, fs::perms::owner_all,
                fs::perm_options::replace, ec);
            if (ec) {
                fs::remove_all(candidate, ec);
                error = "could not restrict temporary directory permissions";
                return {};
            }
            return std::shared_ptr<BrowserCaptureWorkspace>(
                new BrowserCaptureWorkspace(candidate));
        }
        if (ec && ec != std::errc::file_exists) {
            error = "could not create temporary directory: " + ec.message();
            return {};
        }
        ec.clear();
    }
    error = "could not allocate a unique temporary directory";
    return {};
}

BrowserCaptureWorkspace::~BrowserCaptureWorkspace() {
    std::error_code ec;
    if (!root_.empty()) fs::remove_all(root_, ec);
}

bool preflight_browser_capture_directory(
    const fs::path& destination,
    std::string& error) {
    if (destination.empty()) return true;

    std::error_code ec;
    if (fs::exists(destination, ec) && !ec &&
        (!fs::is_directory(destination, ec) || ec ||
         !fs::is_regular_file(
             destination / ".pulp-browser-capture-v1", ec) || ec)) {
        error = "refusing to replace unowned capture directory: " +
                destination.string();
        return false;
    }
    ec.clear();
    auto parent = destination.parent_path();
    if (parent.empty()) parent = fs::current_path(ec);
    if (ec) {
        error = "could not resolve capture artifact directory: " +
                ec.message();
        return false;
    }
    fs::create_directories(parent, ec);
    if (ec) {
        error = "could not create capture artifact directory: " +
                ec.message();
        return false;
    }

    const auto nonce = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    const auto probe = parent /
        ("." + destination.filename().string() + ".preflight-" +
         std::to_string(nonce));
    if (!fs::create_directory(probe, ec) || ec) {
        error = "capture artifact directory is not writable: " +
                (ec ? ec.message() : std::string("probe already exists"));
        return false;
    }
    fs::remove(probe, ec);
    if (ec) {
        error = "could not clean capture artifact preflight: " + ec.message();
        return false;
    }
    return true;
}

bool commit_browser_capture_directory(
    const fs::path& source,
    const fs::path& destination,
    std::string& error) {
    std::error_code ec;
    if (!fs::is_directory(source, ec) || ec) {
        error = "browser capture workspace is missing";
        return false;
    }
    fs::create_directories(destination.parent_path(), ec);
    if (ec) {
        error = "could not create capture artifact directory: " + ec.message();
        return false;
    }
    const auto nonce = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    const auto staged = destination.parent_path() /
        ("." + destination.filename().string() + ".tmp-" +
         std::to_string(nonce));
    const auto backup = destination.parent_path() /
        ("." + destination.filename().string() + ".bak-" +
         std::to_string(nonce));
    if (!fs::create_directory(staged, ec) || ec) {
        error = "could not reserve durable capture staging directory: " +
                (ec ? ec.message() : std::string("path already exists"));
        return false;
    }
    fs::copy(source, staged, fs::copy_options::recursive, ec);
    if (ec) {
        const auto copy_error = ec.message();
        std::error_code cleanup_ec;
        fs::remove_all(staged, cleanup_ec);
        error = "could not stage durable capture evidence: " + copy_error;
        if (cleanup_ec)
            error += "; cleanup failed: " + cleanup_ec.message();
        return false;
    }
    {
        std::ofstream marker(staged / ".pulp-browser-capture-v1");
        marker << "owned\n";
        marker.close();
        if (!marker) {
            fs::remove_all(staged, ec);
            error = "could not write browser capture ownership marker";
            return false;
        }
    }
    const bool destination_exists = fs::exists(destination, ec);
    if (ec) {
        const auto inspect_error = ec.message();
        std::error_code cleanup_ec;
        fs::remove_all(staged, cleanup_ec);
        error = "could not inspect prior capture evidence: " +
                inspect_error;
        if (cleanup_ec)
            error += "; cleanup failed: " + cleanup_ec.message();
        return false;
    }
    if (destination_exists) {
        if (!fs::is_regular_file(
                destination / ".pulp-browser-capture-v1", ec) || ec) {
            std::error_code cleanup_ec;
            fs::remove_all(staged, cleanup_ec);
            error = "refusing to replace unowned capture directory: " +
                    destination.string();
            if (cleanup_ec)
                error += "; cleanup failed: " + cleanup_ec.message();
            return false;
        }
        if (!fs::create_directory(backup, ec) || ec) {
            const auto reserve_error =
                ec ? ec.message() : std::string("path already exists");
            std::error_code cleanup_ec;
            fs::remove_all(staged, cleanup_ec);
            error = "could not reserve prior capture backup: " +
                    reserve_error;
            if (cleanup_ec)
                error += "; cleanup failed: " + cleanup_ec.message();
            return false;
        }
        fs::copy(destination, backup, fs::copy_options::recursive, ec);
        if (ec) {
            const auto backup_error = ec.message();
            std::error_code cleanup_ec;
            fs::remove_all(backup, cleanup_ec);
            fs::remove_all(staged, cleanup_ec);
            error = "could not back up prior capture evidence: " +
                    backup_error;
            if (cleanup_ec)
                error += "; cleanup failed: " + cleanup_ec.message();
            return false;
        }
        fs::remove_all(destination, ec);
        if (ec) {
            const auto remove_error = ec.message();
            std::error_code restore_ec;
            fs::rename(backup, destination, restore_ec);
            std::error_code cleanup_ec;
            fs::remove_all(staged, cleanup_ec);
            error = "could not prepare prior capture evidence: " +
                    remove_error;
            if (restore_ec)
                error += "; rollback failed: " + restore_ec.message() +
                         "; prior capture retained at " + backup.string();
            if (cleanup_ec)
                error += "; cleanup failed: " + cleanup_ec.message();
            return false;
        }
    }
    fs::rename(staged, destination, ec);
    if (ec) {
        const auto commit_error = ec.message();
        std::error_code rollback_ec;
        if (fs::exists(backup, rollback_ec)) {
            rollback_ec.clear();
            fs::rename(backup, destination, rollback_ec);
        }
        std::error_code cleanup_ec;
        fs::remove_all(staged, cleanup_ec);
        error = "could not commit capture evidence: " + commit_error;
        if (rollback_ec)
            error += "; rollback failed: " + rollback_ec.message();
        if (cleanup_ec)
            error += "; cleanup failed: " + cleanup_ec.message();
        return false;
    }
    fs::remove_all(backup, ec);
    // Publication has committed. A stale owned backup is preferable to
    // reporting failure and prompting callers to roll back the already-live
    // primary/assets generation while this capture remains installed.
    return true;
}

}  // namespace pulp::import_design
