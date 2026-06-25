# FindV8.cmake — locate the pinned, sealed prebuilt V8 and expose a
# `v8::v8` IMPORTED target for the in-tree build.
#
# V8 is the optional JS engine backend selected with PULP_JS_ENGINE=v8
# (default is QuickJS; JSC on Apple). The provider is the pinned sealed
# `libv8` from the danielraffel/v8-builder fork, fetched by
# tools/scripts/fetch_v8_for_release.py into external/v8-build/<platform>/.
# This replaces the former Homebrew-`libnode` provider search.
#
# Resolution order (first hit with include/v8.h wins):
#   1. Legacy explicit override: V8_INCLUDE_DIR + V8_LIB_DIR (local experiments)
#   2. $V8_DIR/<platform-key>           (baked golden VM: V8_DIR=~/pulp-v8-build)
#   3. $V8_DIR                          (V8_DIR points straight at a platform dir)
#   4. <repo>/external/v8-build/<platform-key>   (fetch_v8_for_release.py default)
#
# Outputs:
#   PULP_V8_FOUND        — TRUE if headers + library resolved
#   v8::v8               — IMPORTED target (includes + runtime lib + win implib)
#   V8_RUNTIME_LIBRARY   — the .dylib/.so/.dll to ship next to consumers
#   V8_IMPORT_LIBRARY    — the .dll.lib (Windows only)
#
# NOTE on rpath: this module deliberately does NOT set INSTALL_RPATH. The
# in-tree build resolves @rpath/libv8.dylib (and $ORIGIN on Linux) via CMake's
# default build-tree rpath because IMPORTED_LOCATION's directory is on the link
# path. Packaging/signing for shipped artifacts is owned by packaging helpers
# (PulpV8ImportedTarget.cmake + a target_copy_v8_binaries helper), mirroring
# how PulpWebGpuImportedTarget.cmake handles wgpu — see the migration plan.

include_guard(GLOBAL)

# ── Determine the platform key (matches manifest determinism.release_assets) ──
set(_v8_key "")
if(ANDROID)
    set(_v8_key "android-arm64")
elseif(APPLE AND NOT IOS)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64" OR CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
        set(_v8_key "mac-arm64")
    else()
        set(_v8_key "mac-x86_64")
    endif()
elseif(WIN32)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
        set(_v8_key "win-arm64")
    else()
        set(_v8_key "win-x64")
    endif()
elseif(UNIX AND NOT APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(_v8_key "linux-arm64")
    else()
        set(_v8_key "linux-x64")
    endif()
endif()

# ── Build the candidate root list ────────────────────────────────────────────
set(_v8_candidate_roots "")

# (1) Legacy explicit override — keep working for local experiments.
if(DEFINED V8_INCLUDE_DIR AND DEFINED V8_LIB_DIR)
    # Synthesize a root only used to short-circuit below; handled specially.
    set(_v8_legacy_override TRUE)
else()
    set(_v8_legacy_override FALSE)
endif()

set(_v8_env_dir "$ENV{V8_DIR}")
if(_v8_env_dir AND _v8_key)
    list(APPEND _v8_candidate_roots "${_v8_env_dir}/${_v8_key}")
endif()
if(_v8_env_dir)
    list(APPEND _v8_candidate_roots "${_v8_env_dir}")
endif()
if(DEFINED V8_DIR AND V8_DIR)
    if(_v8_key)
        list(APPEND _v8_candidate_roots "${V8_DIR}/${_v8_key}")
    endif()
    list(APPEND _v8_candidate_roots "${V8_DIR}")
endif()
if(_v8_key)
    list(APPEND _v8_candidate_roots "${CMAKE_SOURCE_DIR}/external/v8-build/${_v8_key}")
endif()

# ── Resolve include dir + library ────────────────────────────────────────────
set(_v8_inc "")
set(_v8_lib "")
set(_v8_implib "")

# Per-platform library file names.
if(WIN32)
    set(_v8_runtime_name "v8.dll")
    set(_v8_implib_name "v8.dll.lib")
elseif(APPLE)
    set(_v8_runtime_name "libv8.dylib")
elseif(ANDROID)
    set(_v8_runtime_name "libv8.so")
else()
    set(_v8_runtime_name "libv8.so")
endif()

if(_v8_legacy_override)
    if(EXISTS "${V8_INCLUDE_DIR}/v8.h")
        set(_v8_inc "${V8_INCLUDE_DIR}")
    endif()
    # Honor an explicit V8_LIBRARY_PATH too (the old direct-path override).
    if(DEFINED V8_LIBRARY_PATH AND EXISTS "${V8_LIBRARY_PATH}")
        set(_v8_lib "${V8_LIBRARY_PATH}")
    elseif(EXISTS "${V8_LIB_DIR}/${_v8_runtime_name}")
        set(_v8_lib "${V8_LIB_DIR}/${_v8_runtime_name}")
    endif()
    if(WIN32 AND EXISTS "${V8_LIB_DIR}/${_v8_implib_name}")
        set(_v8_implib "${V8_LIB_DIR}/${_v8_implib_name}")
    endif()
else()
    foreach(_root IN LISTS _v8_candidate_roots)
        if(NOT EXISTS "${_root}/include/v8.h")
            continue()
        endif()
        # Library lives under lib/ (mac/linux/win) or jniLibs/arm64-v8a/ (android).
        set(_cand_lib "")
        if(ANDROID)
            set(_cand_lib "${_root}/jniLibs/arm64-v8a/${_v8_runtime_name}")
        else()
            set(_cand_lib "${_root}/lib/${_v8_runtime_name}")
        endif()
        if(EXISTS "${_cand_lib}")
            set(_v8_inc "${_root}/include")
            set(_v8_lib "${_cand_lib}")
            if(WIN32 AND EXISTS "${_root}/lib/${_v8_implib_name}")
                set(_v8_implib "${_root}/lib/${_v8_implib_name}")
            endif()
            break()
        endif()
    endforeach()
endif()

# ── Result + imported target ─────────────────────────────────────────────────
if(_v8_inc AND _v8_lib AND (NOT WIN32 OR _v8_implib))
    set(PULP_V8_FOUND TRUE)
    set(V8_RUNTIME_LIBRARY "${_v8_lib}" CACHE FILEPATH "Resolved V8 runtime library" FORCE)
    if(_v8_implib)
        set(V8_IMPORT_LIBRARY "${_v8_implib}" CACHE FILEPATH "Resolved V8 import library (Windows)" FORCE)
    endif()

    if(NOT TARGET v8::v8)
        add_library(v8::v8 SHARED IMPORTED GLOBAL)
        set_target_properties(v8::v8 PROPERTIES
            IMPORTED_LOCATION "${_v8_lib}"
            INTERFACE_INCLUDE_DIRECTORIES "${_v8_inc}"
        )
        if(WIN32)
            set_property(TARGET v8::v8 PROPERTY IMPORTED_IMPLIB "${_v8_implib}")
        elseif(UNIX AND NOT APPLE)
            set_property(TARGET v8::v8 PROPERTY IMPORTED_NO_SONAME TRUE)
        endif()
    endif()

    message(STATUS "Pulp V8 provider: ${_v8_lib} (platform key: ${_v8_key})")
else()
    set(PULP_V8_FOUND FALSE)
endif()
