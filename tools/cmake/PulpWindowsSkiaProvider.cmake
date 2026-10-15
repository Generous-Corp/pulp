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
