# PulpTestSuite.cmake — shared helper for declaring Catch2 unit-test binaries.
#
# Extracted in the 2026-05 Phase 3 (R2-7) refactor to slim down
# test/CMakeLists.txt (2,648 lines, 289 add_executable() blocks). The
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
# Validation goal (Codex risk callout): normalized `ctest -N -V` output
# stays identical for every migrated test. The helper always uses the
# `pulp-test-<name>` convention and always links Catch2WithMain, so
# discovered test names and properties are preserved byte-for-byte
# unless an EXTRA_PROPERTIES override is passed in.

include_guard(GLOBAL)

# pulp_add_test_suite(NAME
#     [SOURCES src1 src2 ...]            # default: derived "<NAME>.cpp" stripped of leading "pulp-test-"
#     [LIBRARIES lib1 lib2 ...]          # additional Pulp / system libraries (Catch2WithMain is always linked)
#     [LABELS "label1;label2"]           # catch_discover_tests PROPERTIES LABELS
#     [TIMEOUT seconds]                  # catch_discover_tests PROPERTIES TIMEOUT
#     [PROPERTIES name value ...]        # additional catch_discover_tests PROPERTIES
#     [INCLUDE_DIRS dir1 dir2 ...]       # extra include directories
#     [COMPILE_DEFINITIONS def1 ...]     # extra compile definitions
# )
function(pulp_add_test_suite NAME)
    set(options "")
    set(oneValueArgs LABELS TIMEOUT)
    set(multiValueArgs SOURCES LIBRARIES INCLUDE_DIRS COMPILE_DEFINITIONS PROPERTIES)
    cmake_parse_arguments(P "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

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
    set(_discover_args "")
    if(P_LABELS OR P_TIMEOUT OR P_PROPERTIES)
        list(APPEND _discover_args PROPERTIES)
        if(P_LABELS)
            list(APPEND _discover_args LABELS "${P_LABELS}")
        endif()
        if(P_TIMEOUT)
            list(APPEND _discover_args TIMEOUT "${P_TIMEOUT}")
        endif()
        if(P_PROPERTIES)
            list(APPEND _discover_args ${P_PROPERTIES})
        endif()
    endif()
    catch_discover_tests(${NAME} ${_discover_args})
endfunction()
