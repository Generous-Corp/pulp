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

if(PULP_USE_CCACHE)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER   "${CCACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
        # Objective-C / Objective-C++ on Apple: ccache supports these
        # languages via the same launcher mechanism from Xcode 14+.
        if(APPLE)
            set(CMAKE_OBJC_COMPILER_LAUNCHER   "${CCACHE_PROGRAM}")
            set(CMAKE_OBJCXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
        endif()
        message(STATUS "Pulp: ccache enabled (${CCACHE_PROGRAM})")
    else()
        message(STATUS "Pulp: ccache not found on PATH — using compiler directly")
    endif()
endif()
