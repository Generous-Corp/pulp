# pulp_add_plugin() picks a plug-in's editor stack: lite unless the plug-in
# declares UI_SCRIPT, GPU_UI or FULL_EDITOR. Each case configures a throwaway
# project against the public helper (with stand-in Pulp targets) and reads the
# recorded PULP_<target>_EDITOR_STACK; LITE_EDITOR combined with a keyword that
# needs the full stack must fail with the combination diagnostic.
if(NOT DEFINED PULP_SOURCE_DIR OR NOT DEFINED FIXTURE_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR and FIXTURE_DIR are required")
endif()
file(MAKE_DIRECTORY "${FIXTURE_DIR}")
set(_preamble
    "cmake_minimum_required(VERSION 3.24)\n"
    "project(EditorStack NONE)\n"
    "add_library(pulp-format INTERFACE)\n"
    "add_library(pulp::format ALIAS pulp-format)\n"
    "add_library(pulp-format-core INTERFACE)\n"
    "add_library(pulp::format-core ALIAS pulp-format-core)\n"
    "add_library(ui-pkg INTERFACE)\n"
    "set_target_properties(ui-pkg PROPERTIES PULP_EDITOR_FACTORY_FUNCTION ui::factory PULP_EDITOR_FACTORY_HEADER ui.hpp)\n"
    "include(\"${PULP_SOURCE_DIR}/tools/cmake/PulpUtils.cmake\")\n")

function(_configure name call out_result out_text)
    set(_src "${FIXTURE_DIR}/${name}-src")
    set(_bin "${FIXTURE_DIR}/${name}-build")
    file(REMOVE_RECURSE "${_src}" "${_bin}")
    file(MAKE_DIRECTORY "${_src}")
    file(WRITE "${_src}/ui.js" "// stand-in scripted UI\n")
    file(WRITE "${_src}/CMakeLists.txt" ${_preamble} "${call}\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${_src}" -B "${_bin}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    string(REGEX REPLACE "[ \t\r\n]+" " " _text "${_out}${_err}")
    set(${out_result} "${_result}" PARENT_SCOPE)
    set(${out_text} "${_text}" PARENT_SCOPE)
endfunction()

function(_expect_stack name args expected)
    _configure(${name}
        "pulp_add_plugin(P${name} BUNDLE_ID dev.pulp.${name} ${args})\nmessage(STATUS \"editor-stack=\${PULP_P${name}_EDITOR_STACK}\")"
        _result _text)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "${name}: configure failed:\n${_text}")
    endif()
    string(FIND "${_text}" "editor-stack=${expected} " _at)
    if(_at LESS 0)
        message(FATAL_ERROR "${name}: expected the ${expected} editor stack:\n${_text}")
    endif()
endfunction()

function(_expect_rejected name args needle)
    _configure(${name} "pulp_add_plugin(P${name} BUNDLE_ID dev.pulp.${name} ${args})" _result _text)
    if(_result EQUAL 0)
        message(FATAL_ERROR "${name}: configure succeeded; the conflict was ignored:\n${_text}")
    endif()
    string(FIND "${_text}" "${needle}" _at)
    if(_at LESS 0)
        message(FATAL_ERROR "${name}: failed without '${needle}':\n${_text}")
    endif()
endfunction()

_expect_stack(default "" lite)
_expect_stack(explicitlite "LITE_EDITOR" lite)
_expect_stack(gpuui "GPU_UI" full)
_expect_stack(fulleditor "FULL_EDITOR" full)
_expect_stack(scripted "UI_SCRIPT ui.js" full)
_expect_rejected(litegpu "LITE_EDITOR GPU_UI" "LITE_EDITOR cannot be combined with GPU_UI")
_expect_rejected(litescript "LITE_EDITOR UI_SCRIPT ui.js" "LITE_EDITOR cannot be combined with UI_SCRIPT")

# UI <package> implies NO_EDITOR's view-free link; UI none does not. A keyword
# that selects Pulp's own view editor contradicts a UI package.
function(_expect_no_editor name args expected)
    _configure(${name}
        "pulp_add_plugin(P${name} BUNDLE_ID dev.pulp.${name} ${args})\nmessage(STATUS \"no-editor=\${PULP_P${name}_NO_EDITOR}\")"
        _result _text)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "${name}: configure failed:\n${_text}")
    endif()
    string(FIND "${_text}" "no-editor=${expected} " _at)
    if(_at LESS 0)
        message(FATAL_ERROR "${name}: expected PULP_P${name}_NO_EDITOR=${expected}:\n${_text}")
    endif()
endfunction()

_expect_no_editor(uipackage "UI ui-pkg" TRUE)
_expect_no_editor(uipackagenoeditor "UI ui-pkg NO_EDITOR" TRUE)
_expect_no_editor(uinone "UI none" FALSE)
_expect_no_editor(nouiarg "" FALSE)
foreach(_kw NATIVE_UI GPU_UI FULL_EDITOR LITE_EDITOR)
    string(TOLOWER "${_kw}" _lower)
    string(REPLACE "_" "" _lower "${_lower}")
    _expect_rejected(ui${_lower} "UI ui-pkg ${_kw}" "UI ui-pkg cannot be combined with ${_kw}")
endforeach()
_expect_rejected(uiscript "UI ui-pkg UI_SCRIPT ui.js" "UI ui-pkg cannot be combined with UI_SCRIPT")

# A UI package plug-in names the formats that build without an editor because
# their adapter does not open contract editors yet. Stand-in format targets
# make the formats exist without an SDK.
_configure(uiwarn
    "add_library(Pwarn_LV2 INTERFACE)\nadd_library(Pwarn_AAX INTERFACE)\n_pulp_warn_ui_package_formats(Pwarn ui-pkg)"
    _result _text)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "uiwarn: configure failed:\n${_text}")
endif()
foreach(_format LV2 AAX)
    string(FIND "${_text}" "${_format}" _at)
    if(_at LESS 0 OR NOT _text MATCHES "do not open editor-host contract editors yet")
        message(FATAL_ERROR "uiwarn: no warning naming ${_format}:\n${_text}")
    endif()
endforeach()
_configure(uivst3 "add_library(Pvst3_VST3 INTERFACE)\n_pulp_warn_ui_package_formats(Pvst3 ui-pkg)" _result _text)
if(_text MATCHES "do not open editor-host contract editors yet")
    message(FATAL_ERROR "uivst3: warned about VST3, which opens contract editors:\n${_text}")
endif()
_configure(uiclap "add_library(Pclap_CLAP INTERFACE)\n_pulp_warn_ui_package_formats(Pclap ui-pkg)" _result _text)
if(_text MATCHES "do not open editor-host contract editors yet")
    message(FATAL_ERROR "uiclap: warned about CLAP, which opens contract editors:\n${_text}")
endif()
_configure(uiau "add_library(Pau_AU INTERFACE)\n_pulp_warn_ui_package_formats(Pau ui-pkg)" _result _text)
if(_text MATCHES "do not open editor-host contract editors yet")
    message(FATAL_ERROR "uiau: warned about AU, which opens contract editors:\n${_text}")
endif()
_configure(uinowarn "_pulp_warn_ui_package_formats(Pnone ui-pkg)" _result _text)
if(_text MATCHES "do not open editor-host contract editors yet")
    message(FATAL_ERROR "uinowarn: warned with no format targets:\n${_text}")
endif()
_expect_rejected(uigpufull "UI ui-pkg GPU_UI FULL_EDITOR" "UI ui-pkg cannot be combined with GPU_UI, FULL_EDITOR")
message(STATUS "editor-stack selection: all cases passed")
