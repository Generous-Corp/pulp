#[[
Contract test for pulp_add_plugin(UI_SCRIPT) embedding.

A plug-in's editor script used to reach the image only as an absolute source
path that the editor read at every open, so a shipped plug-in depended on the
developer's source tree existing on the end user's machine -- and whoever
could create that path got script execution in every host that loaded the
plug-in. The script, the theme beside it and the files beside it are now
compiled in as bytes and registered with pulp::format at load through the
same per-target registration unit that carries the on-disk paths, and those
paths are recorded only by a dev build.

Two halves:

  1. A real configure + build + run of a throwaway project that attaches a
     script through the same helper every format target uses
     (_pulp_apply_ui_script_definition), twice:
       - release (PULP_DEV_BUILD unset): the binary carries the script bytes,
         the theme beside it and the image beside it, registers them at load
         (the run prints what the registry holds), and contains neither the
         script's absolute source path nor the declared asset-root path;
       - dev (PULP_DEV_BUILD=ON): the positive control -- the same binary now
         carries both paths, which proves the scan can see them, and still
         carries the embedded bytes.
     The byte scan is its own control: it must find the script marker in both
     builds, so an absent path is an absent path and not a scanner that read
     nothing.

  2. A source lint that every format target pulp_add_plugin() gives an editor
     still attaches its script through that helper. The build half proves one
     target can be wired; it cannot prove the public API wires all of them.
]]

cmake_minimum_required(VERSION 3.24)

get_filename_component(_repo_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_ui_assets_module "${_repo_root}/tools/cmake/PulpUiAssets.cmake")
set(_format_include "${_repo_root}/core/format/include")
set(_plugin_formats "${_repo_root}/tools/cmake/PulpPluginFormats.cmake")
set(_app_targets "${_repo_root}/tools/cmake/PulpAppTargets.cmake")

foreach(_p IN LISTS _ui_assets_module _plugin_formats _app_targets)
    if(NOT EXISTS "${_p}")
        message(FATAL_ERROR "Required file missing: ${_p}")
    endif()
endforeach()

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# ── Half 2: every editor-bearing format target goes through the helper ───
file(READ "${_plugin_formats}" _formats_text)
file(READ "${_app_targets}" _apps_text)
set(_missing "")
foreach(_suffix VST3 CLAP AU AAX)
    string(FIND "${_formats_text}" "_pulp_apply_ui_script_definition(\${target}_${_suffix}" _found)
    if(_found EQUAL -1)
        list(APPEND _missing "${_suffix}")
    endif()
endforeach()
foreach(_suffix Standalone ControlHost)
    string(FIND "${_apps_text}" "_pulp_apply_ui_script_definition(\${target}_${_suffix}" _found)
    if(_found EQUAL -1)
        list(APPEND _missing "${_suffix}")
    endif()
endforeach()
if(_missing)
    string(REPLACE ";" ", " _rendered "${_missing}")
    message(FATAL_ERROR
        "These format targets no longer attach UI_SCRIPT through "
        "_pulp_apply_ui_script_definition(), so they embed nothing: ${_rendered}")
endif()

# ── Half 1: configure, build, run, scan ──────────────────────────────────
if(NOT PULP_UI_SCRIPT_EMBED_TEST_DIR)
    set(PULP_UI_SCRIPT_EMBED_TEST_DIR "${CMAKE_CURRENT_BINARY_DIR}/ui-script-embed")
endif()
set(_pdir "${PULP_UI_SCRIPT_EMBED_TEST_DIR}/project")
file(REMOVE_RECURSE "${PULP_UI_SCRIPT_EMBED_TEST_DIR}")
file(MAKE_DIRECTORY "${_pdir}/ui/assets")

# Distinctive markers: the scan asserts on these exact bytes.
set(_script_marker "embed-marker-7f3a")
set(_theme_marker "#7f3a9c")
set(_asset_marker "sibling-asset-marker-9c1e")
file(WRITE "${_pdir}/ui/main.js"
    "createLabel('${_script_marker}', 'embedded', '');\n")
file(WRITE "${_pdir}/ui/theme.json"
    "{\"colors\":{\"bg.primary\":\"${_theme_marker}\"}}\n")
# A file beside the script, in a subdirectory: embedded under its relative name.
file(WRITE "${_pdir}/ui/assets/logo.svg" "<svg><!-- ${_asset_marker} --></svg>\n")
# Files the embed must leave out: a source map, prose, and a nested build tree.
file(WRITE "${_pdir}/ui/main.js.map" "{\"version\":3}\n")
file(WRITE "${_pdir}/ui/README.md" "readme-marker-4b2d\n")
file(MAKE_DIRECTORY "${_pdir}/ui/out")
file(WRITE "${_pdir}/ui/out/CMakeCache.txt" "x\n")
file(WRITE "${_pdir}/ui/out/stale.json" "{\"stale\":true}\n")
set(_script_abs "${_pdir}/ui/main.js")
set(_assets_abs "${_pdir}/assets")
file(MAKE_DIRECTORY "${_assets_abs}")

# The fixture stands in for pulp::format's registry: the generated unit needs
# only the declarations in editor_assets.hpp, and the run prints the record.
file(WRITE "${_pdir}/main.cpp" [[
#include <pulp/format/editor_assets.hpp>
#include <cstdio>
#include <cstring>

namespace pulp::format {
namespace { EditorAssets g_assets{}; bool g_builder = false; }
void register_editor_assets(const EditorAssets& assets) noexcept { g_assets = assets; }
EditorAssets registered_editor_assets() noexcept { return g_assets; }
void install_scripted_editor_builder() noexcept { g_builder = true; }
} // namespace pulp::format

int main() {
    const auto assets = pulp::format::registered_editor_assets();
    if (assets.script == nullptr || assets.script_size == 0) {
        std::puts("embedded=none");
        return 2;
    }
    std::printf("embedded=%s script_size=%zu theme_size=%zu files=%zu\n",
                assets.script_name, assets.script_size, assets.theme_size, assets.file_count);
    for (std::size_t i = 0; i < assets.file_count; ++i)
        std::printf("file=%s size=%zu\n", assets.files[i].name, assets.files[i].size);
    const bool has_path = assets.script_path != nullptr && assets.script_path[0] != '\0';
    std::printf("dev_path=%s\n", has_path ? assets.script_path : "none");
    std::printf("builder=%s\n", pulp::format::g_builder ? "installed" : "none");
    const bool has_roots = assets.asset_roots != nullptr && assets.asset_roots[0] != '\0';
    std::printf("asset_roots=%s\n", has_roots ? assets.asset_roots : "none");
    return 0;
}
]])

file(WRITE "${_pdir}/CMakeLists.txt" [[
cmake_minimum_required(VERSION 3.24)
project(PulpUiScriptEmbedFixture CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include("${PULP_UI_ASSETS_MODULE}")

add_executable(fixture main.cpp)
target_include_directories(fixture PRIVATE "${PULP_FORMAT_INCLUDE}")
set_target_properties(fixture PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
# The same helpers every format target calls from pulp_add_plugin() /
# pulp_use_kit_ui().
_pulp_apply_ui_script_definition(fixture "${CMAKE_CURRENT_SOURCE_DIR}/ui/main.js")
_pulp_apply_ui_asset_roots_definition(fixture "${CMAKE_CURRENT_SOURCE_DIR}/assets")
]])

# Returns the fixture binary's stdout and path, and the configure output, in
# the named variables.
function(_build_and_run variant extra_args out_stdout out_binary out_configure)
    set(_bdir "${PULP_UI_SCRIPT_EMBED_TEST_DIR}/build-${variant}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${_pdir}" -B "${_bdir}"
                -DCMAKE_BUILD_TYPE=Release
                "-DPULP_UI_ASSETS_MODULE=${_ui_assets_module}"
                "-DPULP_FORMAT_INCLUDE=${_format_include}"
                ${extra_args}
        RESULT_VARIABLE _rv OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rv EQUAL 0)
        message(FATAL_ERROR "[${variant}] fixture configure failed (rc=${_rv}):\n${_out}\n${_err}")
    endif()
    set(${out_configure} "${_out}\n${_err}" PARENT_SCOPE)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${_bdir}" --config Release --parallel 2
        RESULT_VARIABLE _rv OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rv EQUAL 0)
        message(FATAL_ERROR "[${variant}] fixture build failed (rc=${_rv}):\n${_out}\n${_err}")
    endif()
    file(GLOB_RECURSE _bins
        "${_bdir}/bin/fixture" "${_bdir}/bin/fixture.exe" "${_bdir}/bin/*/fixture.exe")
    list(LENGTH _bins _n)
    if(NOT _n EQUAL 1)
        message(FATAL_ERROR "[${variant}] expected one fixture binary under ${_bdir}/bin, found: ${_bins}")
    endif()
    list(GET _bins 0 _bin)
    execute_process(
        COMMAND "${_bin}"
        RESULT_VARIABLE _rv OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rv EQUAL 0)
        message(FATAL_ERROR "[${variant}] fixture binary failed (rc=${_rv}):\n${_out}\n${_err}")
    endif()
    set(${out_stdout} "${_out}" PARENT_SCOPE)
    set(${out_binary} "${_bin}" PARENT_SCOPE)
endfunction()

# True when the binary's bytes contain `needle`. Python rather than strings(1):
# the Windows toolchain has no strings, and a byte search is the same test.
function(_binary_contains binary needle out_var)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -I -c
            "import sys, pathlib; sys.exit(0 if sys.argv[2].encode() in pathlib.Path(sys.argv[1]).read_bytes() else 1)"
            "${binary}" "${needle}"
        RESULT_VARIABLE _rv)
    if(_rv EQUAL 0)
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

set(_failures "")
file(SIZE "${_script_abs}" _script_size)
file(SIZE "${_pdir}/ui/theme.json" _theme_size)
file(SIZE "${_pdir}/ui/assets/logo.svg" _asset_size)

# Release: embedded bytes present and registered, no source path.
_build_and_run(release "" _release_out _release_bin _release_configure)
if(NOT _release_out MATCHES "embedded=main\\.js script_size=${_script_size} theme_size=${_theme_size} files=1")
    list(APPEND _failures "release: the binary did not register the embedded script, theme and one sibling file at load; stdout was: ${_release_out}")
endif()
if(NOT _release_out MATCHES "file=assets/logo\\.svg size=${_asset_size}")
    list(APPEND _failures "release: the sibling file is not registered under its script-relative name; stdout was: ${_release_out}")
endif()
if(_release_out MATCHES "main\\.js\\.map" OR _release_out MATCHES "stale\\.json"
   OR _release_out MATCHES "README\\.md")
    list(APPEND _failures "release: a source map, prose file or nested build tree's file was embedded; stdout was: ${_release_out}")
endif()
# An embedded script with no on-disk path still has to install the scripted
# editor builder, or the editor would open AutoUi.
if(NOT _release_out MATCHES "builder=installed")
    list(APPEND _failures "release: the embedded editor did not install the scripted editor builder; stdout was: ${_release_out}")
endif()
if(NOT _release_out MATCHES "dev_path=none")
    list(APPEND _failures "release: the on-disk script path is registered in a non-dev build: ${_release_out}")
endif()
if(NOT _release_out MATCHES "asset_roots=none")
    list(APPEND _failures "release: asset roots are registered in a non-dev build: ${_release_out}")
endif()
_binary_contains("${_release_bin}" "${_script_marker}" _has_script)
_binary_contains("${_release_bin}" "${_theme_marker}" _has_theme)
_binary_contains("${_release_bin}" "${_asset_marker}" _has_asset)
_binary_contains("${_release_bin}" "${_script_abs}" _has_path)
_binary_contains("${_release_bin}" "${_assets_abs}" _has_roots)
if(NOT _has_script)
    list(APPEND _failures "release: the script bytes are not in the binary")
endif()
if(NOT _has_theme)
    list(APPEND _failures "release: the sibling theme.json bytes are not in the binary")
endif()
if(NOT _has_asset)
    list(APPEND _failures "release: the sibling asset's bytes are not in the binary")
endif()
if(_has_path)
    list(APPEND _failures "release: the absolute source path '${_script_abs}' is baked into the binary")
endif()
if(_has_roots)
    list(APPEND _failures "release: the asset-root source path '${_assets_abs}' is baked into the binary")
endif()

# Dev: the positive control for the path scan.
_build_and_run(dev "-DPULP_DEV_BUILD=ON" _dev_out _dev_bin _dev_configure)
if(NOT _dev_out MATCHES "dev_path=${_script_abs}")
    list(APPEND _failures "dev: PULP_DEV_BUILD=ON did not register the on-disk script path: ${_dev_out}")
endif()
if(NOT _dev_out MATCHES "asset_roots=${_assets_abs}")
    list(APPEND _failures "dev: PULP_DEV_BUILD=ON did not register the asset roots: ${_dev_out}")
endif()
if(NOT _dev_out MATCHES "embedded=main\\.js script_size=${_script_size}")
    list(APPEND _failures "dev: a dev build must still embed the script: ${_dev_out}")
endif()
_binary_contains("${_dev_bin}" "${_script_marker}" _dev_has_script)
_binary_contains("${_dev_bin}" "${_script_abs}" _dev_has_path)
_binary_contains("${_dev_bin}" "${_assets_abs}" _dev_has_roots)
if(NOT _dev_has_script)
    list(APPEND _failures "dev: the script bytes are not in the binary")
endif()
if(NOT _dev_has_path)
    list(APPEND _failures "dev: the control failed -- the scan cannot see '${_script_abs}' in a build that records it")
endif()
if(NOT _dev_has_roots)
    list(APPEND _failures "dev: the control failed -- the asset root '${_assets_abs}' is not in a build that records it")
endif()

# Refusal: a script in a directory that holds a CMakeLists.txt (a project
# root) would embed the project's own files into every format binary.
set(_root_pdir "${PULP_UI_SCRIPT_EMBED_TEST_DIR}/project-root-script")
file(MAKE_DIRECTORY "${_root_pdir}")
file(WRITE "${_root_pdir}/ui.js" "createLabel('root', 'x', '');\n")
file(WRITE "${_root_pdir}/main.cpp" "int main() { return 0; }\n")
file(WRITE "${_root_pdir}/CMakeLists.txt" [[
cmake_minimum_required(VERSION 3.24)
project(PulpUiScriptRootFixture CXX)
include("${PULP_UI_ASSETS_MODULE}")
add_executable(fixture main.cpp)
target_include_directories(fixture PRIVATE "${PULP_FORMAT_INCLUDE}")
_pulp_apply_ui_script_definition(fixture "${CMAKE_CURRENT_SOURCE_DIR}/ui.js")
]])
execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${_root_pdir}" -B "${PULP_UI_SCRIPT_EMBED_TEST_DIR}/build-root"
            "-DPULP_UI_ASSETS_MODULE=${_ui_assets_module}"
            "-DPULP_FORMAT_INCLUDE=${_format_include}"
    RESULT_VARIABLE _root_rv OUTPUT_VARIABLE _root_out ERROR_VARIABLE _root_err)
if(_root_rv EQUAL 0)
    list(APPEND _failures "root-script: a UI_SCRIPT beside a CMakeLists.txt configured instead of being refused")
elseif(NOT "${_root_out}${_root_err}" MATCHES "holds a CMakeLists.txt")
    list(APPEND _failures "root-script: the configure failed for another reason than the refusal:\n${_root_out}\n${_root_err}")
endif()

if(_failures)
    string(REPLACE ";" "\n  " _r "${_failures}")
    message(FATAL_ERROR "UI_SCRIPT embedding contract broken:\n  ${_r}")
endif()

message(STATUS "ui_script_embed_verified=true")
