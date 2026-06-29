#pragma once

/// @file reload_abi.hpp
/// The C-ABI contract a hot-reloadable "logic" library exports, and the macro a
/// plugin uses to export it (v2 plan §4.4 / Phase 1).
///
/// A reloadable plugin is split into a thin shell (owns the audio thread, the
/// StateStore, and the ProcessorHotSwapSlot) and a logic library (the DSP, built
/// as a shared object). The shell `dlopen`s the logic library and resolves these
/// C entry points:
///
///   - kAbiVersionSymbol — returns the reload-ABI version the logic was built
///     against (kReloadAbiVersion). Versions the ENTRY-POINT shape, distinct from
///     the build fingerprint (which versions the C++ ABI). The shell refuses a
///     library built against a different reload ABI before reading anything else.
///   - kFingerprintSymbol — fills a BuildFingerprint with the logic library's
///     own build settings, so the shell can refuse an ABI-incompatible image
///     BEFORE constructing anything across the seam. Plain C taking a POD
///     pointer — the one call made before C++-ABI compatibility is known.
///   - kCreateSymbol — constructs the logic Processor and returns an owning raw
///     pointer (the shell adopts it). Raw pointer, not unique_ptr, because
///     ownership crosses a C ABI boundary.
///   - kDestroySymbol — destroys a Processor created by kCreateSymbol, using the
///     logic image's own operator delete. RESERVED for the static-C++-runtime
///     case: today the shell deletes via its own runtime, which is safe only
///     because the fingerprint gate proves shell and logic share one C++ runtime
///     (same operator new/delete + heap). A logic library that static-links the
///     C++ runtime would have its OWN heap — the fingerprint does not catch that
///     — and the shell must then route deletion through this hook. See
///     reload_transaction.hpp's shared-runtime note.
///
/// `PULP_RELOAD_LOGIC(factory_expr)` exports all four from a logic translation
/// unit. The fingerprint is captured with the logic library's OWN compiler via
/// the header-inline current_build_fingerprint(), which is the whole point.

#include <pulp/format/processor.hpp>
#include <pulp/format/reload/build_fingerprint.hpp>

namespace pulp::format::reload {

/// Reload entry-point ABI version. Bump when the exported symbol set or their
/// signatures change (NOT for C++-ABI changes — that is the build fingerprint).
inline constexpr int kReloadAbiVersion = 1;

inline constexpr const char* kAbiVersionSymbol = "pulp_reload_abi_version";
inline constexpr const char* kFingerprintSymbol = "pulp_reload_fingerprint";
inline constexpr const char* kCreateSymbol = "pulp_reload_create";
inline constexpr const char* kDestroySymbol = "pulp_reload_destroy";

using ReloadAbiVersionFn = int (*)();
/// Fills *out with the logic library's own BuildFingerprint.
using ReloadFingerprintFn = void (*)(BuildFingerprint* out);
/// Constructs the logic Processor; the caller takes ownership of the result.
using ReloadCreateFn = Processor* (*)();
/// Destroys a Processor returned by ReloadCreateFn, via the logic image's
/// operator delete (reserved; see kDestroySymbol).
using ReloadDestroyFn = void (*)(Processor*);

} // namespace pulp::format::reload

#if defined(_WIN32)
#define PULP_RELOAD_EXPORT extern "C" __declspec(dllexport)
#else
#define PULP_RELOAD_EXPORT extern "C" __attribute__((visibility("default")))
#endif

/// Export the reload ABI from a logic translation unit. `factory_expr` must be a
/// callable (or expression) yielding a `pulp::format::Processor*` the caller
/// owns — typically `new MyProcessor()`.
///
/// The exported symbol NAMES below must stay in sync with the k*Symbol string
/// constants above (they are matched by name at dlsym time).
#define PULP_RELOAD_LOGIC(factory_expr)                                            \
    PULP_RELOAD_EXPORT int pulp_reload_abi_version() {                             \
        return ::pulp::format::reload::kReloadAbiVersion;                          \
    }                                                                              \
    PULP_RELOAD_EXPORT void pulp_reload_fingerprint(                               \
        ::pulp::format::reload::BuildFingerprint* out) {                           \
        if (out) *out = ::pulp::format::reload::current_build_fingerprint();       \
    }                                                                              \
    PULP_RELOAD_EXPORT ::pulp::format::Processor* pulp_reload_create() {           \
        return (factory_expr);                                                     \
    }                                                                              \
    PULP_RELOAD_EXPORT void pulp_reload_destroy(::pulp::format::Processor* p) {    \
        delete p;                                                                  \
    }
