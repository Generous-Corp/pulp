# Shared Windows ARM64EC compiler contract.
#
# ARM64X is a mixed-image format. It can contain native ARM64 translation
# units as well as ARM64EC translation units, so it must never be treated as a
# project-wide ARM64EC build. This module recognizes only a true ARM64EC
# compiler/target and applies the flag to both C and C++ sources so C-only
# dependencies cannot silently remain native ARM64.

include_guard(GLOBAL)

function(pulp_windows_target_is_arm64ec out_var)
    set(_is_arm64ec FALSE)
    string(TOUPPER "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}" _compiler_arch)
    string(TOUPPER "${CMAKE_GENERATOR_PLATFORM}" _generator_platform)
    if(_compiler_arch STREQUAL "ARM64EC"
            OR _generator_platform MATCHES "(^|[,;])ARM64EC([,;]|$)"
            OR CMAKE_CXX_FLAGS MATCHES "(^|[ ;])/arm64EC([ ;]|$)")
        set(_is_arm64ec TRUE)
    endif()
    set(${out_var} "${_is_arm64ec}" PARENT_SCOPE)
endfunction()

function(pulp_windows_apply_arm64ec_flags)
    if(NOT WIN32 AND NOT PULP_WINDOWS_ARM64EC_CONTRACT_TEST)
        return()
    endif()
    pulp_windows_target_is_arm64ec(_is_arm64ec)
    if(NOT _is_arm64ec)
        return()
    endif()
    foreach(_flag_var CMAKE_C_FLAGS CMAKE_CXX_FLAGS)
        if(NOT "${${_flag_var}}" MATCHES "(^|[ ;])/arm64EC([ ;]|$)")
            string(APPEND ${_flag_var} " /arm64EC")
            set(${_flag_var} "${${_flag_var}}" CACHE STRING
                "ARM64EC compile flags" FORCE)
        endif()
        set(${_flag_var} "${${_flag_var}}" PARENT_SCOPE)
    endforeach()
endfunction()
