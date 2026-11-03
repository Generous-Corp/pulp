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

foreach(_variant A B None Plain)
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
        FORMATS           CLAP
        PLUGIN_NAME       "Pulp Editor UI ${_variant}"
        BUNDLE_ID         "dev.pulp.test.editor-ui-package"
        MANUFACTURER      "Pulp"
        VERSION           "1.0.0"
        CATEGORY          Effect
    )
endforeach()

add_executable(pulp-test-editor-ui-package-host editor_ui_package_host.cpp)
target_link_libraries(pulp-test-editor-ui-package-host PRIVATE ${CMAKE_DL_LIBS})
add_dependencies(pulp-test-editor-ui-package-host
    PulpEditorUiA_CLAP PulpEditorUiB_CLAP PulpEditorUiNone_CLAP PulpEditorUiPlain_CLAP)

add_test(NAME editor-ui-package-per-plugin-factory
    COMMAND pulp-test-editor-ui-package-host
        "$<TARGET_FILE:PulpEditorUiA_CLAP>=editor:a"
        "$<TARGET_FILE:PulpEditorUiB_CLAP>=editor:b"
        "$<TARGET_FILE:PulpEditorUiNone_CLAP>=editor:none"
        "$<TARGET_FILE:PulpEditorUiPlain_CLAP>=editor:none")
set_tests_properties(editor-ui-package-per-plugin-factory PROPERTIES
    LABELS "format;editor-host" TIMEOUT 60)

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
