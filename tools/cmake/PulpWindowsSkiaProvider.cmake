# Windows ARM64EC/ARM64X Skia/Dawn provider policy.
#
# These targets have no compatible wgpu-native archive. Skia's matching Dawn
# archive is therefore mandatory and supplies the WebGPU feature definition.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/PulpWindowsTargetArch.cmake")

function(pulp_windows_skia_provider_target out_var)
    if(NOT WIN32)
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()
    pulp_windows_target_arch(_target)
    set(${out_var} "${_target}" PARENT_SCOPE)
endfunction()

function(pulp_windows_wgpu_prebuilt_arch out_var)
    pulp_windows_skia_provider_target(_target)
    if(_target MATCHES "^arm64(ec|x)-win$")
        message(FATAL_ERROR
            "Pulp: Windows ${_target} GPU configure is fail-closed. "
            "The pinned wgpu-native/Skia provider publishes ARM64 and x64 slices, "
            "but no ARM64EC/ARM64X-compatible provider.")
    elseif(_target STREQUAL "arm64-win")
        set(_arch "aarch64")
    elseif(_target STREQUAL "x86_64-win")
        set(_arch "x86_64")
    else()
        set(_arch "")
    endif()
    set(${out_var} "${_arch}" PARENT_SCOPE)
endfunction()

function(pulp_windows_skia_autofetch_slice out_platform out_suffix)
    pulp_windows_skia_provider_target(_target)
    if(_target STREQUAL "arm64-win")
        set(_platform "windows-arm64")
        set(_suffix "-arm64")
    elseif(_target STREQUAL "x86_64-win")
        set(_platform "windows-x64")
        set(_suffix "-x64")
    else()
        set(_platform "")
        set(_suffix "")
    endif()
    set(${out_platform} "${_platform}" PARENT_SCOPE)
    set(${out_suffix} "${_suffix}" PARENT_SCOPE)
endfunction()

# Resolve the Windows Skia auto-fetch inputs for the target architecture.
# <out_platform>/<out_suffix>: the auto-fetch matrix slice (ARM64, x64), or ""
# when the target has none. <out_staged>: for ARM64EC/ARM64X, which the matrix
# does not publish, the first skia-build root among ARGN that already holds the
# producer archive for that slice. Fails closed when such a target has no staged
# archive, and when the architecture is unsupported. The target architecture
# is computed here rather than read from a caller's variable.
function(pulp_windows_skia_autofetch_resolve out_platform out_suffix out_staged)
    pulp_windows_skia_provider_target(_target)
    pulp_windows_skia_autofetch_slice(_platform _suffix)
    set(_staged "")
    if(NOT _platform AND _target MATCHES "^arm64(ec|x)-win$")
        string(REGEX REPLACE "-win$" "" _slice "${_target}")
        foreach(_root IN LISTS ARGN)
            if(EXISTS "${_root}/build/win-gpu/lib/Release/${_slice}/skia.lib")
                set(_staged "${_root}")
                break()
            endif()
        endforeach()
        if(NOT _staged)
            message(FATAL_ERROR
                "Pulp: cannot auto-fetch Windows ${_slice} Skia. "
                "Stage a matching producer archive and pass -DSKIA_DIR, "
                "or install it under PULP_SDK_DIR/external/skia-build.")
        endif()
    elseif(NOT _platform)
        message(FATAL_ERROR
            "Pulp: cannot select a prebuilt Windows Skia slice for "
            "target architecture '${_target}'. Supported values "
            "are ARM64, ARM64EC, ARM64X, and x64.")
    endif()
    set(${out_platform} "${_platform}" PARENT_SCOPE)
    set(${out_suffix} "${_suffix}" PARENT_SCOPE)
    set(${out_staged} "${_staged}" PARENT_SCOPE)
endfunction()

# The checkout-local Windows Skia archive for the target architecture under the
# skia-build root <skia_build>, or "" when there is none. x64 also accepts the
# legacy flat Release/skia.lib layout.
function(pulp_windows_local_skia out_var skia_build)
    pulp_windows_skia_provider_target(_target)
    set(_dir "${skia_build}/build/win-gpu/lib/Release")
    set(_found "")
    if(_target STREQUAL "x86_64-win")
        file(GLOB _found "${_dir}/x64/skia.lib" "${_dir}/skia.lib")
    elseif(_target MATCHES "^arm64(ec|x)?-win$")
        string(REGEX REPLACE "-win$" "" _slice "${_target}")
        file(GLOB _found "${_dir}/${_slice}/skia.lib")
    endif()
    set(${out_var} "${_found}" PARENT_SCOPE)
endfunction()

function(pulp_windows_skia_provider_requires_skia_dawn target_arch out_var)
    if(target_arch MATCHES "^arm64(ec|x)-win$")
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(pulp_windows_skia_provider_require_dawn target_arch)
    if(target_arch MATCHES "^arm64(ec|x)-win$")
        if(NOT PULP_SKIA_PROVIDES_DAWN)
            message(FATAL_ERROR
                "Pulp: Windows ${target_arch} requires a matching Skia/Dawn archive. "
                "FindSkia resolved SKIA_FOUND=${SKIA_FOUND} "
                "and DAWN_LIBRARY='${DAWN_LIBRARY}'; refusing the incompatible "
                "wgpu-native fallback.")
        endif()
        set(PULP_HAS_WEBGPU TRUE CACHE INTERNAL
            "Pulp feature flag (Skia supplies Dawn)" FORCE)
    endif()
endfunction()

function(pulp_windows_skia_provider_should_declare_wgpu out_var)
    set(_declare FALSE)
    if(PULP_ENABLE_GPU AND NOT ANDROID AND NOT IOS)
        set(_declare TRUE)
        pulp_windows_skia_provider_target(_target)
        pulp_windows_skia_provider_requires_skia_dawn("${_target}" _requires_skia)
        if(_requires_skia)
            set(_declare FALSE)
        endif()
    endif()
    set(${out_var} ${_declare} PARENT_SCOPE)
endfunction()
