# Canonical Windows target architecture names used by installed bundles and
# build receipts.  CMAKE_SYSTEM_PROCESSOR can describe the host when CMake is
# cross-compiling with Visual Studio, so prefer the selected target platform.

include_guard(GLOBAL)

function(pulp_windows_target_arch out_var)
    if(NOT WIN32)
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()

    string(TOUPPER "${CMAKE_GENERATOR_PLATFORM}" _platform)
    string(TOUPPER "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}" _compiler_arch)
    if(_platform MATCHES "(^|,)ARM64EC(,|$)")
        set(_arch "arm64ec-win")
    elseif(_platform MATCHES "(^|,)ARM64X(,|$)")
        set(_arch "arm64x-win")
    elseif(_platform MATCHES "(^|,)ARM64(,|$)")
        set(_arch "arm64-win")
    elseif(_platform MATCHES "(^|,)(X64|AMD64)(,|$)")
        set(_arch "x86_64-win")
    elseif(_compiler_arch STREQUAL "ARM64EC")
        set(_arch "arm64ec-win")
    elseif(_compiler_arch STREQUAL "ARM64X")
        set(_arch "arm64x-win")
    elseif(_compiler_arch STREQUAL "ARM64"
            OR CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
        set(_arch "arm64-win")
    elseif(_compiler_arch MATCHES "^(X64|AMD64)$"
            OR CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|x64)$")
        set(_arch "x86_64-win")
    else()
        set(_arch "x86-win")
    endif()
    set(${out_var} "${_arch}" PARENT_SCOPE)
endfunction()
