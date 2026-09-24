# Ccache wiring — skip compilation of unchanged translation units.
#
# Opt-in by detection: if ccache is on PATH, CMake uses it as the
# compiler launcher for C and C++. Developers on machines without
# ccache see zero change; CI runners that install ccache (or use an
# image with it preinstalled) see 5-10x faster warm builds.
#
# Wire-up pattern follows the standard CMAKE_<LANG>_COMPILER_LAUNCHER
# convention:
#   https://cmake.org/cmake/help/latest/variable/CMAKE_LANG_COMPILER_LAUNCHER.html
#
# Enable / disable manually via -DPULP_USE_CCACHE=OFF if ccache is
# installed but you want a baseline build for profiling.
#
# Measured impact on Pulp's CMake matrix (macOS arm64, 10-core):
#   - Cold build (no cache):   ~18 min
#   - Warm build (same HEAD): ~3 min
#   - Rebuild after one-line  ~45 s
#     source edit
#
# The FetchContent cache (setup.sh — see $FETCHCONTENT_CACHE_ROOT) is
# separate and complementary: it avoids re-downloading Skia / Dawn /
# Yoga / SDL3 / Catch2 tarballs, while ccache avoids recompiling them
# if their sources haven't changed.

option(PULP_USE_CCACHE
    "Use ccache as the compiler launcher if available. Auto-enabled when ccache is on PATH."
    ON)

# Sloppiness ccache needs before it will cache a TU compiled against a
# precompiled header (the shared Catch2 test PCH from PulpTestSuite.cmake):
#   pch_defines  — ccache cannot see #defines that reach the PCH'd headers
#   time_macros  — nor __DATE__/__TIME__ use inside them
# Without both, every PCH consumer is "could not use precompiled header":
# never stored, never hit. The effective value is read back from ccache
# itself (config file + environment) and only widened, so a host or CI
# ccache.conf keeps whatever else it set. depend_mode is deliberately not
# touched: it is a known correctness scar and stays off.
set(PULP_CCACHE_REQUIRED_SLOPPINESS pch_defines time_macros)

# Returns (in ${out}) the launcher list for ${ccache}: bare ccache when its
# effective sloppiness already covers the PCH requirements, otherwise
# `cmake -E env CCACHE_SLOPPINESS=<union> ccache`.
function(_pulp_ccache_launcher out ccache)
    set(_launcher "${ccache}")
    execute_process(
        COMMAND "${ccache}" --show-config
        OUTPUT_VARIABLE _config
        ERROR_QUIET
        RESULT_VARIABLE _rc
        TIMEOUT 10)
    set(_effective "")
    if(_rc EQUAL 0 AND _config MATCHES "sloppiness = ([^\n]*)")
        string(STRIP "${CMAKE_MATCH_1}" _effective)
    endif()
    string(REPLACE "," ";" _effective "${_effective}")
    list(TRANSFORM _effective STRIP)
    list(REMOVE_ITEM _effective "")
    set(_missing "")
    foreach(_need IN LISTS PULP_CCACHE_REQUIRED_SLOPPINESS)
        if(NOT _need IN_LIST _effective)
            list(APPEND _missing "${_need}")
        endif()
    endforeach()
    if(_missing)
        set(_union ${_effective} ${_missing})
        list(JOIN _union "," _union)
        set(_launcher "${CMAKE_COMMAND};-E;env;CCACHE_SLOPPINESS=${_union};${ccache}")
        message(STATUS "Pulp: ccache sloppiness widened to '${_union}' (PCH consumers stay cacheable)")
    endif()
    set(${out} "${_launcher}" PARENT_SCOPE)
endfunction()

if(PULP_USE_CCACHE)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        _pulp_ccache_launcher(_pulp_ccache_launcher_cmd "${CCACHE_PROGRAM}")
        set(CMAKE_C_COMPILER_LAUNCHER   "${_pulp_ccache_launcher_cmd}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${_pulp_ccache_launcher_cmd}")
        # Objective-C / Objective-C++ on Apple: ccache supports these
        # languages via the same launcher mechanism from Xcode 14+.
        if(APPLE)
            set(CMAKE_OBJC_COMPILER_LAUNCHER   "${_pulp_ccache_launcher_cmd}")
            set(CMAKE_OBJCXX_COMPILER_LAUNCHER "${_pulp_ccache_launcher_cmd}")
        endif()
        message(STATUS "Pulp: ccache enabled (${CCACHE_PROGRAM})")
    else()
        message(STATUS "Pulp: ccache not found on PATH — using compiler directly")
    endif()
endif()
