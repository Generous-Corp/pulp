# PulpTestSuite.cmake — shared helper for declaring Catch2 unit-test binaries.
#
# Keeps test/CMakeLists.txt from owning thousands of repeated target lines. The
# common 3-line pattern
#
#     add_executable(pulp-test-X test_X.cpp)
#     target_link_libraries(pulp-test-X PRIVATE pulp::Y Catch2::Catch2WithMain)
#     catch_discover_tests(pulp-test-X)
#
# collapses to a single
#
#     pulp_add_test_suite(pulp-test-X LIBRARIES pulp::Y)
#
# saving 2 lines per migrated test plus consolidating the
# catch_discover_tests() wiring in one place so future Catch2-config
# changes only need to touch this helper.
#
# Validation goal: normalized `ctest -N -V` output stays identical for every
# migrated test. The helper always uses the
# `pulp-test-<name>` convention and always links Catch2WithMain, so
# discovered test names and properties are preserved byte-for-byte
# unless an EXTRA_PROPERTIES override is passed in.

include_guard(GLOBAL)

# pulp_add_test_suite(NAME
#     [SOURCES src1 src2 ...]            # default: derived "<NAME>.cpp" stripped of leading "pulp-test-"
#     [LIBRARIES lib1 lib2 ...]          # additional Pulp / system libraries (Catch2WithMain is always linked)
#     [TEST_SPEC "spec"]                 # catch_discover_tests TEST_SPEC
#     [LABELS "label1;label2"]           # catch_discover_tests PROPERTIES LABELS
#     [TIMEOUT seconds]                  # catch_discover_tests PROPERTIES TIMEOUT
#     [PROPERTIES name value ...]        # additional catch_discover_tests PROPERTIES
#     [DISCOVERY_ARGS arg1 arg2 ...]      # raw catch_discover_tests args
#     [INCLUDE_DIRS dir1 dir2 ...]       # extra include directories
#     [COMPILE_DEFINITIONS def1 ...]     # extra compile definitions
# )
function(pulp_add_test_suite NAME)
    set(options "")
    set(oneValueArgs TIMEOUT TEST_SPEC)
    set(multiValueArgs SOURCES LIBRARIES INCLUDE_DIRS COMPILE_DEFINITIONS PROPERTIES DISCOVERY_ARGS LABELS)
    cmake_parse_arguments(P "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    if(P_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "pulp_add_test_suite(${NAME}): unparsed arguments: ${P_UNPARSED_ARGUMENTS}")
    endif()

    # Default source file: strip the conventional "pulp-test-" prefix and
    # append .cpp. Most existing test targets follow this convention so
    # the SOURCES argument is rarely needed.
    if(NOT P_SOURCES)
        string(REGEX REPLACE "^pulp-test-" "" _stem "${NAME}")
        string(REPLACE "-" "_" _stem "${_stem}")
        set(P_SOURCES "test_${_stem}.cpp")
    endif()

    add_executable(${NAME} ${P_SOURCES})
    target_link_libraries(${NAME} PRIVATE ${P_LIBRARIES} Catch2::Catch2WithMain)

    if(P_INCLUDE_DIRS)
        target_include_directories(${NAME} PRIVATE ${P_INCLUDE_DIRS})
    endif()
    if(P_COMPILE_DEFINITIONS)
        target_compile_definitions(${NAME} PRIVATE ${P_COMPILE_DEFINITIONS})
    endif()

    # Discover Catch2 cases. Pass LABELS / TIMEOUT / additional properties
    # through if set so downstream `ctest -L <label>` and per-suite timeouts
    # keep working.
    set(_discover_args ${P_DISCOVERY_ARGS})
    if(P_TEST_SPEC)
        list(APPEND _discover_args TEST_SPEC "${P_TEST_SPEC}")
    endif()
    if(P_LABELS OR P_TIMEOUT OR P_PROPERTIES)
        if(P_TIMEOUT OR P_PROPERTIES)
            list(APPEND _discover_args PROPERTIES)
            if(P_TIMEOUT)
                list(APPEND _discover_args TIMEOUT "${P_TIMEOUT}")
            endif()
            if(P_PROPERTIES)
                list(APPEND _discover_args ${P_PROPERTIES})
            endif()
        endif()
        if(P_LABELS)
            # LABELS is a first-class multi-value argument in PulpCatch.cmake.
            # Keep it last so arbitrary CTest property names cannot terminate it.
            list(APPEND _discover_args LABELS ${P_LABELS})
        endif()
    endif()
    catch_discover_tests(${NAME} ${_discover_args})
endfunction()
