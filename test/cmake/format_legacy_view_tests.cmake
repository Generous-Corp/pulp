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

# The object library is part of the default build of the tests, so a compile
# failure fails the build; this test records that it compiled.
add_test(NAME format-core-ui-free-compile
    COMMAND ${CMAKE_COMMAND} -E echo
        "format.cpp compiled with PULP_FORMAT_LEGACY_VIEW=0 under the view poison")
set_tests_properties(format-core-ui-free-compile PROPERTIES
    LABELS "format"
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
