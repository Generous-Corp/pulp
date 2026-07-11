# PulpBundleRelocatable.cmake — make macOS GPU bundles self-contained, and
# fail the build if they aren't.
#
# THE RECURRING FOOTGUN this exists to kill: a GPU plugin/app links
# libwgpu_native.dylib (or another runtime dylib). The upstream WebGPU
# FetchContent copies the dylib INTO the bundle but rpaths the binary only at the
# BUILD CACHE. On the build machine that path exists, so everything — build,
# codesign, notarize, auval, pluginval, even loading in a local DAW — PASSES.
# Copied to any other Mac the dylib isn't found: the app crashes at launch
# ("Library not loaded: @rpath/libwgpu_native.dylib") and plugins show no editor
# / "couldn't load". It is a false pass that only a different machine reveals.
#
# Use BOTH helpers on every distributable GPU bundle target:
#
#   pulp_make_bundle_relocatable(MyPlugin_CLAP)       # the fix
#   pulp_validate_bundle_relocatable(MyPlugin_CLAP)   # the guard (POST_BUILD)
#
# `make` bakes an @loader_path rpath into the shipped binary (the dylib sits next
# to it in Contents/MacOS) via BUILD_WITH_INSTALL_RPATH, since Pulp bundles ship
# from the build tree, not via `cmake --install`. `validate` runs
# check_bundle_relocatable.py against the finished bundle and FAILS the build if
# any binary has an external (build-machine-only) rpath or an unresolved
# @rpath dependency — catching a regression before it can ship.
#
# NOTE for adoption in pulp_add_plugin: prefer the additive POST_BUILD
# `install_name_tool -add_rpath @loader_path` form for targets that also depend
# on OTHER runtime dylibs by build rpath (e.g. some V8 engines) — see FindV8.cmake
# — because BUILD_WITH_INSTALL_RPATH drops ALL auto build rpaths, not just wgpu's.
# wgpu-only bundles (the common case) are fine with the simple form below.

function(pulp_make_bundle_relocatable target)
    if(APPLE AND TARGET ${target})
        set_target_properties(${target} PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            MACOSX_RPATH TRUE)
        set_property(TARGET ${target} APPEND PROPERTY INSTALL_RPATH "@loader_path")
    endif()
endfunction()

function(pulp_validate_bundle_relocatable target)
    if(APPLE AND TARGET ${target})
        find_package(Python3 COMPONENTS Interpreter QUIET)
        set(_py "${Python3_EXECUTABLE}")
        if(NOT _py)
            set(_py python3)
        endif()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${_py}"
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/scripts/check_bundle_relocatable.py"
                "$<TARGET_FILE_DIR:${target}>/../.."
                --strict --label "${target}"
            COMMENT "Verifying ${target} bundle is self-contained (relocatable)"
            VERBATIM)
    endif()
endfunction()

# Assert a shipped macOS bundle carries the expected architecture on its main
# binary AND every embedded dylib, and that each is validly signed. This is the
# G3 universal-build gate: it catches a thin embedded libwgpu_native.dylib /
# libv8.dylib in an otherwise-universal bundle (the app would crash at load on
# the missing arch), and an unsigned raw-lipo fat dylib (whose arm64 slice is
# killed at load). POST_BUILD, mirroring pulp_validate_bundle_relocatable.
#
#   pulp_validate_bundle_architectures(<target> [ARCHS "arm64;x86_64"])
#
# ARCHS defaults to CMAKE_OSX_ARCHITECTURES (the arches the target was built
# for), or the host arch when that is unset.
function(pulp_validate_bundle_architectures target)
    if(NOT (APPLE AND TARGET ${target}))
        return()
    endif()
    cmake_parse_arguments(PVBA "" "" "ARCHS" ${ARGN})

    set(_archs "${PVBA_ARCHS}")
    if(NOT _archs)
        set(_archs "${CMAKE_OSX_ARCHITECTURES}")
    endif()
    if(NOT _archs)
        execute_process(COMMAND uname -m OUTPUT_VARIABLE _archs
                        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
    # The script accepts a comma/semicolon/space list; hand it a comma list.
    string(REPLACE ";" "," _archs_csv "${_archs}")

    # Resolve the checker in both the in-tree layout (tools/cmake/ →
    # ../scripts/) and the installed SDK layout (lib/cmake/Pulp/ →
    # scripts/, shipped by PulpInstallRules.cmake).
    set(_script "")
    foreach(_cand
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../scripts/check_bundle_architectures.py"
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/scripts/check_bundle_architectures.py")
        if(EXISTS "${_cand}")
            set(_script "${_cand}")
            break()
        endif()
    endforeach()
    if(NOT _script)
        message(FATAL_ERROR
            "pulp_validate_bundle_architectures: could not find "
            "check_bundle_architectures.py next to PulpBundleRelocatable.cmake "
            "(looked in ../scripts and scripts). The arch gate would be a "
            "silent no-op.")
    endif()

    find_package(Python3 COMPONENTS Interpreter QUIET)
    set(_py "${Python3_EXECUTABLE}")
    if(NOT _py)
        set(_py python3)
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${_py}" "${_script}"
            "$<TARGET_FILE_DIR:${target}>/../.."
            --archs "${_archs_csv}" --strict --label "${target}"
        COMMENT "Verifying ${target} bundle architectures (${_archs_csv}) + signatures"
        VERBATIM)
endfunction()
