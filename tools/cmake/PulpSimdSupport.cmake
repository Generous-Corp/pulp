# PulpSimdSupport.cmake — target-architecture checks shared by SIMD setup and
# its configure-time contract test.

include_guard(GLOBAL)

# Return TRUE when the selected C++ compiler targets MSVC ARM64EC. CMake 3.10+
# reports ARM64EC through CMAKE_CXX_COMPILER_ARCHITECTURE_ID; the generator
# platform fallback covers toolchains that do not populate that identity (and
# keeps the check explicit about the target rather than the host processor).
function(pulp_simd_target_is_arm64ec out_var)
    set(_is_arm64ec FALSE)

    string(TOUPPER "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}" _compiler_arch)
    if(_compiler_arch STREQUAL "ARM64EC")
        set(_is_arm64ec TRUE)
    elseif(WIN32 AND CMAKE_GENERATOR_PLATFORM)
        string(TOUPPER "${CMAKE_GENERATOR_PLATFORM}" _generator_platform)
        if(_generator_platform MATCHES "(^|,)ARM64EC(,|$)")
            set(_is_arm64ec TRUE)
        endif()
    endif()

    set(${out_var} "${_is_arm64ec}" PARENT_SCOPE)
endfunction()
