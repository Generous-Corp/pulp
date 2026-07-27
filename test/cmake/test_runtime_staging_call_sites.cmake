cmake_minimum_required(VERSION 3.24)

# SOURCE LINT — every format helper that produces a loadable module or app must
# call pulp_stage_runtime_dependencies() (WAH-11).
#
# Deliberately labelled a lint, not a proof. It greps helper text, exactly like
# the ICU sidecar check it sits beside, and it inherits that check's limits: it
# cannot tell you the call reached a target or that a file landed. The
# behavioral proof is cmake-installed-sdk-runtime-staging, which builds a real
# consumer against a real installed SDK.
#
# It exists because a missing call is otherwise invisible until a bundle is
# shipped. `pulp_stage_runtime_dependencies()` is now defined in BOTH builds
# (PulpRuntimeStaging.cmake, included by the root CMakeLists and by
# PulpConfig.cmake.in), so the call sites are unguarded and a forgotten call is
# a configure error via pulp_assert_runtime_dependencies_staged(). This lint is
# the belt to that braces: it catches a helper that drops BOTH lines together,
# which the assertion cannot see because the assertion is one of them.

if(NOT DEFINED PULP_SOURCE_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR is required")
endif()

# Each entry: <helper file>|<target expression the helper creates>
set(_expected_sites
    "tools/cmake/PulpPluginFormats.cmake|\${target}_VST3"
    "tools/cmake/PulpPluginFormats.cmake|\${target}_CLAP"
    "tools/cmake/PulpPluginFormats.cmake|\${target}_AAX"
    "tools/cmake/PulpPluginFormats.cmake|\${target}_AU"
    "tools/cmake/PulpAppTargets.cmake|\${target}_Standalone"
    "tools/cmake/PulpAuv3.cmake|\${fw_target}"
    "tools/cmake/PulpAuv3.cmake|\${target}_AUv3"
)

set(_failures "")
foreach(_site IN LISTS _expected_sites)
    string(REPLACE "|" ";" _parts "${_site}")
    list(GET _parts 0 _rel_path)
    list(GET _parts 1 _target_expr)

    set(_abs "${PULP_SOURCE_DIR}/${_rel_path}")
    if(NOT EXISTS "${_abs}")
        list(APPEND _failures "${_rel_path}: file not found")
        continue()
    endif()
    file(READ "${_abs}" _content)

    # Match the literal call text. `${...}` is not a regex metasequence here
    # because the file is read as data, but the braces and dollar are, so quote
    # by searching for the plain substring via string(FIND).
    set(_needle "pulp_stage_runtime_dependencies(${_target_expr})")
    string(FIND "${_content}" "${_needle}" _found)
    if(_found EQUAL -1)
        list(APPEND _failures
            "${_rel_path}: no ${_needle}")
    endif()

    set(_assert_needle
        "pulp_assert_runtime_dependencies_staged(${_target_expr})")
    string(FIND "${_content}" "${_assert_needle}" _found_assert)
    if(_found_assert EQUAL -1)
        list(APPEND _failures
            "${_rel_path}: no ${_assert_needle}")
    endif()
endforeach()

# No format helper may call the wgpu primitive directly. It copies ONLY the wgpu
# runtime — no Apple @loader_path rpath, no Windows ICU data — so a call site
# left on it produces a module that builds clean and then fails once shared or
# once it renders text. Two files legitimately reference it and are exempt
# below: the consumer-side fallback definition, and the delegating entry point.
file(GLOB_RECURSE _helper_files "${PULP_SOURCE_DIR}/tools/cmake/*.cmake")
foreach(_helper IN LISTS _helper_files)
    get_filename_component(_helper_name "${_helper}" NAME)
    if(_helper_name STREQUAL "PulpWebGpuImportedTarget.cmake")
        continue()  # supplies the consumer-side fallback definition
    endif()
    if(_helper_name STREQUAL "PulpRuntimeStaging.cmake")
        continue()  # DELEGATES the wgpu copy to it, which is the design
    endif()
    file(READ "${_helper}" _content)
    string(FIND "${_content}" "target_copy_webgpu_binaries(" _legacy)
    if(NOT _legacy EQUAL -1)
        list(APPEND _failures
            "${_helper_name}: still calls target_copy_webgpu_binaries(); use "
            "pulp_stage_runtime_dependencies()")
    endif()
endforeach()

if(_failures)
    string(REPLACE ";" "\n  " _rendered "${_failures}")
    message(FATAL_ERROR
        "Runtime-staging call sites are incomplete:\n  ${_rendered}\n"
        "A format helper that skips pulp_stage_runtime_dependencies() produces "
        "a module with no wgpu runtime, no Apple @loader_path rpath, and — on "
        "Windows — no icudtl.dat, which traps the host process on the first "
        "text render.")
endif()

message(STATUS "runtime_staging_call_sites_verified=true")
