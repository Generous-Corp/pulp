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

include(${CMAKE_CURRENT_LIST_DIR}/PulpTestTimeout.cmake)

# ---------------------------------------------------------------------------
# Test groups: several suites, one executable
#
# Every Catch2 executable statically links the whole pulp::view stack, so a
# manifest of N one-file suites costs N links of the same ~150 MiB of archives,
# N copies of them on disk, and N relinks whenever one core .cpp changes.
# A group is one executable that several suites compile into. Each suite keeps
# its own `pulp_add_test_suite` call, its own LABELS / TIMEOUT / PROPERTIES,
# and its own discovered test names: the group runs Catch2 with
# `--filenames-as-tags`, which tags every case with `[#<source stem>]`, and
# each member's discovery is scoped to its own sources' tags. CTest therefore
# sees exactly the tests and properties it saw before; only the executable
# behind them changes.
#
#     pulp_add_test_group(pulp-test-group-view LIBRARIES pulp::view pulp::state)
#     pulp_add_test_suite(pulp-test-widgets GROUP pulp-test-group-view)
#     pulp_add_test_suite(pulp-test-text-editor-mouse GROUP pulp-test-group-view
#         PROPERTIES RESOURCE_LOCK system-clipboard)
#
# Two constraints follow from sharing one process and one compile line:
#   * Catch2 refuses two cases with the same name, class and tags in one
#     binary, and CTest would register the same name twice even when the tags
#     differ. Rename one of them before grouping.
#   * A member may only name libraries the group already links. The group's
#     LIBRARIES define the compile line every member gets; a member that needs
#     something more belongs in a group that declares it (or on its own).
# Suites that need process isolation — a custom main(), a fixture they spawn
# by path, a codesign step on the binary, `-fno-exceptions`, RT allocation
# probes — stay ungrouped.
#
# A member whose tag expression matches no case would otherwise vanish from
# CTest without a diagnostic, so group discovery uses FAIL_IF_EMPTY and the
# build fails instead.
# ---------------------------------------------------------------------------

# pulp_add_test_group(NAME
#     [LIBRARIES lib1 lib2 ...]          # linked once; every member compiles against them
#     [INCLUDE_DIRS dir1 dir2 ...]
#     [COMPILE_DEFINITIONS def1 ...]
# )
function(pulp_add_test_group NAME)
    set(options "")
    set(oneValueArgs "")
    set(multiValueArgs LIBRARIES INCLUDE_DIRS COMPILE_DEFINITIONS)
    cmake_parse_arguments(G "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    if(G_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "pulp_add_test_group(${NAME}): unparsed arguments: ${G_UNPARSED_ARGUMENTS}")
    endif()
    if(TARGET ${NAME})
        message(FATAL_ERROR "pulp_add_test_group(${NAME}): target already exists")
    endif()

    # Sources arrive through the members; a group nobody joins fails at
    # generate time with CMake's own "no sources" error, which is the right
    # outcome for a manifest that lost all its members to an if() guard.
    add_executable(${NAME})
    target_link_libraries(${NAME} PRIVATE ${G_LIBRARIES} Catch2::Catch2WithMain)
    if(G_INCLUDE_DIRS)
        target_include_directories(${NAME} PRIVATE ${G_INCLUDE_DIRS})
    endif()
    if(G_COMPILE_DEFINITIONS)
        target_compile_definitions(${NAME} PRIVATE ${G_COMPILE_DEFINITIONS})
    endif()
    set_target_properties(${NAME} PROPERTIES
        PULP_TEST_GROUP TRUE
        PULP_TEST_GROUP_LIBRARIES "${G_LIBRARIES}"
        PULP_TEST_GROUP_MEMBERS ""
        PULP_TEST_GROUP_SOURCES "")
endfunction()

# Adds a member suite's sources to its group and returns, in ${out_spec}, the
# Catch2 test-spec list that selects exactly that member's cases:
# `-#` (filenames as tags) followed by one `[#<stem>]<member_spec>` term per
# source, OR-ed with commas.
function(_pulp_test_group_attach GROUP MEMBER out_spec)
    set(options "")
    set(oneValueArgs TEST_SPEC)
    set(multiValueArgs SOURCES LIBRARIES INCLUDE_DIRS COMPILE_DEFINITIONS)
    cmake_parse_arguments(M "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT TARGET ${GROUP})
        message(FATAL_ERROR
            "pulp_add_test_suite(${MEMBER}): GROUP ${GROUP} is not a target; "
            "declare it first with pulp_add_test_group()")
    endif()
    get_target_property(_is_group ${GROUP} PULP_TEST_GROUP)
    if(NOT _is_group)
        message(FATAL_ERROR
            "pulp_add_test_suite(${MEMBER}): GROUP ${GROUP} was not created by "
            "pulp_add_test_group()")
    endif()

    get_target_property(_group_libs ${GROUP} PULP_TEST_GROUP_LIBRARIES)
    foreach(_lib IN LISTS M_LIBRARIES)
        if(NOT _lib IN_LIST _group_libs)
            message(FATAL_ERROR
                "pulp_add_test_suite(${MEMBER}): links ${_lib}, which group ${GROUP} "
                "does not. Every member compiles against the group's LIBRARIES; add "
                "${_lib} to the group or keep ${MEMBER} out of it.")
        endif()
    endforeach()

    if(M_TEST_SPEC MATCHES ",")
        message(FATAL_ERROR
            "pulp_add_test_suite(${MEMBER}): a grouped suite's TEST_SPEC must be a "
            "single tag expression (no ','); the group scopes it per source file")
    endif()

    get_target_property(_known_sources ${GROUP} PULP_TEST_GROUP_SOURCES)
    set(_terms "")
    foreach(_src IN LISTS M_SOURCES)
        get_filename_component(_abs "${_src}" ABSOLUTE)
        if(NOT _abs IN_LIST _known_sources)
            target_sources(${GROUP} PRIVATE "${_abs}")
            list(APPEND _known_sources "${_abs}")
        endif()
        if(M_COMPILE_DEFINITIONS)
            set_property(SOURCE "${_abs}" APPEND PROPERTY
                COMPILE_DEFINITIONS ${M_COMPILE_DEFINITIONS})
        endif()
        if(M_INCLUDE_DIRS)
            set_property(SOURCE "${_abs}" APPEND PROPERTY
                INCLUDE_DIRECTORIES ${M_INCLUDE_DIRS})
        endif()
        get_filename_component(_stem "${_abs}" NAME_WLE)
        list(APPEND _terms "[#${_stem}]${M_TEST_SPEC}")
    endforeach()
    set_property(TARGET ${GROUP} PROPERTY PULP_TEST_GROUP_SOURCES "${_known_sources}")
    set_property(TARGET ${GROUP} APPEND PROPERTY PULP_TEST_GROUP_MEMBERS "${MEMBER}")

    list(JOIN _terms "," _expr)
    set(${out_spec} "-#" "${_expr}" PARENT_SCOPE)
endfunction()

# pulp_add_test_suite(NAME
#     [GROUP group]                      # compile into this pulp_add_test_group() executable
#     [SOURCES src1 src2 ...]            # default: derived "<NAME>.cpp" stripped of leading "pulp-test-"
#     [LIBRARIES lib1 lib2 ...]          # additional Pulp / system libraries (Catch2WithMain is always linked)
#     [TEST_SPEC "spec"]                 # catch_discover_tests TEST_SPEC
#     [TEST_PREFIX "prefix"]             # catch_discover_tests TEST_PREFIX
#     [LABELS "label1;label2"]           # catch_discover_tests PROPERTIES LABELS
#     [TIMEOUT seconds]                  # catch_discover_tests PROPERTIES TIMEOUT
#     [PROPERTIES name value ...]        # additional catch_discover_tests PROPERTIES
#     [DISCOVERY_ARGS arg1 arg2 ...]      # raw catch_discover_tests args
#     [INCLUDE_DIRS dir1 dir2 ...]       # extra include directories
#     [COMPILE_DEFINITIONS def1 ...]     # extra compile definitions
# )
function(pulp_add_test_suite NAME)
    set(options "")
    set(oneValueArgs TIMEOUT TEST_SPEC TEST_PREFIX GROUP)
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

    set(_spec_args "")
    if(P_TEST_SPEC)
        set(_spec_args "${P_TEST_SPEC}")
    endif()

    if(P_GROUP)
        set(_target "${P_GROUP}")
        _pulp_test_group_attach(${P_GROUP} ${NAME} _spec_args
            SOURCES ${P_SOURCES}
            LIBRARIES ${P_LIBRARIES}
            INCLUDE_DIRS ${P_INCLUDE_DIRS}
            COMPILE_DEFINITIONS ${P_COMPILE_DEFINITIONS}
            TEST_SPEC "${P_TEST_SPEC}")
    else()
        set(_target "${NAME}")
        add_executable(${NAME} ${P_SOURCES})
        target_link_libraries(${NAME} PRIVATE ${P_LIBRARIES} Catch2::Catch2WithMain)

        if(P_INCLUDE_DIRS)
            target_include_directories(${NAME} PRIVATE ${P_INCLUDE_DIRS})
        endif()
        if(P_COMPILE_DEFINITIONS)
            target_compile_definitions(${NAME} PRIVATE ${P_COMPILE_DEFINITIONS})
        endif()
    endif()

    # Discover Catch2 cases. Pass LABELS / TIMEOUT / additional properties
    # through if set so downstream `ctest -L <label>` and per-suite timeouts
    # keep working.
    set(_discover_args ${P_DISCOVERY_ARGS})
    if(_spec_args)
        list(APPEND _discover_args TEST_SPEC ${_spec_args})
    endif()
    if(P_GROUP)
        list(APPEND _discover_args FAIL_IF_EMPTY)
    endif()
    if(P_TEST_PREFIX)
        list(APPEND _discover_args TEST_PREFIX "${P_TEST_PREFIX}")
    endif()
    if(P_LABELS OR P_TIMEOUT OR P_PROPERTIES)
        if(P_TIMEOUT OR P_PROPERTIES)
            list(APPEND _discover_args PROPERTIES)
            if(P_TIMEOUT)
                # Budgets are authored for an optimized build; a coverage tree
                # runs the same work at -O0 with instrumentation. Scaling here
                # keeps every suite's declared TIMEOUT honest in both without
                # each manifest having to know the build config.
                pulp_scaled_test_timeout(_pulp_timeout "${P_TIMEOUT}")
                list(APPEND _discover_args TIMEOUT "${_pulp_timeout}")
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
    catch_discover_tests(${_target} ${_discover_args})
endfunction()
