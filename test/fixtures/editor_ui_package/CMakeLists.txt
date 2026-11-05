# pulp_add_plugin(... UI <package>): each plug-in binary returns its own UI
# package's editor factory from Processor::editor_factory(), and UI none (or
# no UI argument) generates and links nothing.
#
# Four CLAP plug-ins share one processor that does not override
# editor_factory(): two declare different UI packages (UI alone, which implies
# NO_EDITOR's view-free link), and two are editor-less NO_EDITOR plug-ins, one
# with UI none and one with no UI. pulp-test-editor-ui-package-host loads all
# four into one process and reads which factory each reports.
if(PULP_IOS OR ANDROID OR EMSCRIPTEN OR NOT PULP_HAS_CLAP)
    return()
endif()

# Two UI packages, each declaring the package convention.
foreach(_pkg a b)
    add_library(pulp-test-ui-package-${_pkg} STATIC ui_package_${_pkg}.cpp)
    target_link_libraries(pulp-test-ui-package-${_pkg} PUBLIC pulp::format-core)
    target_include_directories(pulp-test-ui-package-${_pkg} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(pulp-test-ui-package-${_pkg} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        PULP_EDITOR_FACTORY_FUNCTION "pulp_test_ui_${_pkg}::editor_factory"
        PULP_EDITOR_FACTORY_HEADER "ui_package_${_pkg}.hpp")
endforeach()

# The UI package plug-in A and the UI none plug-in also build an AU v2
# component, so the AU contract view factory is exercised in a real module.
set(_au_variants "")
if(APPLE AND PULP_HAS_AUSDK)
    set(_au_variants A None)
endif()

foreach(_variant A B None Plain)
    set(_formats CLAP)
    string(SUBSTRING "${_variant}" 0 1 _code)
    if(_variant IN_LIST _au_variants)
        list(APPEND _formats AU)
    endif()
    if(_variant STREQUAL "A")
        set(_ui UI pulp-test-ui-package-a)
    elseif(_variant STREQUAL "B")
        set(_ui UI pulp-test-ui-package-b)
    elseif(_variant STREQUAL "None")
        set(_ui NO_EDITOR UI none)
    else()
        set(_ui NO_EDITOR)
    endif()
    pulp_add_plugin(PulpEditorUi${_variant}
        ${_ui}
        FORMATS           ${_formats}
        PLUGIN_NAME       "Pulp Editor UI ${_variant}"
        BUNDLE_ID         "dev.pulp.test.editor-ui-package-${_variant}"
        MANUFACTURER      "Pulp"
        VERSION           "1.0.0"
        CATEGORY          Effect
        PLUGIN_CODE       "PEu${_code}"
        MANUFACTURER_CODE "PlEu"
    )
    if(TARGET PulpEditorUi${_variant}_AU)
        target_compile_definitions(PulpEditorUi${_variant}_AU PRIVATE
            PULP_FIXTURE_AU_CLASS=PulpEditorUi${_variant}AU)
    endif()
endforeach()

if(_au_variants)
    add_executable(pulp-test-editor-ui-package-au-host editor_ui_package_au_host.mm)
    target_link_libraries(pulp-test-editor-ui-package-au-host PRIVATE
        "-framework AppKit" "-framework AudioToolbox" "-framework AudioUnit")
    add_dependencies(pulp-test-editor-ui-package-au-host PulpEditorUiA_AU PulpEditorUiNone_AU)
    add_test(NAME editor-ui-package-au-contract-view
        COMMAND pulp-test-editor-ui-package-au-host
            "$<TARGET_BUNDLE_DIR:PulpEditorUiA_AU>" "$<TARGET_BUNDLE_DIR:PulpEditorUiNone_AU>")
    set_tests_properties(editor-ui-package-au-contract-view PROPERTIES
        LABELS "format;editor-host;auv2" TIMEOUT 60
        ENVIRONMENT_MODIFICATION
            "CI=unset:;PULP_HEADLESS=unset:;PULP_TEST_MODE=unset:;PULP_DISABLE_PLUGIN_EDITOR=unset:")
endif()

add_executable(pulp-test-editor-ui-package-host editor_ui_package_host.cpp)
target_link_libraries(pulp-test-editor-ui-package-host PRIVATE clap ${CMAKE_DL_LIBS})
add_dependencies(pulp-test-editor-ui-package-host
    PulpEditorUiA_CLAP PulpEditorUiB_CLAP PulpEditorUiNone_CLAP PulpEditorUiPlain_CLAP)

add_test(NAME editor-ui-package-per-plugin-factory
    COMMAND pulp-test-editor-ui-package-host
        "$<TARGET_FILE:PulpEditorUiA_CLAP>=editor:a"
        "$<TARGET_FILE:PulpEditorUiB_CLAP>=editor:b"
        "$<TARGET_FILE:PulpEditorUiNone_CLAP>=editor:none"
        "$<TARGET_FILE:PulpEditorUiPlain_CLAP>=editor:none")
# The host also opens each UI package plug-in's GUI through the contract,
# which plug-in editors refuse in CI and headless environments, and on X11
# platforms without a display. The tagged editors never reach a display server,
# so a placeholder DISPLAY is enough there.
set(_editor_env
    "CI=unset:;PULP_HEADLESS=unset:;PULP_TEST_MODE=unset:;PULP_DISABLE_PLUGIN_EDITOR=unset:")
set(_x11 FALSE)
if(UNIX AND NOT APPLE AND NOT ANDROID AND NOT EMSCRIPTEN)
    set(_x11 TRUE)
endif()
if(_x11)
    set(_display_env "${_editor_env};DISPLAY=set::99;WAYLAND_DISPLAY=unset:")
else()
    set(_display_env "${_editor_env}")
endif()
set_tests_properties(editor-ui-package-per-plugin-factory PROPERTIES
    LABELS "format;editor-host" TIMEOUT 60
    ENVIRONMENT_MODIFICATION "${_display_env}")

# The production refusal still holds: on X11 platforms the same host with no
# display gets no GUI from the UI package plug-ins, and says so.
if(_x11)
    add_test(NAME editor-ui-package-no-display-refusal-control
        COMMAND pulp-test-editor-ui-package-host
            "$<TARGET_FILE:PulpEditorUiA_CLAP>=editor:a"
            "$<TARGET_FILE:PulpEditorUiB_CLAP>=editor:b")
    set_tests_properties(editor-ui-package-no-display-refusal-control PROPERTIES
        LABELS "format;editor-host" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "${_editor_env};DISPLAY=unset:;WAYLAND_DISPLAY=unset:"
        PASS_REGULAR_EXPRESSION "FAIL [^\n]*offers no GUI")
endif()

# Positive control for the view-free check: the same processor with the
# default editor, which links Pulp's view layer. Only its configured target
# graph is read, so it is not built.
pulp_add_plugin(PulpEditorUiViewControl
    FORMATS           CLAP
    PLUGIN_NAME       "Pulp Editor UI View Control"
    BUNDLE_ID         "dev.pulp.test.editor-ui-package-view-control"
    MANUFACTURER      "Pulp"
    VERSION           "1.0.0"
    CATEGORY          Effect
)
get_property(_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_t IN LISTS _targets)
    if(_t MATCHES "^PulpEditorUiViewControl_")
        set_target_properties(${_t} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()
endforeach()

# The configured target graph of each plug-in, with the plug-in name factored
# out so they compare directly. Recorded at configure time, before dead
# stripping could hide an extra link item.
set(_record "${CMAKE_CURRENT_BINARY_DIR}/editor_ui_package_link_record.cmake")
set(_record_content "")
foreach(_variant A B None Plain ViewControl)
    set(_plugin PulpEditorUi${_variant})
    pulp_link_closure(${_plugin}_CLAP TARGETS_OUT _closure)
    get_target_property(_sources ${_plugin}_CLAP SOURCES)
    get_target_property(_package ${_plugin}_CLAP PULP_UI_PACKAGE)
    foreach(_list _closure _sources)
        string(REPLACE "${_plugin}" "<plugin>" ${_list} "${${_list}}")
    endforeach()
    string(APPEND _record_content
        "set(${_variant}_CLOSURE [==[${_closure}]==])\n"
        "set(${_variant}_SOURCES [==[${_sources}]==])\n"
        "set(${_variant}_PACKAGE [==[${_package}]==])\n")
endforeach()
# The AU component of the UI package plug-in, checked like its CLAP module.
if(TARGET PulpEditorUiA_AU)
    pulp_link_closure(PulpEditorUiA_AU TARGETS_OUT _closure)
    string(REPLACE "PulpEditorUiA" "<plugin>" _closure "${_closure}")
    string(APPEND _record_content "set(AU_CLOSURE [==[${_closure}]==])\n")
endif()
file(WRITE "${_record}" "${_record_content}")

add_test(NAME editor-ui-package-none-links-nothing
    COMMAND ${CMAKE_COMMAND} -DRECORD=${_record}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_ui_none_links_nothing.cmake)
set_tests_properties(editor-ui-package-none-links-nothing PROPERTIES
    LABELS "format;editor-host;cmake" TIMEOUT 60)

add_test(NAME editor-ui-package-links-no-view
    COMMAND ${CMAKE_COMMAND} -DRECORD=${_record}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_ui_package_links_no_view.cmake)
set_tests_properties(editor-ui-package-links-no-view PROPERTIES
    LABELS "format;editor-host;cmake" TIMEOUT 60)

# The editor hook stays inside its module: the built UI package module exports
# none of the hook symbols, checked on every platform with an nm (Linux
# included, where hidden visibility and the version script are what keep it
# in). The control runs the same check with the module's own entry point as
# the forbidden name and must fail, showing the check can see an export.
if(NOT WIN32 AND CMAKE_NM)
    set(_hook_check ${CMAKE_CURRENT_SOURCE_DIR}/check_hook_not_exported.cmake)
    add_test(NAME editor-ui-package-hook-not-exported
        COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM} -DBINARY=$<TARGET_FILE:PulpEditorUiA_CLAP>
            "-DFORBIDDEN=plugin_editor_factory" -DREQUIRED=clap_entry -P ${_hook_check})
    add_test(NAME editor-ui-package-hook-export-check-control
        COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM} -DBINARY=$<TARGET_FILE:PulpEditorUiA_CLAP>
            "-DFORBIDDEN=clap_entry" -DREQUIRED=clap_entry -P ${_hook_check})
    set_tests_properties(editor-ui-package-hook-not-exported
        editor-ui-package-hook-export-check-control PROPERTIES
        LABELS "format;editor-host;cmake" TIMEOUT 60)
    # The control must fail for the right reason: the check finding the entry
    # point it was told is forbidden. A bare WILL_FAIL would also pass on a
    # missing module or a broken nm, which would hide a blind check.
    set_tests_properties(editor-ui-package-hook-export-check-control PROPERTIES
        PASS_REGULAR_EXPRESSION "exported editor-hook symbol: _?clap_entry")
endif()
