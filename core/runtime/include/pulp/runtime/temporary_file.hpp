#pragma once

// RAII temporary file — creates a temp file on construction, deletes on destruction.

#include <filesystem>
#include <string>

namespace pulp::runtime {

class TemporaryFile {
public:
    /// Create a temporary file with optional extension (e.g., ".wav")
    explicit TemporaryFile(std::string_view extension = "");

    ~TemporaryFile();

    /// Full path to the temporary file
    const std::filesystem::path& path() const { return path_; }

    /// String version of the path
    std::string path_string() const { return path_.string(); }

    /// Prevent auto-deletion (e.g., if you want to keep the file)
    void release() { active_ = false; }

    /// Create `path` as an empty file readable and writable by the owner
    /// only. Fails without touching the file when `path` already exists.
    static bool create_exclusive(const std::filesystem::path& path);

    // No copy
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    // Move
    TemporaryFile(TemporaryFile&& other) noexcept;
    TemporaryFile& operator=(TemporaryFile&& other) noexcept;

private:
    std::filesystem::path path_;
    bool active_ = true;
};

}  // namespace pulp::runtime
