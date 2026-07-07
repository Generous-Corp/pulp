# PulpV8Windows.cmake — Windows V8-consumer ABI contract for the sealed
# v8-builder drop-in.
#
# The sealed Windows v8.dll (v8-builder, v8-15.2.24-lkgr-97440bd4f523) is built — by deliberate,
# documented decision in v8-builder/build-v8.py:win_gn_args() — with:
#
#   * v8_enable_pointer_compression = true   (Windows-only; mac/linux are OFF)
#   * use_custom_libcxx              = true   (Chromium's BUNDLED libc++, the
#                                              __Cr ABI namespace)
#   * v8_enable_sandbox              = false
#   * use_rtti                       = false
#
# Consequences for the consumer, all VERIFIED on the Win11-24H2 arm64 QEMU golden
# (host arm64; x64 V8 + x64 consumer run under Windows-ARM x64 emulation):
#
#  1. v8.dll.lib exports its std::-bearing API surface mangled into the Chromium
#     libc++ ABI namespace — e.g. ...V?$shared_ptr@...@__Cr@std@@... (302 such
#     exports / 756 __Cr references). A consumer built with MSVC cl + the MSVC
#     STL emits plain `@std@@` symbols that never resolve against those `__Cr@std`
#     imports → unresolved-external link failure (observed:
#     `lld-link: error: undefined symbol: ... std::unique_ptr<v8::Platform> ...
#     v8::platform::NewDefaultPlatform(...)`). So the consumer MUST build with
#     clang-cl + Chromium-style libc++ (_LIBCPP_ABI_NAMESPACE=__Cr).
#
#  2. The inline header arithmetic in v8-internal.h (kApiTaggedSize) depends on
#     V8_COMPRESS_POINTERS; without it the consumer computes 8-byte tagged
#     fields against a 4-byte-tagged DLL and silently corrupts the heap.
#
#  3. v8.dll exports NONE of the out-of-line libc++ runtime (no std::cout, no
#     basic_ostream/basic_string ctors/dtors, no operator new). A consumer that
#     instantiates anything needing the libc++ runtime (choc uses std::cout /
#     std::endl / std::ostringstream) must link its OWN libc++.lib built with the
#     SAME __Cr ABI. The official Windows LLVM package ships clang-cl but NO
#     libc++, so that runtime must be supplied (built from the llvm-project
#     `runtimes` tree: LLVM_ENABLE_RUNTIMES=libcxx, LIBCXX_ABI_NAMESPACE=__Cr,
#     LIBCXX_ABI_VERSION=2, LIBCXX_CXX_ABI=vcruntime, RTTI ON / exceptions ON).
#
# Proof: a consumer that includes the real choc V8 wrapper
# (choc_javascript_V8.h + Console) built with the flags below links the sealed
# v8.dll.lib + a __Cr-ABI libc++.lib and runs —
#   V8::GetVersion() = 15.2.24 / evaluateExpression('40 + 2') = 42 / PASS.
#
# This module is a NO-OP off Windows.

# ── Provisioning knobs (Windows-only) ────────────────────────────────────────
#   PULP_V8_WIN_LIBCXX_INCLUDE — Chromium-style libc++ header root (dir with
#                                <vector>, <__config>). e.g. an llvm-project
#                                checkout's libcxx/include.
#   PULP_V8_WIN_LIBCXX_CONFIG  — dir holding __config_site (+ __assertion_handler)
#                                pinning _LIBCPP_ABI_NAMESPACE=__Cr. Searched
#                                BEFORE the header root. Auto-generated into the
#                                build tree when empty.
#   PULP_V8_WIN_LIBCXX_LIB     — the __Cr-ABI libc++.lib to link for the
#                                out-of-line libc++ runtime (REQUIRED for the
#                                choc consumer; see point 3 above).
set(PULP_V8_WIN_LIBCXX_INCLUDE "" CACHE PATH
    "Chromium-style libc++ header root for the Windows V8 consumer (dir with <vector>, <__config>)")
set(PULP_V8_WIN_LIBCXX_CONFIG "" CACHE PATH
    "Dir with __config_site + __assertion_handler pinning _LIBCPP_ABI_NAMESPACE=__Cr (auto-generated when empty)")
set(PULP_V8_WIN_LIBCXX_LIB "" CACHE FILEPATH
    "__Cr-ABI libc++.lib supplying the out-of-line libc++ runtime for the Windows V8 consumer")

# Guard: the V8 win consumer requires the clang-cl driver. clang-cl reports
# CMAKE_CXX_COMPILER_ID=Clang with CMAKE_CXX_COMPILER_FRONTEND_VARIANT=MSVC;
# plain MSVC cl reports ID=MSVC. Refuse to silently link the __Cr import lib with
# MSVC cl — that is the documented ABI mismatch.
function(pulp_v8_windows_require_clang_cl)
    if(NOT WIN32)
        return()
    endif()
    set(_is_clang_cl OFF)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang"
            AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        set(_is_clang_cl ON)
    endif()
    if(NOT _is_clang_cl)
        message(FATAL_ERROR
            "Pulp Windows V8: the sealed v8-builder v8.dll exposes the Chromium "
            "libc++ (__Cr) ABI, which can only be consumed by clang-cl + that "
            "libc++ — NOT MSVC cl + MSVC STL. The active C++ compiler is "
            "'${CMAKE_CXX_COMPILER_ID}' (frontend "
            "'${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}'). Configure with a "
            "clang-cl toolchain, e.g.:\n"
            "  cmake -S . -B build -G Ninja \\\n"
            "    -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl \\\n"
            "    -DPULP_JS_ENGINE=v8 -DPULP_V8BUILDER_ROOT=<unpacked v8-win-x64> \\\n"
            "    -DPULP_V8_WIN_LIBCXX_INCLUDE=<llvm-project>/libcxx/include \\\n"
            "    -DPULP_V8_WIN_LIBCXX_LIB=<__Cr libc++.lib>\n"
            "See tools/cmake/PulpV8Windows.cmake for the full ABI rationale.")
    endif()
endfunction()

# Generate a __config_site + __assertion_handler into <build>/v8-win-libcxx-cfg
# pinning the Chromium libc++ ABI knobs, and return that dir. The official
# Windows LLVM libc++ headers expect these CMake-generated files; without them
# libc++ refuses to compile ("__config_site file not found",
# "_LIBCPP_ASSERTION_SEMANTIC_DEFAULT is not defined") and, critically,
# defaults _LIBCPP_HAS_LOCALIZATION to 0 — which silently drops <ostream>/
# <sstream>/std::endl and breaks choc.
function(_pulp_v8_windows_generate_libcxx_config out_dir_var)
    set(_cfg_dir "${CMAKE_BINARY_DIR}/v8-win-libcxx-cfg")
    file(MAKE_DIRECTORY "${_cfg_dir}")
    file(WRITE "${_cfg_dir}/__config_site"
"// Generated by Pulp (PulpV8Windows.cmake) — Windows V8-consumer libc++ ABI.
// Pins the Chromium __Cr ABI namespace so std:: in this consumer mangles
// IDENTICALLY to the sealed v8.dll exports, and turns on the libc++ feature
// switches choc's headers depend on (localization → <ostream>/<sstream>).
// The official Windows LLVM package ships no libc++ and no __config_site, so we
// generate one. Prefer supplying the build-time-generated __config_site from a
// real libc++ build via PULP_V8_WIN_LIBCXX_CONFIG when available.
#ifndef _LIBCPP___CONFIG_SITE
#define _LIBCPP___CONFIG_SITE
#define _LIBCPP_ABI_VERSION 2
#define _LIBCPP_ABI_NAMESPACE __Cr
#define _LIBCPP_ASSERTION_SEMANTIC_DEFAULT _LIBCPP_ASSERTION_SEMANTIC_IGNORE
#define _LIBCPP_HARDENING_MODE_DEFAULT _LIBCPP_HARDENING_MODE_NONE
#define _LIBCPP_HAS_LOCALIZATION 1
#define _LIBCPP_HAS_THREAD_API_WIN32 1
#define _LIBCPP_HAS_FILESYSTEM 1
#define _LIBCPP_HAS_RANDOM_DEVICE 1
#define _LIBCPP_HAS_UNICODE 1
#define _LIBCPP_HAS_WIDE_CHARACTERS 1
#define _LIBCPP_HAS_TIME_ZONE_DATABASE 1
#define _LIBCPP_HAS_MONOTONIC_CLOCK 1
#define _LIBCPP_HAS_TERMINAL 1
#define _LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS 0
#endif // _LIBCPP___CONFIG_SITE
")
    file(WRITE "${_cfg_dir}/__assertion_handler"
"// Generated by Pulp (PulpV8Windows.cmake) — hardening OFF, no-op handler.
#ifndef _LIBCPP___ASSERTION_HANDLER
#define _LIBCPP___ASSERTION_HANDLER
#include <__config>
#define _LIBCPP_ASSERTION_HANDLER(message) ((void)0)
#endif // _LIBCPP___ASSERTION_HANDLER
")
    set(${out_dir_var} "${_cfg_dir}" PARENT_SCOPE)
endfunction()

# Apply the Windows V8-consumer ABI flags to the target that compiles
# src/js_v8_engine.cpp (pulp-view-script). NO-OP off Windows.
function(pulp_v8_windows_apply_abi target)
    if(NOT WIN32)
        return()
    endif()

    pulp_v8_windows_require_clang_cl()

    if(NOT PULP_V8_WIN_LIBCXX_INCLUDE)
        message(FATAL_ERROR
            "Pulp Windows V8: PULP_V8_WIN_LIBCXX_INCLUDE is unset. The official "
            "Windows LLVM package ships clang-cl but no libc++ headers; point "
            "this at a Chromium-style libc++ header root (e.g. an llvm-project "
            "checkout's libcxx/include).")
    endif()
    if(NOT EXISTS "${PULP_V8_WIN_LIBCXX_INCLUDE}/__config")
        message(FATAL_ERROR
            "Pulp Windows V8: PULP_V8_WIN_LIBCXX_INCLUDE="
            "'${PULP_V8_WIN_LIBCXX_INCLUDE}' has no <__config> — not a libc++ "
            "header root.")
    endif()
    if(NOT PULP_V8_WIN_LIBCXX_LIB)
        message(FATAL_ERROR
            "Pulp Windows V8: PULP_V8_WIN_LIBCXX_LIB is unset. The sealed v8.dll "
            "exports no out-of-line libc++ runtime; supply a __Cr-ABI libc++.lib "
            "(built from llvm-project/runtimes with LIBCXX_ABI_NAMESPACE=__Cr).")
    endif()

    set(_cfg_dir "${PULP_V8_WIN_LIBCXX_CONFIG}")
    if(NOT _cfg_dir)
        _pulp_v8_windows_generate_libcxx_config(_cfg_dir)
    endif()

    # ── Compile flags ────────────────────────────────────────────────────────
    # Route libc++ header dirs through clang-cl's /clang: escape — clang-cl
    # silently DROPS bare -nostdinc++/-isystem, and that drop is exactly what
    # makes a build fall back to the MSVC STL and fail to link. The config dir is
    # searched first so libc++'s `#include <__config_site>` resolves to the
    # __Cr-pinned one. /GR- matches the use_rtti=false sealed build.
    target_compile_options(${target} PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:/clang:-nostdinc++>"
        "$<$<COMPILE_LANGUAGE:CXX>:/clang:-isystem>"
        "$<$<COMPILE_LANGUAGE:CXX>:/clang:${_cfg_dir}>"
        "$<$<COMPILE_LANGUAGE:CXX>:/clang:-isystem>"
        "$<$<COMPILE_LANGUAGE:CXX>:/clang:${PULP_V8_WIN_LIBCXX_INCLUDE}>"
        "$<$<COMPILE_LANGUAGE:CXX>:/GR->")

    target_compile_definitions(${target} PRIVATE
        V8_COMPRESS_POINTERS
        _LIBCPP_DISABLE_AVAILABILITY
        # Suppress libc++'s `#pragma comment(lib,"c++")` autolink — we link the
        # __Cr libc++.lib explicitly below; the default autolink would pull a
        # mismatched system libc++.
        _LIBCPP_NO_AUTO_LINK
        # Match the CRT wide-stdio specifier the libc++.lib was built with
        # (LIBCXX standalone builds bake _CRT_STDIO_ISO_WIDE_SPECIFIERS=1); a
        # mismatch trips lld-link /FAILIFMISMATCH.
        _CRT_STDIO_ISO_WIDE_SPECIFIERS=1)
    # Do NOT define V8_ENABLE_SANDBOX — the sealed build is sandbox OFF.

    # ── Link the libc++ runtime ──────────────────────────────────────────────
    # msvcprt.lib supplies the MSVC __ExceptionPtr* glue that the vcruntime-ABI
    # libc++ exception_ptr references (we use the libc++ HEADERS but the MS C++
    # ABI for exceptions, per LIBCXX_CXX_ABI=vcruntime).
    target_link_libraries(${target} PRIVATE
        "${PULP_V8_WIN_LIBCXX_LIB}"
        msvcprt.lib)

    message(STATUS
        "Pulp Windows V8: applied clang-cl + Chromium libc++ (__Cr) consumer "
        "ABI to '${target}' (V8_COMPRESS_POINTERS; libc++ headers "
        "${PULP_V8_WIN_LIBCXX_INCLUDE}; config ${_cfg_dir}; runtime "
        "${PULP_V8_WIN_LIBCXX_LIB}).")
endfunction()
