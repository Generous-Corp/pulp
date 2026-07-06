#pragma once

/// @file reload_library.hpp
/// Dynamic-library handle for the DSP hot-reload path, with an explicit and
/// deliberately conservative leak policy (v2 plan §4.4 / Phase 0).
///
/// THE LEAK POLICY — why the default is to NOT unload:
///   Once a freshly-built logic library has been `dlopen`ed and a `Processor`
///   constructed from it, that instance's code, vtables, RTTI, inline functions,
///   thread-locals, and any registered atexit/static-destructor handlers all
///   live *inside the loaded image*. `dlclose`ing the image while any of that is
///   still reachable — even after the audio thread has been swapped off the old
///   processor — is a use-after-free waiting to happen: a queued UI callback, a
///   pending exception unwinding through the image, a TLS destructor, or a
///   std::function holding a lambda defined in the image will all jump into
///   freed pages. There is no portable way to *prove* an image is fully
///   quiesced. So a live reload session deliberately STACKS loaded images and
///   leaks their handles for the process lifetime (`LeakPolicy::Retain`, the
///   default). The leak is bounded — one handle per successful reload during a
///   dev session — and is the correct trade against an unload-race crash.
///
///   `LeakPolicy::CloseOnDestroy` is opt-in for callers that can guarantee the
///   image never contributed a still-live object: a scratch probe that only
///   read a build fingerprint / parameter contract and instantiated nothing, or
///   a teardown path in a test. Use it only when that guarantee holds.
///
/// The wrapper is intentionally dependency-light (mirrors the self-contained
/// shape of build_fingerprint.hpp) and carries its own minimal cross-platform
/// loader surface rather than depending on the host layer's dl_shim.hpp, so the
/// reload primitives stay loadable below the host.

#include <string>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace pulp::format::reload {

/// What the handle does with the loaded image when it is destroyed.
enum class LeakPolicy {
    Retain,         ///< Never unload (default). Safe for live reload — see file header.
    CloseOnDestroy  ///< dlclose on destruction. Only when the image is provably quiesced.
};

class ReloadLibrary {
public:
    ReloadLibrary() = default;

    /// Load @p path. On failure, valid() is false and error() explains why.
    /// The default policy retains (never unloads) the image — see file header.
    explicit ReloadLibrary(const std::string& path,
                           LeakPolicy policy = LeakPolicy::Retain)
        : path_(path), policy_(policy) {
        open(path);
    }

    // Move-only: a handle has unique ownership of the (un)load decision.
    ReloadLibrary(ReloadLibrary&& other) noexcept { move_from(other); }
    ReloadLibrary& operator=(ReloadLibrary&& other) noexcept {
        if (this != &other) {
            destroy();
            move_from(other);
        }
        return *this;
    }
    ReloadLibrary(const ReloadLibrary&) = delete;
    ReloadLibrary& operator=(const ReloadLibrary&) = delete;

    ~ReloadLibrary() { destroy(); }

    bool valid() const noexcept { return handle_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    const std::string& error() const noexcept { return error_; }
    const std::string& path() const noexcept { return path_; }
    void* native_handle() const noexcept { return handle_; }

    LeakPolicy leak_policy() const noexcept { return policy_; }
    /// Promote/demote the unload decision after construction (e.g. flip a probe
    /// to Retain once it is about to contribute a live processor).
    void set_leak_policy(LeakPolicy policy) noexcept { policy_ = policy; }

    /// Resolve a symbol. Returns nullptr if absent or the image isn't loaded.
    void* raw_symbol(const char* name) const noexcept {
        if (!handle_ || !name) return nullptr;
#if defined(_WIN32)
        return reinterpret_cast<void*>(
            ::GetProcAddress(reinterpret_cast<HMODULE>(handle_), name));
#else
        return ::dlsym(handle_, name);
#endif
    }

    /// Typed convenience over raw_symbol (typically a factory function pointer).
    template <typename T>
    T symbol(const char* name) const noexcept {
        return reinterpret_cast<T>(raw_symbol(name));
    }

    /// Explicitly unload now, regardless of policy. Returns true on success.
    /// ONLY call when the image is provably quiesced (see file header) — this
    /// is the unsafe escape hatch, used by tests and quiesced-probe teardown.
    bool close() noexcept {
        if (!handle_) return false;
        const bool ok = native_close(handle_);
        handle_ = nullptr;
        return ok;
    }

private:
    void open(const std::string& path) {
#if defined(_WIN32)
        DWORD old_mode = 0;
        const BOOL changed = ::SetThreadErrorMode(
            SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &old_mode);
        handle_ = reinterpret_cast<void*>(::LoadLibraryA(path.c_str()));
        if (changed) ::SetThreadErrorMode(old_mode, nullptr);
        if (!handle_) error_ = "LoadLibraryA failed for '" + path + "'";
#else
        handle_ = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle_) {
            const char* e = ::dlerror();
            error_ = e ? e : ("dlopen failed for '" + path + "'");
        }
#endif
    }

    static bool native_close(void* handle) noexcept {
#if defined(_WIN32)
        return ::FreeLibrary(reinterpret_cast<HMODULE>(handle)) != 0;
#else
        return ::dlclose(handle) == 0;
#endif
    }

    void destroy() noexcept {
        if (handle_ && policy_ == LeakPolicy::CloseOnDestroy) {
            native_close(handle_);
        }
        // LeakPolicy::Retain: intentionally leak the image for the process
        // lifetime (see file header). Drop our reference without unloading.
        handle_ = nullptr;
    }

    void move_from(ReloadLibrary& other) noexcept {
        handle_ = other.handle_;
        path_ = std::move(other.path_);
        error_ = std::move(other.error_);
        policy_ = other.policy_;
        other.handle_ = nullptr;  // moved-from never unloads
    }

    void* handle_ = nullptr;
    std::string path_;
    std::string error_;
    LeakPolicy policy_ = LeakPolicy::Retain;
};

} // namespace pulp::format::reload
