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
