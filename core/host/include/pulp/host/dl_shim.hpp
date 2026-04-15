#pragma once

// Dynamic-loader shim for Windows/POSIX parity.
//
// The host plugin loaders (`plugin_slot_{clap,vst3,lv2}.cpp`,
// `scanner_clap.cpp`) use `<dlfcn.h>` to dlopen plugin bundles. Windows
// does not ship `<dlfcn.h>`; the release-cli build broke on every v0.4.0
// and later tag because of that. This header papers over the split by
// exposing a `dlopen` / `dlsym` / `dlclose` / `dlerror` surface that
// forwards to the platform-native loader.
//
// Scope is deliberately minimal: only the symbols the host loaders
// actually call. If callers need more of POSIX `<dlfcn.h>`, extend the
// Windows branch rather than reaching for a third-party port.

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// POSIX flag constants — no-ops on Windows, kept so call sites don't
// need #ifdefs at every use.
#ifndef RTLD_LAZY
#define RTLD_LAZY 0
#endif
#ifndef RTLD_LOCAL
#define RTLD_LOCAL 0
#endif
#ifndef RTLD_NOW
#define RTLD_NOW 0
#endif

namespace pulp::host::detail {

inline void* dl_open(const char* path, int /*flags*/) {
    // LoadLibraryA returns NULL on failure. Caller inspects return value.
    return reinterpret_cast<void*>(::LoadLibraryA(path));
}

inline void* dl_sym(void* handle, const char* name) {
    return reinterpret_cast<void*>(
        ::GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
}

inline int dl_close(void* handle) {
    // POSIX dlclose returns 0 on success; Windows FreeLibrary returns
    // non-zero on success. Normalize to POSIX.
    return ::FreeLibrary(reinterpret_cast<HMODULE>(handle)) ? 0 : -1;
}

inline const char* dl_error() {
    // Windows does not have a dlerror equivalent that returns a stable
    // thread-local string. Call sites already log the path; this stub
    // lets them keep compiling. Returning nullptr is safe — existing
    // call sites already guard with `err ? err : "unknown"`.
    return nullptr;
}

} // namespace pulp::host::detail

// POSIX-style entry points in the global namespace so call sites don't
// change.
inline void* dlopen(const char* path, int flags) {
    return ::pulp::host::detail::dl_open(path, flags);
}
inline void* dlsym(void* handle, const char* name) {
    return ::pulp::host::detail::dl_sym(handle, name);
}
inline int dlclose(void* handle) {
    return ::pulp::host::detail::dl_close(handle);
}
inline const char* dlerror() {
    return ::pulp::host::detail::dl_error();
}

#else // !_WIN32

#include <dlfcn.h>

#endif // _WIN32
