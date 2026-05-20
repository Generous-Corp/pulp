// source_jump.cpp — Inspector source-jump implementation.
//
// See pulp/inspect/source_jump.hpp for the public surface and the
// design context (planning Phase 5.1).

#include <pulp/inspect/source_jump.hpp>
#include <pulp/view/view.hpp>

#include <cstdlib>

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

// Test / CI / non-interactive guard. When `PULP_INSPECTOR_NO_LAUNCH` is
// set to a non-empty value, `launch_editor_url()` resolves and formats
// the URL exactly as it would otherwise, but never spawns the editor
// process. This keeps headless test runs, CI, and any scripted /
// non-interactive use of the inspector from popping a real editor
// window (and, on macOS, the "an external application wants to open …"
// security dialog). A genuine user action — the J hotkey in an
// interactive session — runs without this env var and launches for
// real. Tests assert the *constructed* URL via `resolve_source_jump()`
// or the `dryRun` protocol path; they must never depend on a real
// launch.
bool editor_launch_suppressed() {
    const char* v = std::getenv("PULP_INSPECTOR_NO_LAUNCH");
    return v != nullptr && v[0] != '\0';
}

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

bool launch_editor_url(std::string_view url) {
    if (url.empty()) return false;

    // Defense in depth: refuse to spawn under the test/CI/non-interactive
    // guard. This is independent of the `dry_run` flag — even a caller
    // that asked to launch (the J hotkey wired with `dry_run=false`) is
    // suppressed when the env var is set, so a non-interactive run can
    // never pop a real editor or the macOS open-confirmation dialog.
    if (editor_launch_suppressed()) return false;

#if defined(_WIN32)
    std::wstring wurl(url.begin(), url.end());
    // ShellExecuteW returns a value > 32 on success.
    auto rc = reinterpret_cast<INT_PTR>(
        ::ShellExecuteW(nullptr, L"open", wurl.c_str(),
                        nullptr, nullptr, SW_SHOWNORMAL));
    return rc > 32;
#elif defined(__APPLE__)
    const std::string url_str(url);
    const char* argv[] = {"open", url_str.c_str(), nullptr};
    pid_t pid = 0;
    int rc = ::posix_spawnp(&pid, "open", nullptr, nullptr,
                            const_cast<char* const*>(argv), environ);
    if (rc != 0) return false;
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#else
    const std::string url_str(url);
    const char* argv[] = {"xdg-open", url_str.c_str(), nullptr};
    pid_t pid = 0;
    int rc = ::posix_spawnp(&pid, "xdg-open", nullptr, nullptr,
                            const_cast<char* const*>(argv), environ);
    if (rc != 0) return false;
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

SourceJumpResult jump_to_source(const InspectorConfig& config,
                                const pulp::view::View* view,
                                bool dry_run) {
    SourceJumpResult result = resolve_source_jump(config, view);
    if (result.ok && !dry_run) {
        result.launched = launch_editor_url(result.url);
    }
    return result;
}

} // namespace pulp::inspect
