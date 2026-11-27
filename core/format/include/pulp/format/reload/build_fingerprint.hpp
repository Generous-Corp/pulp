#pragma once

/// @file build_fingerprint.hpp
/// Strict build-compatibility fingerprint — the PRIMARY safety gate for DSP
/// hot reload. The shell and the hot-loaded logic dylib must be
/// built with a byte-identical toolchain + ABI configuration, because the seam
/// carries full C++ traffic (vtable dispatch, std:: containers, exceptions,
/// RTTI). A `sizeof`/layout probe cannot prove that; this fingerprint does, by
/// rejecting any logic binary whose compiler/stdlib/flags differ.
///
/// CRITICAL: `BuildFingerprint` is a fixed-size POD (no std::string). It is the
/// ONE struct that crosses the shell↔logic boundary *before* compatibility is
/// established, so it must not itself depend on the STL ABI it exists to check.
/// `current_build_fingerprint()` is header-inline + macro-driven so each binary
/// captures its OWN compiler's settings at its own compile time.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace pulp::format::reload {

/// Bump when the fingerprint layout/semantics change.
inline constexpr uint32_t kBuildFingerprintSchema = 2;

struct BuildFingerprint {
    uint32_t schema_version = 0;
    int32_t cpp_standard = 0;       // __cplusplus (_MSVC_LANG under MSVC)
    uint8_t sample_precision = 0;   // bits: 32 (float) / 64 (double)
    char compiler[64] = {};         // "clang 17.0.0" / "gcc 13.2.0" / "msvc 1939"
    char target[48] = {};           // "arm64-macos"
    char stdlib[48] = {};           // "libc++ 170000" / "libstdc++ 20230801"
    char abi_flags[160] = {};       // ndebug/exc/rtti/cxx11abi/itdbg/hardening/libcppabi/legacyview
    char sanitizers[48] = {};       // "asan,tsan" or "none"
    char sdk_version[48] = {};      // Pulp SDK version (shell+logic share a checkout)
};

static_assert(sizeof(BuildFingerprint) > 0);

namespace detail {
inline void set_field(char* dst, std::size_t cap, const char* src) {
    std::snprintf(dst, cap, "%s", src ? src : "");
}
} // namespace detail

/// Capture THIS translation unit's build fingerprint. Header-inline so the shell
/// and the logic dylib each record their own compiler's macros.
inline BuildFingerprint current_build_fingerprint() {
    BuildFingerprint fp{};
    fp.schema_version = kBuildFingerprintSchema;
#if defined(_MSVC_LANG)
    // MSVC reports __cplusplus as 199711 unless /Zc:__cplusplus is set;
    // _MSVC_LANG always carries the selected language standard.
    fp.cpp_standard = static_cast<int32_t>(_MSVC_LANG);
#else
    fp.cpp_standard = static_cast<int32_t>(__cplusplus);
#endif
    fp.sample_precision = static_cast<uint8_t>(sizeof(float) * 8);

#if defined(__clang__)
    std::snprintf(fp.compiler, sizeof fp.compiler, "clang %d.%d.%d",
                  __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(__GNUC__)
    std::snprintf(fp.compiler, sizeof fp.compiler, "gcc %d.%d.%d",
                  __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
    std::snprintf(fp.compiler, sizeof fp.compiler, "msvc %d", _MSC_VER);
#else
    detail::set_field(fp.compiler, sizeof fp.compiler, "unknown-compiler");
#endif

    {
#if defined(__aarch64__) || defined(_M_ARM64)
        const char* arch = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
        const char* arch = "x86_64";
#else
        const char* arch = "unknown-arch";
#endif
#if defined(__APPLE__)
        const char* os = "macos";
#elif defined(__linux__)
        const char* os = "linux";
#elif defined(_WIN32)
        const char* os = "windows";
#else
        const char* os = "unknown-os";
#endif
        std::snprintf(fp.target, sizeof fp.target, "%s-%s", arch, os);
    }

#if defined(_LIBCPP_VERSION)
    std::snprintf(fp.stdlib, sizeof fp.stdlib, "libc++ %d", _LIBCPP_VERSION);
#elif defined(__GLIBCXX__)
    std::snprintf(fp.stdlib, sizeof fp.stdlib, "libstdc++ %d", __GLIBCXX__);
#elif defined(_MSVC_STL_VERSION)
    std::snprintf(fp.stdlib, sizeof fp.stdlib, "msvcstl %d", _MSVC_STL_VERSION);
#else
    detail::set_field(fp.stdlib, sizeof fp.stdlib, "unknown-stdlib");
#endif

    {
#if defined(NDEBUG)
        const int ndebug = 1;
#else
        const int ndebug = 0;
#endif
#if defined(__EXCEPTIONS) || defined(_CPPUNWIND)
        const int exc = 1;
#else
        const int exc = 0;
#endif
#if defined(__GXX_RTTI) || defined(_CPPRTTI)
        const int rtti = 1;
#else
        const int rtti = 0;
#endif
#if defined(_GLIBCXX_USE_CXX11_ABI)
        const int cxx11abi = _GLIBCXX_USE_CXX11_ABI;
#else
        const int cxx11abi = -1;  // n/a (not libstdc++)
#endif
#if defined(_ITERATOR_DEBUG_LEVEL)
        const int itdbg = _ITERATOR_DEBUG_LEVEL;
#elif defined(_LIBCPP_DEBUG)
        const int itdbg = _LIBCPP_DEBUG;
#else
        const int itdbg = 0;
#endif
#if defined(_LIBCPP_HARDENING_MODE)
        const int hardening = _LIBCPP_HARDENING_MODE;
#elif defined(_GLIBCXX_ASSERTIONS)
        const int hardening = 1;
#else
        const int hardening = 0;
#endif
#if defined(_LIBCPP_ABI_VERSION)
        const int libcppabi = _LIBCPP_ABI_VERSION;
#else
        const int libcppabi = -1;
#endif
        // Processor's legacy view hooks keep their slots in both modes, but
        // under different names, so a logic library and a shell that disagree
        // on PULP_FORMAT_LEGACY_VIEW (processor.hpp) are not one build.
#if defined(PULP_FORMAT_LEGACY_VIEW) && !PULP_FORMAT_LEGACY_VIEW
        const int legacyview = 0;
#else
        const int legacyview = 1;
#endif
        std::snprintf(fp.abi_flags, sizeof fp.abi_flags,
                      "ndebug=%d;exc=%d;rtti=%d;cxx11abi=%d;itdbg=%d;harden=%d;libcppabi=%d;"
                      "legacyview=%d",
                      ndebug, exc, rtti, cxx11abi, itdbg, hardening, libcppabi, legacyview);
    }

    {
        char buf[48] = {};
        std::size_t n = 0;
        [[maybe_unused]] auto append = [&](const char* tag) {
            if (n) n += static_cast<std::size_t>(std::snprintf(buf + n, sizeof buf - n, ","));
            n += static_cast<std::size_t>(std::snprintf(buf + n, sizeof buf - n, "%s", tag));
        };
#if defined(__SANITIZE_ADDRESS__)
        append("asan");
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
        append("asan");
#  endif
#endif
#if defined(__has_feature)
#  if __has_feature(thread_sanitizer)
        append("tsan");
#  endif
#  if __has_feature(memory_sanitizer)
        append("msan");
#  endif
#endif
#if defined(__SANITIZE_THREAD__)
        append("tsan");
#endif
#if defined(UNDEFINED_BEHAVIOR_SANITIZER) || defined(__SANITIZE_UNDEFINED__)
        append("ubsan");
#endif
        detail::set_field(fp.sanitizers, sizeof fp.sanitizers, n ? buf : "none");
    }

#if defined(PULP_SDK_VERSION)
    detail::set_field(fp.sdk_version, sizeof fp.sdk_version, PULP_SDK_VERSION);
#else
    detail::set_field(fp.sdk_version, sizeof fp.sdk_version, "dev");
#endif

    return fp;
}

/// Field-by-field equality (the gate). Compares POD fields; ignores trailing
/// padding (so a struct memcmp can't false-fail on uninitialized pad bytes).
inline bool fingerprints_match(const BuildFingerprint& a, const BuildFingerprint& b) {
    return a.schema_version == b.schema_version &&
           a.cpp_standard == b.cpp_standard &&
           a.sample_precision == b.sample_precision &&
           std::strncmp(a.compiler, b.compiler, sizeof a.compiler) == 0 &&
           std::strncmp(a.target, b.target, sizeof a.target) == 0 &&
           std::strncmp(a.stdlib, b.stdlib, sizeof a.stdlib) == 0 &&
           std::strncmp(a.abi_flags, b.abi_flags, sizeof a.abi_flags) == 0 &&
           std::strncmp(a.sanitizers, b.sanitizers, sizeof a.sanitizers) == 0 &&
           std::strncmp(a.sdk_version, b.sdk_version, sizeof a.sdk_version) == 0;
}

/// Human-readable per-field differences (diagnostics only). Empty == identical.
inline std::vector<std::string> fingerprint_diff(const BuildFingerprint& shell,
                                                 const BuildFingerprint& logic) {
    std::vector<std::string> out;
    // Each field compares over its own capacity, as fingerprints_match does,
    // so a difference past the 64th character (the legacyview flag sits late
    // in abi_flags) is reported too.
    auto cmp_str = [&](const char* field, const char* s, const char* l, std::size_t capacity) {
        if (std::strncmp(s, l, capacity) != 0)
            out.push_back(std::string(field) + ": shell='" + std::string(s, ::strnlen(s, capacity)) +
                          "' logic='" + std::string(l, ::strnlen(l, capacity)) + "'");
    };
    auto cmp_int = [&](const char* field, long s, long l) {
        if (s != l)
            out.push_back(std::string(field) + ": shell=" + std::to_string(s) +
                          " logic=" + std::to_string(l));
    };
    cmp_int("schema_version", shell.schema_version, logic.schema_version);
    cmp_int("cpp_standard", shell.cpp_standard, logic.cpp_standard);
    cmp_int("sample_precision", shell.sample_precision, logic.sample_precision);
    cmp_str("compiler", shell.compiler, logic.compiler, sizeof shell.compiler);
    cmp_str("target", shell.target, logic.target, sizeof shell.target);
    cmp_str("stdlib", shell.stdlib, logic.stdlib, sizeof shell.stdlib);
    cmp_str("abi_flags", shell.abi_flags, logic.abi_flags, sizeof shell.abi_flags);
    cmp_str("sanitizers", shell.sanitizers, logic.sanitizers, sizeof shell.sanitizers);
    cmp_str("sdk_version", shell.sdk_version, logic.sdk_version, sizeof shell.sdk_version);
    return out;
}

} // namespace pulp::format::reload
