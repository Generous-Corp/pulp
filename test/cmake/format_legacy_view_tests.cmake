# Processor's legacy view hooks under PULP_FORMAT_LEGACY_VIEW (processor.hpp).
#
# An SDK whose UI comes from Vellum builds format-core with the macro 0: the
# view-named virtuals keep their slots under reserved names and opaque types.
# These tests hold the two promises that makes, in the default build, with no
# Vellum SDK:
#
#   format-legacy-view-slot-layout   the vtable layout is the same in both
#       modes. A probe prints slot positions built both ways, plus once against
#       a processor.hpp with one legacy slot removed, which must move them.
#   format-core-ui-free-compile      format.cpp and processor.hpp compile with
#       the macro 0, a forced-include poison that rejects any use of the
#       pulp::view namespace, and no pulp/view header on the include path. A
#       compile failure fails the build of pulp-test-format-core-ui-free.
#   format-core-poison-control       the same poison with the macro 1 must fail
#       to compile, and must fail on the poison: the control's include path does
#       have the view headers, so a missing include cannot pass it.
#
# The probe reads Itanium member function pointers, so these are not
# registered under MSVC.

if(MSVC OR NOT TARGET pulp-format-core)
    return()
endif()

set(_legacy_view_dir "${CMAKE_SOURCE_DIR}/test/format_legacy_view")
set(_legacy_view_poison "${_legacy_view_dir}/view_poison.hpp")

# Headers and definitions only: these never link format-core, so the probes
# see exactly what a consumer's compiler sees in each mode.
function(_pulp_legacy_view_usage target)
    target_include_directories(${target} PRIVATE
        $<TARGET_PROPERTY:pulp-format-core,INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_definitions(${target} PRIVATE
        $<TARGET_PROPERTY:pulp-format-core,INTERFACE_COMPILE_DEFINITIONS>)
    target_compile_features(${target} PRIVATE cxx_std_20)
endfunction()

add_executable(pulp-test-format-slot-probe-legacy "${_legacy_view_dir}/slot_probe.cpp")
_pulp_legacy_view_usage(pulp-test-format-slot-probe-legacy)
target_compile_definitions(pulp-test-format-slot-probe-legacy PRIVATE PULP_FORMAT_LEGACY_VIEW=1)

add_executable(pulp-test-format-slot-probe-ui-free "${_legacy_view_dir}/slot_probe.cpp")
_pulp_legacy_view_usage(pulp-test-format-slot-probe-ui-free)
target_compile_definitions(pulp-test-format-slot-probe-ui-free PRIVATE
    PULP_FORMAT_LEGACY_VIEW=0 PULP_TEST_REQUIRE_NO_VIEW_INCLUDE_PATH=1)
target_compile_options(pulp-test-format-slot-probe-ui-free PRIVATE
    -include "${_legacy_view_poison}")

# The control header: processor.hpp without the on_view_opened slot. Configure
# fails if that line is gone, so the control can never silently become a copy.
set(_legacy_view_shadow "${CMAKE_CURRENT_BINARY_DIR}/format_legacy_view_shadow")
file(READ "${CMAKE_SOURCE_DIR}/core/format/include/pulp/format/processor.hpp" _processor_hpp)
string(REGEX MATCH "\n[^\n]*virtual void PULP_FORMAT_LEGACY_VIEW_NAME\\(on_view_opened\\)[^\n]*"
    _legacy_view_slot "${_processor_hpp}")
if(NOT _legacy_view_slot)
    message(FATAL_ERROR "format_legacy_view_tests: the on_view_opened slot was not found in "
        "processor.hpp; update the shifted-layout control")
endif()
string(REPLACE "${_legacy_view_slot}" "" _processor_hpp "${_processor_hpp}")
file(WRITE "${_legacy_view_shadow}/processor.hpp.in" "${_processor_hpp}")
configure_file("${_legacy_view_shadow}/processor.hpp.in"
    "${_legacy_view_shadow}/pulp/format/processor.hpp" COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/core/format/include/pulp/format/processor.hpp")

add_executable(pulp-test-format-slot-probe-shifted "${_legacy_view_dir}/slot_probe.cpp")
target_include_directories(pulp-test-format-slot-probe-shifted BEFORE PRIVATE
    "${_legacy_view_shadow}")
_pulp_legacy_view_usage(pulp-test-format-slot-probe-shifted)
target_compile_definitions(pulp-test-format-slot-probe-shifted PRIVATE PULP_FORMAT_LEGACY_VIEW=0)

add_test(NAME format-legacy-view-slot-layout
    COMMAND ${CMAKE_COMMAND}
        -DLEGACY=$<TARGET_FILE:pulp-test-format-slot-probe-legacy>
        -DUI_FREE=$<TARGET_FILE:pulp-test-format-slot-probe-ui-free>
        -DSHIFTED=$<TARGET_FILE:pulp-test-format-slot-probe-shifted>
        -P "${_legacy_view_dir}/compare_slots.cmake")
set_tests_properties(format-legacy-view-slot-layout PROPERTIES
    LABELS "format"
    PULP_SELECT_WITH_TARGETS "pulp-format-core")

# format.cpp is the one format-core source that included a view header.
add_library(pulp-test-format-core-ui-free OBJECT
    "${CMAKE_SOURCE_DIR}/core/format/src/format.cpp")
_pulp_legacy_view_usage(pulp-test-format-core-ui-free)
target_compile_definitions(pulp-test-format-core-ui-free PRIVATE
    PULP_FORMAT_LEGACY_VIEW=0 PULP_TEST_REQUIRE_NO_VIEW_INCLUDE_PATH=1)
target_compile_options(pulp-test-format-core-ui-free PRIVATE -include "${_legacy_view_poison}")

# The object library builds with the tests, so a compile failure fails the
# build. This test checks the build product: the object exists and is newer
# than the sources it compiles, so a build that skipped the target fails here.
add_test(NAME format-core-ui-free-compile
    COMMAND ${CMAKE_COMMAND}
        "-DOBJECTS=$<TARGET_OBJECTS:pulp-test-format-core-ui-free>"
        "-DINPUTS=${CMAKE_SOURCE_DIR}/core/format/src/format.cpp;${CMAKE_SOURCE_DIR}/core/format/include/pulp/format/processor.hpp"
        -P "${_legacy_view_dir}/check_objects_built.cmake")
set_tests_properties(format-core-ui-free-compile PROPERTIES
    LABELS "format"
    PULP_SELECT_WITH_TARGETS "pulp-format-core")

# A macro-0 consumer linked against this build's pulp-format-core (built with
# Pulp's view layer) must fail to link on the marker that names the mismatch.
# Built only by the test, since its link is meant to fail.
add_executable(pulp-test-format-mode-mismatch-consumer EXCLUDE_FROM_ALL
    "${_legacy_view_dir}/mode_mismatch_consumer.cpp")
target_link_libraries(pulp-test-format-mode-mismatch-consumer PRIVATE pulp::format-core)
target_compile_definitions(pulp-test-format-mode-mismatch-consumer PRIVATE PULP_FORMAT_LEGACY_VIEW=0)
add_test(NAME format-core-mode-mismatch-link
    COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}"
        --target pulp-test-format-mode-mismatch-consumer -j 1)
set_tests_properties(format-core-mode-mismatch-link PROPERTIES
    LABELS "format"
    PASS_REGULAR_EXPRESSION "pulp_format_core_was_built_with_PULP_FORMAT_LEGACY_VIEW_0"
    PULP_SELECT_WITH_TARGETS "pulp-format-core")

if(TARGET pulp-view-core)
    add_library(pulp-test-format-core-poison-control OBJECT EXCLUDE_FROM_ALL
        "${CMAKE_SOURCE_DIR}/core/format/src/format.cpp")
    _pulp_legacy_view_usage(pulp-test-format-core-poison-control)
    target_include_directories(pulp-test-format-core-poison-control PRIVATE
        $<TARGET_PROPERTY:pulp-view-core,INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_definitions(pulp-test-format-core-poison-control PRIVATE
        PULP_FORMAT_LEGACY_VIEW=1)
    target_compile_options(pulp-test-format-core-poison-control PRIVATE
        -include "${_legacy_view_poison}")
    add_test(NAME format-core-poison-control
        COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}"
            --target pulp-test-format-core-poison-control -j 1)
    set_tests_properties(format-core-poison-control PROPERTIES
        LABELS "format"
        PASS_REGULAR_EXPRESSION "pulp_format_core_must_not_name_view"
        FAIL_REGULAR_EXPRESSION "file not found|No such file or directory"
        PULP_SELECT_WITH_TARGETS "pulp-format-core")
endif()

# The format adapters in the same mode. The VST3 and CLAP editor-host contract
# tests are compiled again with every adapter source and format.cpp at
# PULP_FORMAT_LEGACY_VIEW=0, under the view poison, with no pulp/view header
# reachable: the adapters an SDK with Vellum UI ships, opening a
# Processor::editor_factory() editor with nothing of Pulp's view behind them.
# format-adapters-ui-free-symbols checks each binary really holds the mode-0
# Processor (its reserved create_view slot, not the legacy one), with the
# mode-1 build of the same test as the control.
set(_ui_free_adapter_pairs "")
function(_pulp_ui_free_adapter_test target)
    cmake_parse_arguments(PARSE_ARGV 1 _arg "" "TEST;LEGACY" "SOURCES;LIBRARIES;DEFINITIONS")
    add_executable(${target} "${CMAKE_SOURCE_DIR}/test/${_arg_TEST}"
        "${CMAKE_SOURCE_DIR}/core/format/src/format.cpp" ${_arg_SOURCES})
    target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}/test")
    target_link_libraries(${target} PRIVATE ${_arg_LIBRARIES} Catch2::Catch2WithMain)
    target_compile_definitions(${target} PRIVATE
        PULP_FORMAT_LEGACY_VIEW=0 PULP_TEST_REQUIRE_NO_VIEW_INCLUDE_PATH=1 ${_arg_DEFINITIONS})
    target_compile_options(${target} PRIVATE -include "${_legacy_view_poison}")
    catch_discover_tests(${target} TEST_PREFIX "ui-free." PROPERTIES LABELS "format")
    set(_ui_free_adapter_pairs ${_ui_free_adapter_pairs}
        "$<TARGET_FILE:${target}>|$<TARGET_FILE:${_arg_LEGACY}>" PARENT_SCOPE)
endfunction()

set(_ui_free_format_src "${CMAKE_SOURCE_DIR}/core/format/src")
if(PULP_HAS_VST3 AND TARGET pulp-test-vst3-contract-editor)
    set(_vst3_sources
        "${_ui_free_format_src}/vst3_adapter.cpp"
        "${_ui_free_format_src}/vst3_contract_view.cpp")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND _vst3_sources "${_ui_free_format_src}/vst3_host_run_loop.cpp")
    endif()
    _pulp_ui_free_adapter_test(pulp-test-vst3-contract-editor-ui-free
        TEST test_vst3_contract_editor.cpp
        LEGACY pulp-test-vst3-contract-editor
        SOURCES ${_vst3_sources}
        LIBRARIES pulp::format-core
        DEFINITIONS PULP_VST3=1 PULP_VST3_GUI=1)
endif()
if(PULP_HAS_CLAP AND TARGET pulp-test-clap-contract-editor)
    _pulp_ui_free_adapter_test(pulp-test-clap-contract-editor-ui-free
        TEST test_clap_contract_editor.cpp
        LEGACY pulp-test-clap-contract-editor
        SOURCES
            "${_ui_free_format_src}/clap_adapter.cpp"
            "${_ui_free_format_src}/clap_contract_gui.cpp"
            "${_ui_free_format_src}/clap_host_run_loop.cpp"
            "${_ui_free_format_src}/clap_remote_controls.cpp"
            "${_ui_free_format_src}/clap_note_name.cpp"
        LIBRARIES pulp::format-core clap
        DEFINITIONS PULP_CLAP=1)
    if(PULP_HAS_AUSDK)
        set_target_properties(pulp-test-clap-contract-editor-ui-free PROPERTIES CXX_STANDARD 23)
    endif()
endif()
if(_ui_free_adapter_pairs AND CMAKE_NM)
    add_test(NAME format-adapters-ui-free-symbols
        COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM} "-DPAIRS=${_ui_free_adapter_pairs}"
            -P "${_legacy_view_dir}/check_ui_free_symbols.cmake")
    set_tests_properties(format-adapters-ui-free-symbols PROPERTIES
        LABELS "format"
        PULP_SELECT_WITH_TARGETS "pulp-format-core")
endif()
