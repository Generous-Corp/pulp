# Included at the end of the top-level directory of a tree configured with
# PULP_UI_FROM_VELLUM_IN_TREE, by cmake-vellum-ui-in-tree-real through a
# deferred project include, so no file in the source tree changes for it.
#
# pulp-in-tree-plugin-proof builds what the test then checks:
#   - every retained example, pulp-native's metal probe and the NO_EDITOR link
#     floor fixture;
#   - pulp-in-tree-view-marker, an object that defines a pulp::view symbol, as
#     the control for the module scan;
#   - one reload editor probe per hot-reload example. Each links the example's
#     shell header, pulp::format-core and the plug-in's UI package the way
#     pulp_add_plugin attaches it, and asks the shell for an editor headlessly.
# The list of modules, configured example directories and probes is written to
# pulp-in-tree-plugin-modules.txt.

set(_proof_dir "${CMAKE_CURRENT_LIST_DIR}")
add_library(pulp-in-tree-view-marker OBJECT "${_proof_dir}/view_marker.cpp")
add_custom_target(pulp-in-tree-plugin-proof)
add_dependencies(pulp-in-tree-plugin-proof pulp-in-tree-view-marker pulp-examples-all
    pulp-format-core pulp-format-view pulp-vellum)
foreach(_t PulpNoEditorLinkFloor_VST3 PulpNoEditorLinkFloor_CLAP PulpNoEditorLinkFloor_AU
        PulpNoEditorLinkFloorControl_CLAP)
    if(TARGET ${_t})
        add_dependencies(pulp-in-tree-plugin-proof ${_t})
    endif()
endforeach()

include("${CMAKE_SOURCE_DIR}/tools/cmake/PulpCollectTargets.cmake")
set(_module_pattern "_(VST3|CLAP|AU|Standalone|AUv3Framework)$")
set(_lines "")
pulp_collect_targets_recursive(_targets "${CMAKE_SOURCE_DIR}/examples"
    TYPES MODULE_LIBRARY SHARED_LIBRARY EXECUTABLE)
get_property(_dirs DIRECTORY "${CMAKE_SOURCE_DIR}/examples" PROPERTY SUBDIRECTORIES)
foreach(_dir IN LISTS _dirs)
    string(APPEND _lines "dir|${_dir}\n")
endforeach()
# Plug-in examples outside examples/ that the root adds with the examples.
set(_native_probe_dir "${CMAKE_SOURCE_DIR}/integrations/pulp-native/examples/metal-probe")
if(TARGET PulpNativeMetalProbe_Core)
    pulp_collect_targets_recursive(_native_targets "${_native_probe_dir}"
        TYPES MODULE_LIBRARY SHARED_LIBRARY EXECUTABLE)
    list(APPEND _targets ${_native_targets})
    string(APPEND _lines "extra|${_native_probe_dir}\n")
endif()
foreach(_t IN LISTS _targets)
    if(_t MATCHES "${_module_pattern}")
        get_target_property(_src ${_t} SOURCE_DIR)
        string(APPEND _lines "module|${_t}|${_src}|$<TARGET_FILE:${_t}>\n")
        add_dependencies(pulp-in-tree-plugin-proof ${_t})
    endif()
endforeach()
string(APPEND _lines "marker|$<TARGET_OBJECTS:pulp-in-tree-view-marker>\n")

# The plug-ins' UI package, registered in each probe as pulp_add_plugin
# registers it in a plug-in's modules.
set(_ui_package "${PULP_HotReloadDemo_UI_PACKAGE}")
if(NOT _ui_package)
    message(FATAL_ERROR "plugin_proof: HotReloadDemo has no UI package in this mode")
endif()
_pulp_resolve_ui_package("plugin_proof" "${_ui_package}" _package_function _package_header)
set(_registrar "${CMAKE_BINARY_DIR}/pulp-in-tree-reload-probe/package.cpp")
file(CONFIGURE OUTPUT "${_registrar}" CONTENT
"#include <pulp/format/editor_package.hpp>
#include <${_package_header}>
namespace {
::pulp::format::EditorFactory* hook() { return ${_package_function}(); }
struct Registrar {
    Registrar() noexcept { ::pulp::format::install_plugin_editor_factory_hook(&hook); }
} registrar;
} // namespace
" @ONLY)
foreach(_probe IN ITEMS demo synth)
    if(_probe STREQUAL "demo")
        set(_dir hot-reload-demo)
        set(_header hot_reload_shell.hpp)
        set(_factory pulp::examples::create_hot_reload_shell)
    else()
        set(_dir hot-reload-synth)
        set(_header hot_reload_synth_shell.hpp)
        set(_factory pulp::examples::create_hot_reload_synth)
    endif()
    set(_exe pulp-in-tree-reload-editor-probe-${_probe})
    add_executable(${_exe} "${_proof_dir}/reload_editor_probe.cpp" "${_registrar}")
    target_include_directories(${_exe} PRIVATE "${CMAKE_SOURCE_DIR}/examples/${_dir}")
    target_compile_definitions(${_exe} PRIVATE
        "PROBE_HEADER=\"${_header}\"" "PROBE_FACTORY=${_factory}" "PROBE_NAME=\"${_probe}\""
        "PROBE_LOGIC=\"$<TARGET_FILE:${_dir}-logic>\"")
    target_link_libraries(${_exe} PRIVATE pulp::format-core "${_ui_package}")
    add_dependencies(${_exe} ${_dir}-logic)
    add_dependencies(pulp-in-tree-plugin-proof ${_exe})
    string(APPEND _lines "reload|${_probe}|$<TARGET_FILE:${_exe}>\n")
endforeach()

file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/pulp-in-tree-plugin-modules.txt" CONTENT "${_lines}")
