// open_url_in_browser — a tiny, dependency-free system-browser opener for the
// activation handoff. Pulp has no core URL-open helper, so the example supplies
// one. It avoids a shell (no `system()`) to sidestep argument-injection: it
// hands the URL to the OS opener as a separate argv entry (posix_spawn) or via
// ShellExecute. Best-effort: returns false if the opener could not be launched.

#pragma once

#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#else
#include <spawn.h>
extern char** environ;
#endif

namespace moonbase_pulp {

inline bool open_url_in_browser(const std::string& url)
{
    // Only hand http(s) URLs to the OS opener. Besides rejecting unexpected
    // schemes, this refuses a value beginning with '-' (which `open`/`xdg-open`
    // would parse as an option) since neither prefix matches.
    const bool ok_scheme = url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0;
    if (!ok_scheme) return false;
#if defined(_WIN32)
    const HINSTANCE result =
        ::ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
#elif defined(__APPLE__)
    const char* argv[] = {"open", url.c_str(), nullptr};
    pid_t pid = 0;
    return ::posix_spawn(&pid, "/usr/bin/open", nullptr, nullptr,
                         const_cast<char* const*>(argv), environ) == 0;
#else
    const char* argv[] = {"xdg-open", url.c_str(), nullptr};
    pid_t pid = 0;
    return ::posix_spawnp(&pid, "xdg-open", nullptr, nullptr,
                          const_cast<char* const*>(argv), environ) == 0;
#endif
}

} // namespace moonbase_pulp
