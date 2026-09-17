# PulpRuntimeStaging.cmake — the one truthful entry point for staging a
# plug-in/app module's runtime sidecars (WAH-11).
#
# Included by BOTH builds, which is the whole point:
#
#   * a Pulp SOURCE build (root CMakeLists.txt, after PulpDependencies.cmake),
#     where `target_copy_webgpu_binaries` comes from the upstream wgpu
#     FetchContent package;
#   * a find_package(Pulp) CONSUMER build (PulpConfig.cmake.in), where it comes
#     from PulpWebGpuImportedTarget.cmake's own fallback definition.
#
# That split is why this module exists rather than a rename. The old call sites
# invoked `target_copy_webgpu_binaries` directly, guarded by
# `if(COMMAND target_copy_webgpu_binaries)`. Two problems with that shape:
#
#   1. The NAME describes only half the job. Pulp additionally needs Skia's
#      `icudtl.dat` beside every Windows module — and a reader scanning for
#      "why does my plug-in have no text engine" has no reason to look at
#      something called "copy webgpu binaries".
#   2. Upstream's function does the wgpu copy and nothing else. It has no
#      concept of ICU, so the ICU staging could only live somewhere else,
#      which is exactly how the two ended up describable only by reading two
#      files in the right order.
#
# So: `pulp_stage_runtime_dependencies()` is the entry point, it DELEGATES the
# wgpu copy to whichever `target_copy_webgpu_binaries` is in scope, and it owns
# everything else. It never redefines or shadows upstream's function.

if(COMMAND pulp_stage_runtime_dependencies)
    return()
endif()

set(_pulp_runtime_staging_verify_script
    "${CMAKE_CURRENT_LIST_DIR}/PulpVerifyRuntimeStaging.cmake")
set(_pulp_macho_linked_runtime_staging_script
    "${CMAKE_CURRENT_LIST_DIR}/PulpStageMachOLinkedRuntime.cmake")

function(_pulp_runtime_staging_append_unique target property value)
    get_target_property(_existing ${target} ${property})
    if(NOT _existing OR _existing STREQUAL "_existing-NOTFOUND")
        set(_existing "")
    endif()
    list(FIND _existing "${value}" _index)
    if(_index EQUAL -1)
        set_property(TARGET ${target} APPEND PROPERTY ${property} "${value}")
    endif()
endfunction()

# Register an imported or in-tree shared-library target that must travel beside
# every Pulp plug-in/app module. Keeping targets rather than resolved paths lets
# multi-config generators select the matching artifact via TARGET_FILE while
# the staging and verification paths share one registry.
function(pulp_register_runtime_dependency_target runtime_target)
    if(NOT TARGET ${runtime_target})
        message(FATAL_ERROR
            "pulp_register_runtime_dependency_target(${runtime_target}): not a target")
    endif()
    get_property(_registered GLOBAL PROPERTY PULP_RUNTIME_DEPENDENCY_TARGETS)
    if(NOT _registered)
        set(_registered "")
    endif()
    list(FIND _registered "${runtime_target}" _index)
    if(_index EQUAL -1)
        set_property(GLOBAL APPEND PROPERTY
            PULP_RUNTIME_DEPENDENCY_TARGETS "${runtime_target}")
    endif()
endfunction()

function(_pulp_registered_runtime_dependency_targets out_var)
    get_property(_registered GLOBAL PROPERTY PULP_RUNTIME_DEPENDENCY_TARGETS)
    if(NOT _registered)
        set(_registered "")
    endif()
    set(${out_var} "${_registered}" PARENT_SCOPE)
endfunction()

# Register a Mach-O runtime that should be copied only when the final target
# actually has a load command for it. This keeps a large optional provider out
# of unrelated plug-ins while following static-library dead stripping exactly:
# a target pays for the sidecar if and only if its linked image needs it.
function(pulp_register_macho_linked_runtime_dependency_target runtime_target)
    if(NOT APPLE)
        message(FATAL_ERROR
            "Mach-O linked runtime registration is available only on Apple")
    endif()
    if(NOT TARGET ${runtime_target})
        message(FATAL_ERROR
            "pulp_register_macho_linked_runtime_dependency_target(${runtime_target}): not a target")
    endif()
    get_property(_registered GLOBAL PROPERTY
        PULP_MACHO_LINKED_RUNTIME_DEPENDENCY_TARGETS)
    if(NOT _registered)
        set(_registered "")
    endif()
    list(FIND _registered "${runtime_target}" _index)
    if(_index EQUAL -1)
        set_property(GLOBAL APPEND PROPERTY
            PULP_MACHO_LINKED_RUNTIME_DEPENDENCY_TARGETS "${runtime_target}")
    endif()
endfunction()

function(_pulp_macho_linked_runtime_dependency_targets out_var)
    get_property(_registered GLOBAL PROPERTY
        PULP_MACHO_LINKED_RUNTIME_DEPENDENCY_TARGETS)
    if(NOT _registered)
        set(_registered "")
    endif()
    set(${out_var} "${_registered}" PARENT_SCOPE)
endfunction()

# Resolve Skia's ICU data file from ONE canonical Pulp-side variable.
#
# Two spellings were in play: `PULP_SKIA_ICUDTL_FILE`, which PulpDependencies
# sets in a source build, and `SKIA_ICUDTL_FILE`, which FindSkia sets (including
# for an installed SDK, where PulpConfig re-runs FindSkia against the shipped
# skia-build). Prefer the Pulp-owned name; fall back to the Skia one.
function(pulp_resolve_icudtl out_var)
    set(_resolved "")
    if(PULP_SKIA_ICUDTL_FILE AND EXISTS "${PULP_SKIA_ICUDTL_FILE}")
        set(_resolved "${PULP_SKIA_ICUDTL_FILE}")
    elseif(SKIA_ICUDTL_FILE AND EXISTS "${SKIA_ICUDTL_FILE}")
        set(_resolved "${SKIA_ICUDTL_FILE}")
    endif()
    set(${out_var} "${_resolved}" PARENT_SCOPE)
endfunction()

# The runtime files `target` must have beside it after a build, for the current
# platform. Kept next to the staging function so the two cannot disagree about
# the file set.
function(pulp_expected_runtime_dependencies out_var)
    set(_expected "")
    if(WEBGPU_RUNTIME_LIB AND EXISTS "${WEBGPU_RUNTIME_LIB}")
        get_filename_component(_name "${WEBGPU_RUNTIME_LIB}" NAME)
        list(APPEND _expected "${_name}")
    endif()
    if(WIN32 AND SKIA_FOUND)
        list(APPEND _expected "icudtl.dat")
    endif()
    _pulp_registered_runtime_dependency_targets(_registered)
    foreach(_runtime_target IN LISTS _registered)
        list(APPEND _expected "$<TARGET_FILE_NAME:${_runtime_target}>")
    endforeach()

    set(${out_var} "${_expected}" PARENT_SCOPE)
endfunction()

# Stage every runtime file `target` needs beside its binary.
#
# Pulp's format helpers call this for every plug-in/app target they create, so
# a plug-in built with pulp_add_plugin / pulp_add_app needs nothing extra. Call
# it directly only when you create a loadable module yourself.
function(pulp_stage_runtime_dependencies target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR
            "pulp_stage_runtime_dependencies(${target}): not a target")
    endif()

    # The wgpu runtime copy. Delegated, never reimplemented: in a source build
    # this is the upstream FetchContent package's function, and duplicating its
    # WEBGPU_RUNTIME_LIB logic here would be a second source of truth that
    # drifts on the next dependency bump.
    if(COMMAND target_copy_webgpu_binaries)
        target_copy_webgpu_binaries(${target})
    endif()

    _pulp_registered_runtime_dependency_targets(_registered)
    foreach(_runtime_target IN LISTS _registered)
        add_custom_command(
            TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:${_runtime_target}>" $<TARGET_FILE_DIR:${target}>
            VERBATIM
            COMMENT "Staging $<TARGET_FILE_NAME:${_runtime_target}> beside ${target}")
    endforeach()

    _pulp_macho_linked_runtime_dependency_targets(_macho_linked)
    foreach(_runtime_target IN LISTS _macho_linked)
        get_target_property(_runtime_attribution_dir ${_runtime_target}
            PULP_RUNTIME_ATTRIBUTION_DIR)
        if(NOT _runtime_attribution_dir
           OR _runtime_attribution_dir MATCHES "-NOTFOUND$")
            set(_runtime_attribution_dir "")
        endif()
        get_target_property(_runtime_attribution_name ${_runtime_target}
            PULP_RUNTIME_ATTRIBUTION_NAME)
        if(NOT _runtime_attribution_name
           OR _runtime_attribution_name MATCHES "-NOTFOUND$")
            set(_runtime_attribution_name "")
        endif()
        if(_runtime_attribution_dir AND NOT _runtime_attribution_name)
            message(FATAL_ERROR
                "${_runtime_target} has PULP_RUNTIME_ATTRIBUTION_DIR but no "
                "PULP_RUNTIME_ATTRIBUTION_NAME")
        endif()
        add_custom_command(
            TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                "-DPULP_LINKED_IMAGE=$<TARGET_FILE:${target}>"
                "-DPULP_LINKED_RUNTIME=$<TARGET_FILE:${_runtime_target}>"
                "-DPULP_LINKED_RUNTIME_NAME=$<TARGET_FILE_NAME:${_runtime_target}>"
                "-DPULP_LINKED_ATTRIBUTION_DIR=${_runtime_attribution_dir}"
                "-DPULP_LINKED_ATTRIBUTION_NAME=${_runtime_attribution_name}"
                "-DPULP_STAGING_DIR=$<TARGET_FILE_DIR:${target}>"
                "-DPULP_STAGING_OWNERSHIP_DIR=${CMAKE_BINARY_DIR}/CMakeFiles/pulp-runtime-staging"
                "-DPULP_STAGING_LABEL=${target}"
                -P "${_pulp_macho_linked_runtime_staging_script}"
            VERBATIM
            COMMENT "Checking whether ${target} needs $<TARGET_FILE_NAME:${_runtime_target}>")
    endforeach()

    if(APPLE)
        # Upstream copies the dylib next to the binary but does NOT touch the
        # rpath. The binary references it as @rpath/libwgpu_native.dylib, so
        # without an @loader_path entry it resolves against the build tree —
        # fine on this machine, broken the moment the bundle is shared, and a
        # hard failure under Developer ID library validation.
        set_target_properties(${target} PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            MACOSX_RPATH TRUE)
        _pulp_runtime_staging_append_unique(${target} INSTALL_RPATH "@loader_path")
    endif()

    if(WIN32 AND SKIA_FOUND)
        # Not optional packaging metadata. SkParagraph's ICU-backed SkUnicode
        # returns null without this file and deliberately traps on its
        # `fUnicode` check during the first label render — the host process
        # dies on first paint rather than degrading to simpler text.
        pulp_resolve_icudtl(_icudtl)
        if(NOT _icudtl)
            message(FATAL_ERROR
                "Pulp's Windows Skia runtime requires icudtl.dat beside each "
                "plug-in/app module, but neither PULP_SKIA_ICUDTL_FILE nor "
                "SKIA_ICUDTL_FILE resolved to an existing file. Reinstall the "
                "matching Pulp/Skia SDK.")
        endif()
        add_custom_command(
            TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_icudtl}" $<TARGET_FILE_DIR:${target}>
            COMMENT "Staging Skia ICU runtime data next to ${target}")
    endif()

    set_property(TARGET ${target} PROPERTY PULP_RUNTIME_DEPENDENCIES_STAGED TRUE)
endfunction()

# Configure-time check that the staging call was made at all.
function(pulp_assert_runtime_dependencies_staged target)
    if(NOT TARGET ${target})
        return()
    endif()
    get_target_property(_staged ${target} PULP_RUNTIME_DEPENDENCIES_STAGED)
    if(NOT _staged)
        message(FATAL_ERROR
            "${target} is a Pulp plug-in/app output but never called "
            "pulp_stage_runtime_dependencies(). On Windows that module would "
            "ship without Skia's icudtl.dat and trap the host on its first "
            "text render; on Apple it would reference the wgpu dylib through "
            "a build-tree path. Add the call to the helper that creates it.")
    endif()
endfunction()

# POST_BUILD check that the files actually landed.
#
# Distinct from the assertion above, which only proves the CALL happened. This
# one catches a staging command whose source path was wrong, whose generator
# expression resolved somewhere unexpected, or whose copy silently produced
# nothing — none of which a source-text inspection can see.
function(pulp_verify_runtime_dependencies_staged target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR
            "pulp_verify_runtime_dependencies_staged(${target}): not a target")
    endif()
    pulp_expected_runtime_dependencies(_expected)
    # Deliberately still emitted when the list is empty: the script reports
    # "no sidecars required on this platform", so a configuration that
    # legitimately stages nothing still leaves evidence the check ran. Without
    # that, "wiring removed" and "nothing to check" are identical silence.
    add_custom_command(
        TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DPULP_STAGING_DIR=$<TARGET_FILE_DIR:${target}>"
            "-DPULP_STAGING_FILES=${_expected}"
            "-DPULP_STAGING_LABEL=${target}"
            -P "${_pulp_runtime_staging_verify_script}"
        VERBATIM
        COMMENT "Verifying staged runtime dependencies for ${target}")
endfunction()
