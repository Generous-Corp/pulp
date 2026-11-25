# The consumer modules (PulpUtils, PulpPlugin, PulpPluginFormats,
# PulpJsBytecode) read PULP_UI_PROVIDER instead of treating any Pulp::view as
# Pulp's own. An installed SDK that serves its UI names from Vellum marks them
# PULP_UI_PROVIDER "vellum" (integrations/pulp-vellum/cmake/PulpVellumUiCompat.cmake);
# Pulp's own targets carry no such property.
#
# Each scenario configures a throwaway project twice against the real modules:
# once with stand-ins shaped like Pulp's own UI targets ("pulp"), once with
# stand-ins shaped like the Vellum-backed compatibility targets ("vellum"),
# and asserts the behaviour each mode needs.
#
#   provider   the resolved view picks: Pulp's targets in pulp mode, none in
#              vellum mode (Pulp's editor adapters cannot compile against it).
#   editor     pulp_add_plugin() with the default editor configures in pulp
#              mode and is refused in vellum mode with the editors that apply.
#   no-editor  an editor-less plug-in and a UI <package> plug-in configure in
#              both modes.
#   bundle     pulp_add_plugin_bundle() shares Pulp's view editor host, so it
#              is refused in vellum mode.
#   reload     thin reload logic gets the view headers in both modes: a
#              compatibility target has no include directories of its own, and
#              PulpReload's $<TARGET_PROPERTY:...,INTERFACE_INCLUDE_DIRECTORIES>
#              reaches the Vellum target it links transitively.
#   objc       the per-binary macOS view ObjC cluster is compiled into a target
#              in pulp mode and skipped in vellum mode.
#   bytecode   pulp_add_js_bytecode() builds its generator against Pulp::view in
#              pulp mode and is refused in vellum mode.
if(NOT DEFINED PULP_SOURCE_DIR OR NOT DEFINED FIXTURE_DIR OR NOT DEFINED SCENARIO)
    message(FATAL_ERROR "PULP_SOURCE_DIR, FIXTURE_DIR and SCENARIO are required")
endif()

# Imported targets must name include directories that exist.
file(MAKE_DIRECTORY "${FIXTURE_DIR}/stand-in/pulp/include" "${FIXTURE_DIR}/stand-in/mirror/include")

# Stand-in targets for one mode. "pulp" mode exports Pulp::view and
# Pulp::view-native as Pulp's own targets. "vellum" mode defines stand-in
# Vellum mirror targets and lets the real compatibility module
# (integrations/pulp-vellum/cmake/PulpVellumUiCompat.cmake) define the Pulp
# names, so the property it sets is the one the consumer modules read.
function(_stand_ins mode out)
    set(_text
        "add_library(pulp-format INTERFACE)\n"
        "add_library(pulp::format ALIAS pulp-format)\n"
        "add_library(pulp-format-core INTERFACE)\n"
        "add_library(pulp::format-core ALIAS pulp-format-core)\n"
        "add_library(ui-pkg INTERFACE)\n"
        "set_target_properties(ui-pkg PROPERTIES PULP_EDITOR_FACTORY_FUNCTION ui::factory PULP_EDITOR_FACTORY_HEADER ui.hpp)\n")
    if(mode STREQUAL "vellum")
        list(APPEND _text
            "foreach(t IN ITEMS MirrorCanvas MirrorRender_2d MirrorView_core MirrorDesign_import MirrorView_script)\n"
            "  add_library(Vellum::\${t} INTERFACE IMPORTED)\n"
            "  set_property(TARGET Vellum::\${t} PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${FIXTURE_DIR}/stand-in/mirror/include)\n"
            "endforeach()\n"
            "include(\"${PULP_SOURCE_DIR}/integrations/pulp-vellum/cmake/PulpVellumUiCompat.cmake\")\n"
            "pulp_define_vellum_ui_compat_targets()\n"
            "list(LENGTH PULP_SDK_VELLUM_UI_COMPAT_TARGETS n)\n"
            "if(NOT n EQUAL 5)\n"
            "  message(FATAL_ERROR \"compat module defined [\${PULP_SDK_VELLUM_UI_COMPAT_TARGETS}]\")\n"
            "endif()\n")
    else()
        list(APPEND _text
            "add_library(Pulp::view INTERFACE IMPORTED)\n"
            "add_library(Pulp::view-native INTERFACE IMPORTED)\n"
            "set_property(TARGET Pulp::view PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${FIXTURE_DIR}/stand-in/pulp/include)\n")
    endif()
    string(JOIN "" _text ${_text})
    set(${out} "${_text}" PARENT_SCOPE)
endfunction()

function(_configure mode name body out_result out_text)
    _stand_ins(${mode} _targets)
    set(_src "${FIXTURE_DIR}/${SCENARIO}-${mode}-${name}-src")
    set(_bin "${FIXTURE_DIR}/${SCENARIO}-${mode}-${name}-build")
    file(REMOVE_RECURSE "${_src}" "${_bin}")
    file(MAKE_DIRECTORY "${_src}")
    file(WRITE "${_src}/ui.js" "// stand-in scripted UI\n")
    file(WRITE "${_src}/logic.cpp" "int pulp_stand_in_logic() { return 0; }\n")
    file(WRITE "${_src}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.24)\n"
        "project(UiProviderConsumer CXX)\n"
        "${_targets}"
        "include(\"${PULP_SOURCE_DIR}/tools/cmake/PulpUtils.cmake\")\n"
        "${body}\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${_src}" -B "${_bin}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    string(REGEX REPLACE "[ \t\r\n]+" " " _text "${_out}${_err}")
    set(${out_result} "${_result}" PARENT_SCOPE)
    set(${out_text} "${_text}" PARENT_SCOPE)
endfunction()

function(_expect_pass mode name body)
    _configure(${mode} ${name} "${body}" _result _text)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "${SCENARIO} (${mode}): configure failed:\n${_text}")
    endif()
    set(_text "${_text}" PARENT_SCOPE)
endfunction()

function(_expect_refused mode name body needle)
    _configure(${mode} ${name} "${body}" _result _text)
    if(_result EQUAL 0)
        message(FATAL_ERROR "${SCENARIO} (${mode}): configure succeeded; expected a refusal:\n${_text}")
    endif()
    string(FIND "${_text}" "${needle}" _at)
    if(_at LESS 0)
        message(FATAL_ERROR "${SCENARIO} (${mode}): refused without '${needle}':\n${_text}")
    endif()
endfunction()

function(_expect_text needle what)
    string(FIND "${_text}" "${needle}" _at)
    if(_at LESS 0)
        message(FATAL_ERROR "${SCENARIO}: expected ${what} ('${needle}') in:\n${_text}")
    endif()
endfunction()

set(_vellum_refusal "this Pulp SDK serves Pulp::view from Vellum, so Pulp's own view editor")

if(SCENARIO STREQUAL "provider")
    set(_probe "message(STATUS \"provider=[\${_PULP_UI_PROVIDER}] view=[\${_PULP_VIEW_TARGET}] native=[\${_PULP_NATIVE_VIEW_TARGET}]\")")
    _expect_pass(pulp probe "${_probe}")
    _expect_text("provider=[pulp] view=[Pulp::view] native=[Pulp::view-native]" "Pulp's own view picks")
    _expect_pass(vellum probe "${_probe}")
    _expect_text("provider=[vellum] view=[] native=[]" "no view picks for Pulp's editor adapters")

elseif(SCENARIO STREQUAL "editor")
    set(_call "pulp_add_plugin(Peditor BUNDLE_ID dev.pulp.editor)\nmessage(STATUS \"view-target=[\${PULP_Peditor_VIEW_TARGET}]\")")
    _expect_pass(pulp plugin "${_call}")
    _expect_text("view-target=[Pulp::view]" "the default editor on Pulp's view")
    _expect_refused(vellum plugin "${_call}" "pulp_add_plugin(Peditor): ${_vellum_refusal}")
    _expect_refused(vellum native "pulp_add_plugin(Pnative BUNDLE_ID dev.pulp.native NATIVE_UI)"
        "pulp_add_plugin(Pnative): ${_vellum_refusal}")
    # The format helper refuses on its own too, past its SDK check.
    _expect_refused(vellum vst3 "set(_PULP_VST3_SDK_DIR \"\${CMAKE_SOURCE_DIR}\")\nset(_PULP_VST3_SDK_TARGET stand-in)\n_pulp_add_vst3(Pfmt Pfmt dev.pulp.fmt 1.0.0 Pulp Effect)"
        "pulp_add_plugin(Pfmt) VST3: ${_vellum_refusal}")

elseif(SCENARIO STREQUAL "no-editor")
    # NO_EDITOR and UI <package> (which implies it) are the editors the
    # vellum-mode refusal recommends, so both must configure there.
    set(_call "pulp_add_plugin(Pquiet BUNDLE_ID dev.pulp.quiet NO_EDITOR)\nmessage(STATUS \"no-editor=[\${PULP_Pquiet_NO_EDITOR}]\")")
    set(_package "pulp_add_plugin(Ppkg BUNDLE_ID dev.pulp.pkg UI ui-pkg)\nmessage(STATUS \"no-editor=[\${PULP_Ppkg_NO_EDITOR}] package=[\${PULP_Ppkg_UI_PACKAGE}]\")")
    foreach(_mode pulp vellum)
        _expect_pass(${_mode} plugin "${_call}")
        _expect_text("no-editor=[TRUE]" "an editor-less plug-in in ${_mode} mode")
        _expect_pass(${_mode} package "${_package}")
        _expect_text("no-editor=[TRUE] package=[ui-pkg]" "a UI package plug-in in ${_mode} mode")
    endforeach()

elseif(SCENARIO STREQUAL "bundle")
    set(_call "pulp_add_plugin_bundle(Pbundle BUNDLE_NAME Pbundle BUNDLE_ID dev.pulp.bundle VERSION 1.0.0 MANUFACTURER Pulp)\nmessage(STATUS \"bundle-view=[\${PULP_Pbundle_VIEW_TARGET}]\")")
    _expect_pass(pulp bundle "${_call}")
    _expect_text("bundle-view=[Pulp::view]" "the bundle on Pulp's view")
    _expect_refused(vellum bundle "${_call}" "pulp_add_plugin_bundle(Pbundle): ${_vellum_refusal}")

elseif(SCENARIO STREQUAL "reload")
    if(WIN32)
        message(STATUS "SKIP: thin reload logic is not supported on Windows")
        return()
    endif()
    set(_call
        "pulp_add_reload_logic(Rlogic RESOLVE_FROM_HOST SOURCES logic.cpp)\n"
        "file(GENERATE OUTPUT \"\${CMAKE_BINARY_DIR}/includes.txt\" CONTENT \"[$<TARGET_PROPERTY:Rlogic,INCLUDE_DIRECTORIES>]\")\n")
    string(JOIN "" _call ${_call})
    foreach(_mode pulp vellum)
        _expect_pass(${_mode} reload "${_call}")
        file(READ "${FIXTURE_DIR}/${SCENARIO}-${_mode}-reload-build/includes.txt" _includes)
        if(_mode STREQUAL "pulp")
            set(_want "${FIXTURE_DIR}/stand-in/pulp/include")
        else()
            set(_want "${FIXTURE_DIR}/stand-in/mirror/include")
        endif()
        string(FIND "${_includes}" "${_want}" _at)
        if(_at LESS 0)
            message(FATAL_ERROR "reload (${_mode}): the thin logic's include directories lack ${_want}: ${_includes}")
        endif()
    endforeach()

elseif(SCENARIO STREQUAL "objc")
    if(NOT APPLE)
        message(STATUS "SKIP: the view ObjC cluster is macOS-only")
        return()
    endif()
    set(_call
        "add_library(Pobjc MODULE logic.cpp)\n"
        "_pulp_apply_view_mac_objc_suffix(Pobjc)\n"
        "get_target_property(s Pobjc SOURCES)\n"
        "get_target_property(d Pobjc COMPILE_DEFINITIONS)\n"
        "message(STATUS \"objc-sources=[\${s}] objc-defs=[\${d}]\")\n")
    string(JOIN "" _call ${_call})
    _expect_pass(pulp objc "${_call}")
    _expect_text("Pobjc_mac_objc_cluster.mm" "the per-binary ObjC cluster in pulp mode")
    _expect_pass(vellum objc "${_call}")
    _expect_text("objc-sources=[logic.cpp] objc-defs=[d-NOTFOUND]" "no Pulp ObjC cluster in vellum mode")
    if(_text MATCHES "macOS view ObjC sources not found")
        message(FATAL_ERROR "objc (vellum): warned about Pulp's ObjC sources:\n${_text}")
    endif()

elseif(SCENARIO STREQUAL "bytecode")
    set(_call "pulp_add_js_bytecode(Pscripts SOURCES ui.js)\nif(TARGET pulp-js-bytecode)\n  message(STATUS \"generator=[present]\")\nendif()")
    _expect_pass(pulp bytecode "${_call}")
    _expect_text("generator=[present]" "the generator built against Pulp::view")
    _expect_refused(vellum bytecode "${_call}"
        "pulp_add_js_bytecode(Pscripts): this Pulp SDK serves Pulp::view from Vellum")
    _expect_refused(vellum bytecode "${_call}"
        "this Vellum package has no vellum_add_js_bytecode()")
    # A Vellum package that ships the helper is named as the replacement.
    set(_stub "function(vellum_add_js_bytecode)\nendfunction()\n")
    _expect_refused(vellum bytecode "${_stub}${_call}"
        "Call vellum_add_js_bytecode() from the Vellum package instead")

else()
    message(FATAL_ERROR "unknown SCENARIO '${SCENARIO}'")
endif()
message(STATUS "${SCENARIO}: ok")
