// source_jump.cpp — Inspector source-jump implementation.
//
// See pulp/inspect/source_jump.hpp for the public surface and the
// design context (planning Phase 5.1).

#include <pulp/inspect/source_jump.hpp>
#include <pulp/view/view.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#if defined(_WIN32)
  #include <windows.h>
  #include <shellapi.h>
#else
  #include <spawn.h>
  #include <sys/wait.h>
  #include <unistd.h>
  extern char** environ;
#endif

namespace pulp::inspect {

namespace {

bool env_is_truthy(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return false;
    std::string normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return normalized != "0"
        && normalized != "false"
        && normalized != "no"
        && normalized != "off";
}

std::string current_process_name() {
#if defined(_WIN32)
    std::array<char, MAX_PATH> path{};
    const DWORD len = ::GetModuleFileNameA(nullptr, path.data(),
                                           static_cast<DWORD>(path.size()));
    if (len == 0)
        return {};
    std::string name(path.data(), static_cast<std::size_t>(len));
#elif defined(__APPLE__)
    std::string name = ::getprogname() ? ::getprogname() : "";
#else
    std::array<char, 4096> path{};
    const ssize_t len = ::readlink("/proc/self/exe", path.data(), path.size() - 1);
    if (len <= 0)
        return {};
    std::string name(path.data(), static_cast<std::size_t>(len));
#endif
    const auto slash = name.find_last_of("/\\");
    if (slash != std::string::npos)
        name.erase(0, slash + 1);
    return name;
}

bool is_pulp_test_process() {
    const auto name = current_process_name();
    return name.rfind("pulp-test-", 0) == 0;
}

std::optional<std::string> launch_block_reason() {
    if (env_is_truthy("PULP_INSPECTOR_NO_LAUNCH"))
        return "editor launch suppressed because PULP_INSPECTOR_NO_LAUNCH is set";
    if (env_is_truthy("PULP_HEADLESS"))
        return "editor launch suppressed because PULP_HEADLESS is set";
    if (env_is_truthy("PULP_TEST_MODE"))
        return "editor launch suppressed because PULP_TEST_MODE is set";
    if (env_is_truthy("CI"))
        return "editor launch suppressed because CI is set";
    if (is_pulp_test_process())
        return "editor launch suppressed because this is a pulp-test binary";

#if !defined(_WIN32) && !defined(__APPLE__)
    const bool has_x11 = std::getenv("DISPLAY") != nullptr
        && *std::getenv("DISPLAY") != '\0';
    const bool has_wayland = std::getenv("WAYLAND_DISPLAY") != nullptr
        && *std::getenv("WAYLAND_DISPLAY") != '\0';
    if (!has_x11 && !has_wayland)
        return "editor launch suppressed because no display is available";
#endif

    return std::nullopt;
}

void set_launch_error(std::string* out, std::string message) {
    if (out) *out = std::move(message);
}

#if !defined(_WIN32)
bool wait_for_launcher(pid_t pid, std::string_view launcher, std::string* error) {
    int status = 0;
    while (true) {
        const pid_t waited = ::waitpid(pid, &status, 0);
        if (waited == pid)
            break;
        if (waited == -1 && errno == EINTR)
            continue;

        set_launch_error(error, std::string(launcher)
            + " wait failed while launching editor URL");
        return false;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        set_launch_error(error, std::string(launcher)
            + " exited non-zero while launching editor URL");
        return false;
    }

    return true;
}
#endif

} // namespace

SourceJumpResult resolve_source_jump(const InspectorConfig& config,
                                     const pulp::view::View* view) {
    SourceJumpResult result;
    if (view == nullptr) {
        result.error = "no view selected";
        return result;
    }
    if (!view->has_source_loc()) {
        // Not a failure of the inspector — the view simply was not
        // created from a JSX element carrying a `__source` prop. The
        // overlay treats this as a graceful no-op.
        result.error = "view has no source location (not imported from JSX)";
        return result;
    }
    const auto& loc = view->source_loc();
    if (!loc.valid()) {
        result.error = "view source location has an empty file path";
        return result;
    }

    // Apply the env override / config / default precedence (Phase 5.3).
    auto effective = effective_editor_url(config);
    std::string err;
    if (!validate_editor_url_template(effective.template_str, &err)) {
        result.error = "editor URL template invalid: " + err;
        return result;
    }

    result.path = loc.file;
    result.line = loc.line;
    result.col = loc.col;
    // A 0 line/col means "unknown" — substitute 1 for the line so the
    // editor opens at the file top rather than a nonsensical line 0.
    int line = loc.line > 0 ? loc.line : 1;
    std::optional<int> col;
    if (loc.col > 0) col = loc.col;
    result.url = format_editor_url(effective.template_str, loc.file, line, col);
    result.ok = true;
    return result;
}

bool launch_editor_url(std::string_view url, std::string* error) {
    if (url.empty()) {
        set_launch_error(error, "editor URL is empty");
        return false;
    }
    if (auto reason = launch_block_reason()) {
        set_launch_error(error, *reason);
        return false;
    }

#if defined(_WIN32)
    std::wstring wurl(url.begin(), url.end());
    // ShellExecuteW returns a value > 32 on success.
    auto rc = reinterpret_cast<INT_PTR>(
        ::ShellExecuteW(nullptr, L"open", wurl.c_str(),
                        nullptr, nullptr, SW_SHOWNORMAL));
    if (rc <= 32) {
        set_launch_error(error, "ShellExecuteW failed while launching editor URL");
        return false;
    }
    return true;
#elif defined(__APPLE__)
    const std::string url_str(url);
    const char* argv[] = {"open", url_str.c_str(), nullptr};
    pid_t pid = 0;
    int rc = ::posix_spawnp(&pid, "open", nullptr, nullptr,
                            const_cast<char* const*>(argv), environ);
    if (rc != 0) {
        set_launch_error(error, "posix_spawnp(open) failed while launching editor URL");
        return false;
    }
    if (!wait_for_launcher(pid, "open", error))
        return false;
    return true;
#else
    const std::string url_str(url);
    const char* argv[] = {"xdg-open", url_str.c_str(), nullptr};
    pid_t pid = 0;
    int rc = ::posix_spawnp(&pid, "xdg-open", nullptr, nullptr,
                            const_cast<char* const*>(argv), environ);
    if (rc != 0) {
        set_launch_error(error, "posix_spawnp(xdg-open) failed while launching editor URL");
        return false;
    }
    if (!wait_for_launcher(pid, "xdg-open", error))
        return false;
    return true;
#endif
}

SourceJumpResult jump_to_source(const InspectorConfig& config,
                                const pulp::view::View* view,
                                bool dry_run) {
    SourceJumpResult result = resolve_source_jump(config, view);
    if (result.ok && !dry_run) {
        std::string launch_error;
        result.launched = launch_editor_url(result.url, &launch_error);
        if (!result.launched && !launch_error.empty())
            result.error = std::move(launch_error);
    }
    return result;
}

} // namespace pulp::inspect
