# FindV8.cmake — locate the pinned, sealed prebuilt V8 and expose a
# `v8::v8` IMPORTED target for the in-tree build.
#
# V8 is the optional JS engine backend selected with PULP_JS_ENGINE=v8
# (default is QuickJS; JSC is opt-in on Apple). The provider is the pinned sealed
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

if(DEFINED PULP_ROOT_DIR AND PULP_ROOT_DIR)
    set(_v8_pulp_root "${PULP_ROOT_DIR}")
else()
    get_filename_component(_v8_pulp_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
endif()

# ── Determine the platform key (matches manifest determinism.release_assets) ──
set(_v8_key "")
if(ANDROID)
    set(_v8_key "android-arm64")
elseif(APPLE AND NOT IOS)
    # A universal (arm64;x86_64) mac target has no universal libv8: v8-builder
    # publishes only thin mac-arm64 + mac-x86_64 dylibs. The old
    # `MATCHES "arm64"` test would silently resolve the thin arm64 dylib for a
    # universal build, and ld64 would drop the x86_64 slice at link. Fail loud
    # with the fix rather than ship a broken half-arch V8. (Follow-up: a
    # lipo'd universal libv8 à la PulpWgpuUniversal.cmake — deferred.)
    if(CMAKE_OSX_ARCHITECTURES MATCHES "arm64" AND CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
        message(FATAL_ERROR
            "FindV8: PULP_JS_ENGINE=v8 with a universal mac target "
            "(CMAKE_OSX_ARCHITECTURES='${CMAKE_OSX_ARCHITECTURES}') is not "
            "supported — v8-builder ships no universal libv8, only thin "
            "mac-arm64 and mac-x86_64 slices. Build V8 as two thin slices "
            "(-DCMAKE_OSX_ARCHITECTURES=arm64 and =x86_64 separately) and lipo "
            "the results, or select a different engine "
            "(-DPULP_JS_ENGINE=quickjs, or jsc on Apple) for the universal build.")
    endif()
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

# Load the active asset and aggregate-metadata receipts structurally. Non-legacy
# candidates (baked golden or checkout fetch) are accepted only when their
# extracted-file generation receipt re-hashes successfully against these pins.
set(_v8_expected_asset_sha "")
set(_v8_expected_metadata_sha "")
set(_v8_matched_milestone FALSE)
set(_v8_manifest_path "${_v8_pulp_root}/tools/deps/manifest.json")
if(_v8_key AND EXISTS "${_v8_manifest_path}")
    file(READ "${_v8_manifest_path}" _v8_manifest_json)
    string(JSON _v8_dep_count ERROR_VARIABLE _v8_json_error
        LENGTH "${_v8_manifest_json}" dependencies)
    if(_v8_json_error STREQUAL "NOTFOUND" AND _v8_dep_count GREATER 0)
        math(EXPR _v8_dep_last "${_v8_dep_count} - 1")
        foreach(_v8_dep_index RANGE 0 ${_v8_dep_last})
            string(JSON _v8_dep_name ERROR_VARIABLE _v8_name_error
                GET "${_v8_manifest_json}" dependencies ${_v8_dep_index} name)
            if(_v8_name_error STREQUAL "NOTFOUND" AND _v8_dep_name STREQUAL "V8")
                string(JSON _v8_expected_asset_sha ERROR_VARIABLE _v8_asset_error
                    GET "${_v8_manifest_json}" dependencies ${_v8_dep_index}
                    determinism release_assets "${_v8_key}" sha256)
                string(JSON _v8_expected_metadata_sha ERROR_VARIABLE _v8_metadata_error
                    GET "${_v8_manifest_json}" dependencies ${_v8_dep_index}
                    determinism release_metadata_sha256)
                string(JSON _v8_pair_kind ERROR_VARIABLE _v8_pair_kind_error
                    GET "${_v8_manifest_json}" dependencies ${_v8_dep_index}
                    determinism pair_kind)
                if(NOT _v8_asset_error STREQUAL "NOTFOUND")
                    set(_v8_expected_asset_sha "")
                endif()
                if(NOT _v8_metadata_error STREQUAL "NOTFOUND")
                    set(_v8_expected_metadata_sha "")
                endif()
                if(_v8_pair_kind_error STREQUAL "NOTFOUND"
                   AND _v8_pair_kind STREQUAL "chromium-milestone")
                    set(_v8_matched_milestone TRUE)
                endif()
                break()
            endif()
        endforeach()
    endif()
endif()

function(_pulp_v8_generation_valid root result)
    set(_valid FALSE)
    if(NOT _v8_expected_asset_sha
       OR (_v8_matched_milestone AND NOT _v8_expected_metadata_sha))
        set(${result} FALSE PARENT_SCOPE)
        return()
    endif()
    set(_asset_stamp "${root}/.v8-asset-sha256")
    set(_metadata_stamp "${root}/.v8-release-metadata-sha256")
    set(_receipt_path "${root}/.v8-generation-manifest.json")
    if(NOT EXISTS "${_asset_stamp}" OR NOT EXISTS "${_receipt_path}"
       OR (_v8_matched_milestone AND NOT EXISTS "${_metadata_stamp}"))
        set(${result} FALSE PARENT_SCOPE)
        return()
    endif()
    file(READ "${_asset_stamp}" _asset_stamp_value)
    string(STRIP "${_asset_stamp_value}" _asset_stamp_value)
    if(_v8_matched_milestone)
        file(READ "${_metadata_stamp}" _metadata_stamp_value)
        string(STRIP "${_metadata_stamp_value}" _metadata_stamp_value)
    endif()
    if(NOT _asset_stamp_value STREQUAL _v8_expected_asset_sha
       OR (_v8_matched_milestone
           AND NOT _metadata_stamp_value STREQUAL _v8_expected_metadata_sha))
        set(${result} FALSE PARENT_SCOPE)
        return()
    endif()
    file(READ "${_receipt_path}" _receipt_json)
    foreach(_field IN ITEMS schema platform asset_sha256 release_metadata_sha256)
        string(JSON _receipt_${_field} ERROR_VARIABLE _receipt_error
            GET "${_receipt_json}" ${_field})
        if(NOT _receipt_error STREQUAL "NOTFOUND")
            set(${result} FALSE PARENT_SCOPE)
            return()
        endif()
    endforeach()
    if(NOT _receipt_schema EQUAL 1 OR NOT _receipt_platform STREQUAL _v8_key
       OR NOT _receipt_asset_sha256 STREQUAL _v8_expected_asset_sha
       OR (_v8_matched_milestone
           AND NOT _receipt_release_metadata_sha256 STREQUAL _v8_expected_metadata_sha))
        set(${result} FALSE PARENT_SCOPE)
        return()
    endif()
    string(JSON _file_count ERROR_VARIABLE _files_error LENGTH "${_receipt_json}" files)
    if(NOT _files_error STREQUAL "NOTFOUND" OR _file_count LESS 1)
        set(${result} FALSE PARENT_SCOPE)
        return()
    endif()
    math(EXPR _file_last "${_file_count} - 1")
    foreach(_file_index RANGE 0 ${_file_last})
        string(JSON _relative ERROR_VARIABLE _path_error
            GET "${_receipt_json}" files ${_file_index} path)
        string(JSON _expected_size ERROR_VARIABLE _size_error
            GET "${_receipt_json}" files ${_file_index} size)
        string(JSON _expected_sha ERROR_VARIABLE _sha_error
            GET "${_receipt_json}" files ${_file_index} sha256)
        if(NOT _path_error STREQUAL "NOTFOUND" OR NOT _size_error STREQUAL "NOTFOUND"
           OR NOT _sha_error STREQUAL "NOTFOUND" OR NOT EXISTS "${root}/${_relative}")
            set(${result} FALSE PARENT_SCOPE)
            return()
        endif()
        file(SIZE "${root}/${_relative}" _actual_size)
        file(SHA256 "${root}/${_relative}" _actual_sha)
        if(NOT _actual_size EQUAL _expected_size OR NOT _actual_sha STREQUAL _expected_sha)
            set(${result} FALSE PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${result} TRUE PARENT_SCOPE)
endfunction()

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
    list(APPEND _v8_candidate_roots "${_v8_pulp_root}/external/v8-build/${_v8_key}")
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
        _pulp_v8_generation_valid("${_root}" _v8_candidate_valid)
        if(NOT _v8_candidate_valid)
            message(STATUS "FindV8: rejecting unverified or stale provider ${_root}")
            continue()
        endif()
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
