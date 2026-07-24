#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace pulp::tools::timeline {

struct OperationResult {
    int exit_code = 0;
    std::string json;

    explicit operator bool() const noexcept {
        return exit_code == 0;
    }
};

/// Converts a UTF-8 text boundary (JSON, MCP, or CLI bytes) to a native path.
std::filesystem::path filesystem_path_from_utf8(std::string_view value);
/// Converts a native path to UTF-8 for JSON and other text output.
std::string filesystem_path_to_utf8(const std::filesystem::path& value);

enum class ProjectSourceKind : std::uint8_t {
    AutoDetect,
    InlineJson,
    File,
};

/// An explicit source for a timeline operation.
///
/// Use `inline_json()` or `file()` when the caller already knows the source
/// kind. `auto_detect()` preserves the command-line convenience of accepting
/// either canonical JSON or a path.
class ProjectSource {
  public:
    static ProjectSource auto_detect(std::string_view value);
    static ProjectSource inline_json(std::string_view json);
    static ProjectSource file(const std::filesystem::path& path);

    ProjectSourceKind kind() const noexcept {
        return kind_;
    }
    std::string_view text() const noexcept {
        return text_;
    }
    const std::filesystem::path& file_path() const noexcept {
        return file_path_;
    }

  private:
    explicit ProjectSource(ProjectSourceKind kind, std::string text);
    explicit ProjectSource(std::filesystem::path path);

    ProjectSourceKind kind_ = ProjectSourceKind::AutoDetect;
    std::string text_;
    std::filesystem::path file_path_;
};

OperationResult project_open(const ProjectSource& project);
OperationResult command_apply(const ProjectSource& project, std::string_view commands);
OperationResult validate(const ProjectSource& project);
OperationResult explain(const ProjectSource& project, std::uint32_t sample_rate = 48'000);
OperationResult render(const ProjectSource& project, const std::filesystem::path& output,
                       std::uint32_t sample_rate = 48'000);

/// Convenience overloads that auto-detect canonical inline JSON versus a path.
OperationResult project_open(std::string_view project);
OperationResult command_apply(std::string_view project, std::string_view commands);
OperationResult validate(std::string_view project);
OperationResult explain(std::string_view project, std::uint32_t sample_rate = 48'000);
OperationResult render(std::string_view project, const std::filesystem::path& output,
                       std::uint32_t sample_rate = 48'000);
OperationResult schema();

} // namespace pulp::tools::timeline
