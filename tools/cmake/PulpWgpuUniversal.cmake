# PulpWgpuUniversal.cmake — synthesize a universal (arm64 + x86_64)
# libwgpu_native.dylib for a macOS universal build.
#
# WHY THIS EXISTS: wgpu-native (gfx-rs) publishes only THIN per-arch mac
# dylibs — there is no universal asset. WebGPU-distribution therefore creates
# the `webgpu` IMPORTED target from a single slice (the host arch, since Pulp
# leaves ARCH unset for a universal target — see PulpDependencies.cmake). For a
# -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" build that thin dylib links for only
# one slice and ld64 drops the other. This module fetches BOTH pinned slices,
# `lipo -create`s them into a fat dylib, ad-hoc re-signs it, and overrides the
# `webgpu` target's IMPORTED_LOCATION so link/copy/install all see the fat one.
#
# THE RE-SIGN IS NOT OPTIONAL. wgpu-native's per-slice dylibs each carry an
# adhoc linker signature; `lipo -create` does NOT merge those into a valid
# universal signature, so the raw fat output fails `codesign --verify` with
# "code object is not signed at all". An unsigned arm64 dylib is killed at load
# on Apple Silicon. `codesign -f -s -` produces one valid adhoc signature over
# the fat file (verified 2026-07-10 on the pinned v24.0.3.1 slices).
#
# LC_ID_DYLIB is already `@rpath/libwgpu_native.dylib` in both slices and the
# dylib carries no LC_RPATH, so the existing `@loader_path` consumer contract
# (PulpWebGpuImportedTarget.cmake / PulpBundleRelocatable.cmake) holds for the
# fat dylib unchanged.

include_guard(GLOBAL)

# Pinned wgpu-native mac slice digests. Keep in lockstep with the version
# passed to pulp_register_wgpu_native_precompiled_source() in
# PulpDependencies.cmake. Bumping wgpu-native means re-recording BOTH shas
# here (download the two macos zips and `shasum -a 256`).
set(_PULP_WGPU_UNIVERSAL_VERSION "v24.0.3.1")
set(_PULP_WGPU_UNIVERSAL_MIRROR "https://github.com/gfx-rs/wgpu-native")
set(_PULP_WGPU_UNIVERSAL_SHA_AARCH64
    "f140ff27234ebfa9fcca2b492d0cb499f2e197424b9edc45134bcbad0f8d3a78")
set(_PULP_WGPU_UNIVERSAL_SHA_X86_64
    "1fbc6930e2811b7fde7f046e5300ae5dc20c451d0c3e42a10ff71efae1f565ac")

# Fabricate the fat dylib and repoint the `webgpu` target at it.
#   version — the wgpu-native release the caller pinned; must match the pin
#             recorded above (a mismatch is a maintainer error, so FATAL).
function(pulp_make_wgpu_universal version)
    if(NOT APPLE)
        return()
    endif()
    # Only act for a genuine universal target.
    set(_want_arm64 FALSE)
    set(_want_x86_64 FALSE)
    if(CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
        set(_want_arm64 TRUE)
    endif()
    if(CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
        set(_want_x86_64 TRUE)
    endif()
    if(NOT (_want_arm64 AND _want_x86_64))
        return()
    endif()

    if(NOT TARGET webgpu)
        message(WARNING
            "PulpWgpuUniversal: universal mac target requested but the `webgpu` "
            "IMPORTED target does not exist — skipping fat-dylib synthesis. GPU "
            "may be disabled or wgpu fetch failed.")
        return()
    endif()

    if(NOT version STREQUAL _PULP_WGPU_UNIVERSAL_VERSION)
        message(FATAL_ERROR
            "PulpWgpuUniversal: pinned universal shas are for wgpu-native "
            "${_PULP_WGPU_UNIVERSAL_VERSION}, but the build pinned '${version}'. "
            "Re-record both macos slice sha256 digests in "
            "tools/cmake/PulpWgpuUniversal.cmake before building universal.")
    endif()

    set(_out_dir "${CMAKE_BINARY_DIR}/wgpu-universal")
    set(_fat "${_out_dir}/libwgpu_native.dylib")
    file(MAKE_DIRECTORY "${_out_dir}")

    # Fetch + extract each thin slice (sha256-verified) unless already present.
    set(_slice_dylibs "")
    foreach(_slice aarch64 x86_64)
        if(_slice STREQUAL "aarch64")
            set(_sha "${_PULP_WGPU_UNIVERSAL_SHA_AARCH64}")
        else()
            set(_sha "${_PULP_WGPU_UNIVERSAL_SHA_X86_64}")
        endif()
        set(_zip "${_out_dir}/wgpu-macos-${_slice}-release.zip")
        set(_extract "${_out_dir}/${_slice}")
        set(_dylib "${_extract}/lib/libwgpu_native.dylib")
        if(NOT EXISTS "${_dylib}")
            set(_url "${_PULP_WGPU_UNIVERSAL_MIRROR}/releases/download/${version}/wgpu-macos-${_slice}-release.zip")
            message(STATUS "PulpWgpuUniversal: fetching ${_url}")
            file(DOWNLOAD "${_url}" "${_zip}"
                EXPECTED_HASH SHA256=${_sha}
                STATUS _dl_status
                TLS_VERIFY ON)
            list(GET _dl_status 0 _dl_rc)
            if(NOT _dl_rc EQUAL 0)
                list(GET _dl_status 1 _dl_msg)
                message(FATAL_ERROR
                    "PulpWgpuUniversal: failed to download ${_url}: ${_dl_msg}")
            endif()
            file(ARCHIVE_EXTRACT INPUT "${_zip}" DESTINATION "${_extract}")
        endif()
        if(NOT EXISTS "${_dylib}")
            message(FATAL_ERROR
                "PulpWgpuUniversal: libwgpu_native.dylib not found in the "
                "${_slice} slice after extract (${_dylib}).")
        endif()
        list(APPEND _slice_dylibs "${_dylib}")
    endforeach()

    # lipo -create → ad-hoc re-sign (REQUIRED — see file header).
    find_program(_lipo NAMES lipo REQUIRED)
    find_program(_codesign NAMES codesign REQUIRED)
    execute_process(
        COMMAND "${_lipo}" -create ${_slice_dylibs} -output "${_fat}"
        RESULT_VARIABLE _lipo_rc
        ERROR_VARIABLE _lipo_err)
    if(NOT _lipo_rc EQUAL 0)
        message(FATAL_ERROR "PulpWgpuUniversal: lipo -create failed: ${_lipo_err}")
    endif()
    execute_process(
        COMMAND "${_codesign}" -f -s - "${_fat}"
        RESULT_VARIABLE _cs_rc
        ERROR_VARIABLE _cs_err)
    if(NOT _cs_rc EQUAL 0)
        message(FATAL_ERROR
            "PulpWgpuUniversal: ad-hoc codesign of the fat dylib failed "
            "(${_cs_err}). Without a valid signature the arm64 slice is killed "
            "at load.")
    endif()
    # Prove the result before we commit the build to it.
    execute_process(
        COMMAND "${_codesign}" --verify "${_fat}"
        RESULT_VARIABLE _csv_rc
        ERROR_VARIABLE _csv_err)
    if(NOT _csv_rc EQUAL 0)
        message(FATAL_ERROR
            "PulpWgpuUniversal: codesign --verify failed on the fat dylib "
            "after re-signing (${_csv_err}).")
    endif()

    # Repoint every consumer surface at the fat dylib: link (IMPORTED_LOCATION),
    # copy-next-to-binary + install (WEBGPU_RUNTIME_LIB).
    set_target_properties(webgpu PROPERTIES
        IMPORTED_LOCATION "${_fat}"
        IMPORTED_LOCATION_RELEASE "${_fat}")
    set(WEBGPU_RUNTIME_LIB "${_fat}" CACHE INTERNAL
        "Path to the WebGPU library binary (universal fat dylib)" FORCE)
    message(STATUS
        "PulpWgpuUniversal: universal libwgpu_native.dylib ready at ${_fat} "
        "(arm64 + x86_64, ad-hoc signed)")
endfunction()
