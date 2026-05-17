# PulpSdkGuards.cmake — defensive guards for downstream find_package(Pulp).
#
# Extracted from PulpConfig.cmake.in in the 2026-05 B3 refactor. The
# guard block below preserves its original header text on purpose so
# the fixture `test/cmake/test_debug_sdk_guard.cmake` can locate it
# by its regex.

# Debug-SDK perf-killer guard (pulp-internal task #35).
#
# A Pulp SDK that was itself BUILT with CMAKE_BUILD_TYPE=Debug ships
# debug-mode Skia, debug-mode audio framework, and debug-mode JS
# engine. Consuming it from a downstream plugin produces real-time
# audio code so slow that buffer underruns happen on every reasonable
# host — a UX failure mode that's hard to attribute to "the SDK
# install is wrong" because the link succeeds and the plugin appears
# to work, just very poorly.
#
# The CMakeLists.txt that produced this SDK install writes the
# CMAKE_BUILD_TYPE it was configured with to
# `${PULP_SDK_DIR}/sdk_build_type.txt`. If we can read that file and
# it contains "Debug", refuse the find_package — with an escape hatch
# (`PULP_ALLOW_DEBUG_SDK=ON`) for the rare developer who *does* want
# a Debug SDK.
#
# Empty marker file (older SDK installs that predate the marker) and
# multi-config generators (which install per-config without recording
# a single canonical build-type) fall through silently — we cannot
# tell, so we don't warn.
set(_pulp_sdk_build_type_marker "${PULP_SDK_DIR}/sdk_build_type.txt")
if(EXISTS "${_pulp_sdk_build_type_marker}")
    file(READ "${_pulp_sdk_build_type_marker}" _pulp_sdk_build_type)
    string(STRIP "${_pulp_sdk_build_type}" _pulp_sdk_build_type)
    # CMake accepts CMAKE_BUILD_TYPE case-insensitively and preserves
    # the user's spelling on disk, so `-DCMAKE_BUILD_TYPE=debug` /
    # `DeBuG` are valid Debug builds whose marker won't STREQUAL
    # "Debug". Normalize to lowercase before comparing — otherwise
    # the guard silently no-ops on a real Debug install (Codex P1 on
    # PR #1828).
    string(TOLOWER "${_pulp_sdk_build_type}" _pulp_sdk_build_type_lc)
    if(_pulp_sdk_build_type_lc STREQUAL "debug")
        if(PULP_ALLOW_DEBUG_SDK)
            message(WARNING
                "Pulp SDK at '${PULP_SDK_DIR}' was built with "
                "CMAKE_BUILD_TYPE=Debug. PULP_ALLOW_DEBUG_SDK=ON — proceeding, but expect "
                "buffer underruns + slow paint on the resulting plugin. "
                "See task #35 / pulp-internal #36 for the path-forward "
                "plan to make a Debug SDK usable for data collection.")
        else()
            message(FATAL_ERROR
                "Pulp SDK at '${PULP_SDK_DIR}' was built with "
                "CMAKE_BUILD_TYPE=Debug. This SDK ships debug-mode "
                "Skia / audio framework / JS engine — plugins linked "
                "against it are NOT usable for real-time audio work "
                "(buffer underruns, audible glitches, multi-second "
                "paint hitches).\n"
                "\n"
                "Fix: rebuild the Pulp SDK with "
                "CMAKE_BUILD_TYPE=Release and reinstall:\n"
                "    cmake -S <pulp-src> -B build -DCMAKE_BUILD_TYPE=Release\n"
                "    cmake --build build --target install\n"
                "\n"
                "Or, if you genuinely want a Debug SDK (e.g. debugging "
                "Skia internals on a host that doesn't care about "
                "real-time), re-configure your consumer with "
                "-DPULP_ALLOW_DEBUG_SDK=ON to acknowledge the perf "
                "implications and proceed anyway.")
        endif()
    endif()
    unset(_pulp_sdk_build_type)
    unset(_pulp_sdk_build_type_lc)
endif()
unset(_pulp_sdk_build_type_marker)
