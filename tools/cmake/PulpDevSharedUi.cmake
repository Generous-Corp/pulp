# PulpDevSharedUi.cmake: development-only shared Pulp runtime for fast relinks.
#
# A plug-in module normally links every Pulp library and the Skia/Dawn stack
# statically: a 40+ MB image whose link and post-link scans run on every edit.
# With PULP_DEV_SHARED_UI=ON (macOS only, OFF by default; `pulp dev` and `pulp
# loop` turn it on in SDK projects), the format-independent Pulp libraries are
# linked ONCE into libpulp-dev-shared-ui.<version>.dylib in the build tree, and
# each plug-in module links against that dylib instead. The module image drops
# to about a megabyte, so relinking it after a one-file edit costs a fraction of
# the static link.
#
# A dev-shared module's link edge runs only the linker. The per-link steps of a
# static module move to build edges that already exist for `all` builds: the
# control-shipping scan and pulp-build-info.json refresh through their stamp
# steps, PkgInfo through its own edge, and the stale-wgpu cleanup is skipped. A
# dev-shared AU is not checked for relocatability. Each of those helpers tests
# the PULP_DEV_SHARED_UI target property this file sets.
#
# What stays static in every module, and why:
# - libpulp-format* and libpulp-standalone*: the format adapters call hooks the
#   plug-in itself defines (make_plug_view, the processor factory), so they
#   must resolve against that module's own objects.
# - The VST3 SDK and AudioUnitSDK: only the adapters use them.
# - The per-binary Objective-C view cluster: it keeps compiling into each
#   module with its suffixed class names, exactly as in a static build.
#
# A build made this way is NOT distributable. The module loads its Pulp code
# from an absolute path inside this build tree, so on any other machine, or
# after the build directory moves, it fails to load. `pulp ship package`,
# `notarize`, `release` and `share`, and `sign` with a distribution identity,
# switch such a build back to static links before shipping (or print the one
# command that does); an ad hoc or Apple Development signature for local
# loading is allowed. tools/scripts/build_combined_installer.sh refuses any
# input that references the dylib.
#
# Behaviour that differs from a static build, all of it development-only:
# - Every member of the force-loaded libraries is present, so their static
#   initializers all run when the dylib loads.
# - The VST3, CLAP, AU and Standalone builds of one project share a single
#   copy of Pulp's globals when a host loads more than one of them.
# - The dylib carries Pulp's and SDL's fixed-name Objective-C classes, so a host
#   that is itself a Pulp app logs "implemented in both" warnings, and two
#   dev-shared projects loaded into one host each bring a copy.
# - A sandboxed host may refuse to load code from outside the bundle.
#
# The link rewriting is done by pulp_dev_shared_link.sh, installed as the
# linker launcher of the dylib and of each module (see that script). That needs
# a generator that honours <LANG>_LINKER_LAUNCHER for a single configuration:
# Ninja or Unix Makefiles.

option(PULP_DEV_SHARED_UI
    "Development only: link plug-in modules against one shared Pulp runtime dylib (macOS; never distributable)"
    OFF)

set(_PULP_DEV_SHARED_UI_LINKER "${CMAKE_CURRENT_LIST_DIR}/scripts/pulp_dev_shared_link.sh")

# The option only speeds up development links, so where it cannot apply the
# build links statically and the cache records OFF. That keeps the ship guards,
# which refuse a cache with the option on, from refusing a static build.
if(PULP_DEV_SHARED_UI)
    get_property(_pulp_dev_shared_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    set(_pulp_dev_shared_unsupported "")
    if(NOT APPLE OR IOS OR PULP_IOS)
        set(_pulp_dev_shared_unsupported "it is supported on macOS only")
    elseif(_pulp_dev_shared_multi_config OR CMAKE_GENERATOR STREQUAL "Xcode")
        set(_pulp_dev_shared_unsupported
            "it needs a single-configuration Ninja or Unix Makefiles generator, not '${CMAKE_GENERATOR}'")
    endif()
    if(_pulp_dev_shared_unsupported)
        message(WARNING
            "PULP_DEV_SHARED_UI is ignored because ${_pulp_dev_shared_unsupported}; "
            "plug-ins link statically.")
        set(PULP_DEV_SHARED_UI OFF CACHE BOOL
            "Development only: link plug-in modules against one shared Pulp runtime dylib (macOS; never distributable)"
            FORCE)
    endif()
endif()
if(PULP_DEV_SHARED_UI)
    if(NOT EXISTS "${_PULP_DEV_SHARED_UI_LINKER}")
        message(FATAL_ERROR
            "PULP_DEV_SHARED_UI: the link helper is missing: ${_PULP_DEV_SHARED_UI_LINKER}")
    endif()
    message(STATUS
        "Pulp: PULP_DEV_SHARED_UI=ON. Plug-in modules link a development-only shared runtime; "
        "these artifacts cannot be signed, packaged, notarized or shared.")
endif()

# Creates the shared runtime target once per build tree and returns its name.
function(_pulp_dev_shared_ui_library out_var)
    set(_lib pulp_dev_shared_ui)
    set(${out_var} ${_lib} PARENT_SCOPE)
    if(TARGET ${_lib})
        return()
    endif()
    set(_dir "${CMAKE_BINARY_DIR}/pulp-dev-shared-ui")
    # The dylib needs one object of its own, in whichever language the project
    # enabled. The retained string is a recognizable marker in the image for
    # anyone inspecting it by hand.
    if(CMAKE_CXX_COMPILER_LOADED)
        set(_source "${_dir}/pulp_dev_shared_ui.cpp")
        set(_linkage "extern \"C\" ")
    else()
        set(_source "${_dir}/pulp_dev_shared_ui.c")
        set(_linkage "")
    endif()
    file(CONFIGURE OUTPUT "${_source}" CONTENT
"// Generated by PulpDevSharedUi.cmake. Do not edit.
${_linkage}__attribute__((used, visibility(\"default\")))
const char pulp_dev_shared_ui_marker[] = \"PULP_DEV_SHARED_UI_DO_NOT_SHIP\";
" @ONLY)
    add_library(${_lib} SHARED "${_source}")
    # Link the umbrella targets every module links, so the dylib sees the same
    # archive closure. The link helper force-loads each format-independent Pulp
    # archive and drops the adapter archives; third-party archives resolve
    # normally, so only the members Pulp actually uses are included.
    target_link_libraries(${_lib} PRIVATE ${_PULP_FORMAT_TARGET} ${_PULP_VIEW_TARGET})
    if(_PULP_STANDALONE_TARGET)
        target_link_libraries(${_lib} PRIVATE ${_PULP_STANDALONE_TARGET})
    endif()
    set(_version "${Pulp_VERSION}")
    if(NOT _version)
        set(_version "${PROJECT_VERSION}")
    endif()
    set_target_properties(${_lib} PROPERTIES
        OUTPUT_NAME "pulp-dev-shared-ui.${_version}"
        LIBRARY_OUTPUT_DIRECTORY "${_dir}"
        INSTALL_NAME_DIR "@rpath"
        BUILD_WITH_INSTALL_NAME_DIR ON)
    foreach(_lang C CXX OBJC OBJCXX)
        set_property(TARGET ${_lib} PROPERTY ${_lang}_LINKER_LAUNCHER
            /bin/bash "${_PULP_DEV_SHARED_UI_LINKER}" dylib "${_dir}/archives.txt")
    endforeach()
endfunction()

# Links `module_target` (a format binary of plug-in `target`) against the
# shared runtime when PULP_DEV_SHARED_UI is on and the plug-in uses the
# default view stack. Sets `out_var` to TRUE when it did. Editor-less,
# native-view and control-capable plug-ins keep their static link: they
# either do not carry the heavy stack or link libraries the dylib does not.
function(_pulp_dev_shared_ui_attach target module_target out_var)
    set(${out_var} FALSE PARENT_SCOPE)
    if(NOT PULP_DEV_SHARED_UI)
        return()
    endif()
    if(PULP_${target}_NO_EDITOR OR PULP_${target}_CONTROL_CAPABILITIES OR
       NOT "${PULP_${target}_VIEW_TARGET}" STREQUAL "${_PULP_VIEW_TARGET}")
        return()
    endif()
    _pulp_dev_shared_ui_library(_lib)
    set(_dir "${CMAKE_BINARY_DIR}/pulp-dev-shared-ui")
    get_target_property(_name ${_lib} OUTPUT_NAME)
    foreach(_lang C CXX OBJC OBJCXX)
        set_property(TARGET ${module_target} PROPERTY ${_lang}_LINKER_LAUNCHER
            /bin/bash "${_PULP_DEV_SHARED_UI_LINKER}" module
            "${_dir}/lib${_name}.dylib" "${_dir}/archives.txt")
    endforeach()
    add_dependencies(${module_target} ${_lib})
    set_property(TARGET ${module_target} PROPERTY PULP_DEV_SHARED_UI TRUE)
    set(${out_var} TRUE PARENT_SCOPE)
endfunction()
