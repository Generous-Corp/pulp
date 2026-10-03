# PulpTestData.cmake — declared runtime data inputs for compiled tests.
#
# A compiled test that opens files from the checkout at run time reads inputs
# the build graph never sees: change the fixture and every object and link
# stays up to date, yet the test's verdict can move. A selector that reuses a
# test result by its build inputs alone would then skip a test whose data
# changed. So a test that reads the checkout declares what it reads:
#
#     pulp_add_test_suite(pulp-test-modal-spec ...)
#     pulp_test_data(pulp-test-modal-spec PATHS examples/modal-specs)
#
# pulp_test_data(<suite-or-executable>
#     PATHS <repo-relative path or glob> ... | NONE
#     [SOURCES <source> ...]                   # default: all of the suite's
#     [DEFINE <macro> | NO_DEFINE]             # default DEFINE PULP_SOURCE_DIR
# )
#
# It defines <macro> as the checkout root for the declaring sources (the
# COMPILE_DEFINITIONS PULP_SOURCE_DIR=... it replaces) and records the paths.
# NO_DEFINE records the paths and leaves the target's own definitions alone,
# for a test that reaches its data through another definition (a fixture
# directory, PULP_REPO_ROOT) or by walking up from its working directory.
# NONE records a reviewed source that opens nothing in the checkout at run
# time, so text that merely names test/fixtures (a comment, a staged temp
# fixture) does not leave its executable undeclared.
# Every path must match something in the checkout at configure time, so a
# typo or a moved fixture fails the configure instead of declaring nothing.
#
# The suite argument is either a pulp_add_test_suite name (grouped or not;
# only that member's sources are declared) or any executable target.
# SOURCES narrows the declaration to some of those sources, for an executable
# whose other sources read data the declaration does not cover.
#
# At the end of the test directory two artifacts are written under
# ${CMAKE_BINARY_DIR}/test/test-data/:
#   <executable>.inputs.json  per executable with a declaration: the sources
#                             that declared and the union of their paths;
#   executables.json          every compiled test executable with its
#                             repo-relative sources and the names of the
#                             compile definitions whose value points into the
#                             checkout.
# executables.json also lists each executable's `runtime_targets`: the built
# executables and modules it depends on through add_dependencies, which is how
# a test that spawns or loads one at run time names it.
#
# A test that runs or loads another built target at run time declares it:
#
#     pulp_test_spawns(pulp-test-cli-import-design pulp-cli pulp-import-design)
#
# Test manifests are read before tools/cli, tools/import-design and examples,
# so an `if(TARGET <tool>)` written next to the test is false and the edge is
# silently never created. pulp_test_spawns() adds the edge once the whole tree
# has been read, and only for a tool this configuration builds.
#
#     pulp_test_spawns(pulp-test-group-native-platform NONE)
#
# records a reviewed executable whose sources call a process API (ChildProcess,
# popen, posix_spawn, fork, ...) yet run or load nothing this repo builds:
# system tools, or a fork of itself. tools/scripts/script_test_inputs.py marks
# an executable that calls one with neither an edge nor NONE `spawns:
# undeclared`, which a selector must never skip. The finalize
# step then fails the configure when a test executable's compile definitions
# name `$<TARGET_FILE:x>` but x is neither a dependency nor a linked library,
# because a selector working from the build graph cannot see that spawn.
#
# tools/scripts/script_test_inputs.py folds both into
# test/ctest_script_inputs.json as `kind: compiled` entries. A source that
# reads the checkout (it names PULP_SOURCE_DIR, test/fixtures, or one of its
# executable's checkout definitions) without a declaration marks its
# executable `data: undeclared`, which a selector must never skip.

include_guard(GLOBAL)

set(PULP_TEST_DATA_DIR "${CMAKE_BINARY_DIR}/test/test-data")

# Records a suite's executable and sources; called by pulp_add_test_suite.
function(_pulp_test_data_register_suite NAME EXECUTABLE)
    set(_abs "")
    foreach(_src IN LISTS ARGN)
        get_filename_component(_a "${_src}" ABSOLUTE)
        list(APPEND _abs "${_a}")
    endforeach()
    set_property(GLOBAL PROPERTY PULP_TEST_DATA_SUITE_EXE_${NAME} "${EXECUTABLE}")
    set_property(GLOBAL PROPERTY PULP_TEST_DATA_SUITE_SOURCES_${NAME} "${_abs}")
endfunction()

function(_pulp_test_data_rel out path)
    file(RELATIVE_PATH _rel "${CMAKE_SOURCE_DIR}" "${path}")
    if(_rel MATCHES "^\\.\\./" OR IS_ABSOLUTE "${_rel}")
        set(_rel "")
    endif()
    set(${out} "${_rel}" PARENT_SCOPE)
endfunction()

function(_pulp_test_data_json_list out)
    set(_items "")
    foreach(_v IN LISTS ARGN)
        string(REPLACE "\\" "\\\\" _v "${_v}")
        string(REPLACE "\"" "\\\"" _v "${_v}")
        list(APPEND _items "\"${_v}\"")
    endforeach()
    list(JOIN _items ", " _joined)
    set(${out} "[${_joined}]" PARENT_SCOPE)
endfunction()

function(pulp_test_data NAME)
    cmake_parse_arguments(D "NONE;NO_DEFINE" "DEFINE" "PATHS;SOURCES" ${ARGN})
    if(D_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "pulp_test_data(${NAME}): unparsed arguments: ${D_UNPARSED_ARGUMENTS}")
    endif()
    if(D_NONE AND D_PATHS)
        message(FATAL_ERROR "pulp_test_data(${NAME}): NONE and PATHS are exclusive")
    endif()
    if(NOT D_PATHS AND NOT D_NONE)
        message(FATAL_ERROR "pulp_test_data(${NAME}): PATHS (or NONE) is required")
    endif()
    if(D_NO_DEFINE AND D_DEFINE)
        message(FATAL_ERROR "pulp_test_data(${NAME}): DEFINE and NO_DEFINE are exclusive")
    endif()
    if(NOT D_DEFINE)
        set(D_DEFINE PULP_SOURCE_DIR)
    endif()

    get_property(_exe GLOBAL PROPERTY PULP_TEST_DATA_SUITE_EXE_${NAME})
    if(_exe)
        get_property(_sources GLOBAL PROPERTY PULP_TEST_DATA_SUITE_SOURCES_${NAME})
    elseif(TARGET ${NAME})
        get_target_property(_type ${NAME} TYPE)
        if(NOT _type STREQUAL "EXECUTABLE")
            message(FATAL_ERROR "pulp_test_data(${NAME}): not an executable (${_type})")
        endif()
        set(_exe ${NAME})
        get_target_property(_srcs ${NAME} SOURCES)
        get_target_property(_dir ${NAME} SOURCE_DIR)
        set(_sources "")
        foreach(_s IN LISTS _srcs)
            if(NOT _s MATCHES "^\\$<")
                get_filename_component(_a "${_s}" ABSOLUTE BASE_DIR "${_dir}")
                list(APPEND _sources "${_a}")
            endif()
        endforeach()
    else()
        message(FATAL_ERROR "pulp_test_data(${NAME}): no pulp_add_test_suite or executable "
                            "of that name; call it after the suite is declared")
    endif()

    if(D_SOURCES)
        set(_narrowed "")
        foreach(_s IN LISTS D_SOURCES)
            get_filename_component(_a "${_s}" ABSOLUTE)
            if(NOT _a IN_LIST _sources)
                message(FATAL_ERROR "pulp_test_data(${NAME}): '${_s}' is not one of its sources")
            endif()
            list(APPEND _narrowed "${_a}")
        endforeach()
        set(_sources "${_narrowed}")
    endif()

    foreach(_p IN LISTS D_PATHS)
        if(IS_ABSOLUTE "${_p}" OR _p MATCHES "(^|/)\\.\\.(/|$)")
            message(FATAL_ERROR "pulp_test_data(${NAME}): '${_p}' must be relative to the checkout root")
        endif()
        if(_p MATCHES "[*?[]")
            file(GLOB _hits LIST_DIRECTORIES true "${CMAKE_SOURCE_DIR}/${_p}")
        elseif(EXISTS "${CMAKE_SOURCE_DIR}/${_p}")
            set(_hits "${_p}")
        else()
            set(_hits "")
        endif()
        if(NOT _hits)
            message(FATAL_ERROR "pulp_test_data(${NAME}): '${_p}' matches nothing in the checkout")
        endif()
    endforeach()

    # A grouped member's definition goes on its own sources, so the macro
    # never leaks into its neighbours' translation units; a standalone
    # executable takes it target-wide, as its COMPILE_DEFINITIONS did.
    if(D_NO_DEFINE OR D_NONE)
        # The target keeps its own definitions.
    elseif(_exe STREQUAL NAME AND NOT D_SOURCES)
        target_compile_definitions(${_exe} PRIVATE "${D_DEFINE}=\"${CMAKE_SOURCE_DIR}\"")
    else()
        set_property(SOURCE ${_sources} APPEND PROPERTY
            COMPILE_DEFINITIONS "${D_DEFINE}=\"${CMAKE_SOURCE_DIR}\"")
    endif()

    set(_rel_sources "")
    foreach(_a IN LISTS _sources)
        _pulp_test_data_rel(_r "${_a}")
        if(_r)
            list(APPEND _rel_sources "${_r}")
        endif()
    endforeach()
    set_property(GLOBAL APPEND PROPERTY PULP_TEST_DATA_SOURCES_${_exe} ${_rel_sources})
    set_property(GLOBAL APPEND PROPERTY PULP_TEST_DATA_PATHS_${_exe} ${D_PATHS})
    get_property(_declared GLOBAL PROPERTY PULP_TEST_DATA_EXECUTABLES)
    if(NOT _exe IN_LIST _declared)
        set_property(GLOBAL APPEND PROPERTY PULP_TEST_DATA_EXECUTABLES ${_exe})
    endif()
endfunction()

function(_pulp_test_data_collect_targets dir out)
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_sub IN LISTS _subdirs)
        _pulp_test_data_collect_targets("${_sub}" _more)
        list(APPEND _targets ${_more})
    endforeach()
    set(${out} "${_targets}" PARENT_SCOPE)
endfunction()

# A definition value that names the checkout (and not the build tree, which
# may live inside it) is a path the executable can open at run time.
function(_pulp_test_data_tree_defines out defs)
    set(_names "")
    foreach(_d IN LISTS defs)
        if(_d MATCHES "^([A-Za-z_][A-Za-z0-9_]*)=(.*)$")
            set(_name "${CMAKE_MATCH_1}")
            set(_value "${CMAKE_MATCH_2}")
            string(FIND "${_value}" "${CMAKE_BINARY_DIR}" _in_build)
            string(FIND "${_value}" "${CMAKE_SOURCE_DIR}" _in_tree)
            if(_in_tree GREATER -1 AND _in_build EQUAL -1)
                list(APPEND _names "${_name}")
            endif()
        endif()
    endforeach()
    set(${out} "${_names}" PARENT_SCOPE)
endfunction()

# The built executables and modules TARGET depends on through
# add_dependencies (`runtime`), and one error line per `$<TARGET_FILE:x>` in
# its definitions whose x it neither depends on nor links (`unbound`).
function(_pulp_test_data_runtime_targets runtime unbound TARGET defs)
    get_target_property(_manual ${TARGET} MANUALLY_ADDED_DEPENDENCIES)
    get_target_property(_linked ${TARGET} LINK_LIBRARIES)
    if(NOT _manual)
        set(_manual "")
    endif()
    if(NOT _linked)
        set(_linked "")
    endif()
    set(_run "")
    foreach(_dep IN LISTS _manual)
        if(TARGET ${_dep})
            get_target_property(_dep_type ${_dep} TYPE)
            if(_dep_type STREQUAL "EXECUTABLE" OR _dep_type STREQUAL "MODULE_LIBRARY")
                list(APPEND _run "${_dep}")
            endif()
        endif()
    endforeach()
    list(SORT _run)
    set(_errors "")
    string(REGEX MATCHALL "\\$<TARGET_FILE:[^>]+>" _files "${defs}")
    foreach(_file IN LISTS _files)
        string(REGEX REPLACE "^\\$<TARGET_FILE:([^>]+)>$" "\\1" _named "${_file}")
        if(NOT _named IN_LIST _manual AND NOT _named IN_LIST _linked)
            list(APPEND _errors "${TARGET} -> ${_named}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _errors)
    set(${runtime} "${_run}" PARENT_SCOPE)
    set(${unbound} "${_errors}" PARENT_SCOPE)
endfunction()

function(_pulp_test_data_finalize)
    # Runs deferred in the top-level directory, where the variable set when
    # this module was included under test/ is not in scope.
    set(PULP_TEST_DATA_DIR "${CMAKE_BINARY_DIR}/test/test-data")
    file(MAKE_DIRECTORY "${PULP_TEST_DATA_DIR}")
    set(_spawn_errors "")
    get_property(_spawns_none GLOBAL PROPERTY PULP_TEST_SPAWNS_NONE)
    _pulp_test_data_collect_targets("${CMAKE_SOURCE_DIR}/test" _targets)
    list(SORT _targets)
    set(_rows "")
    foreach(_t IN LISTS _targets)
        get_target_property(_type ${_t} TYPE)
        get_target_property(_imported ${_t} IMPORTED)
        if(NOT _type STREQUAL "EXECUTABLE" OR _imported)
            continue()
        endif()
        get_target_property(_srcs ${_t} SOURCES)
        get_target_property(_dir ${_t} SOURCE_DIR)
        get_target_property(_tdefs ${_t} COMPILE_DEFINITIONS)
        if(NOT _tdefs)
            set(_tdefs "")
        endif()
        set(_rels "")
        set(_defs "${_tdefs}")
        foreach(_s IN LISTS _srcs)
            if(_s MATCHES "^\\$<")
                continue()
            endif()
            get_filename_component(_a "${_s}" ABSOLUTE BASE_DIR "${_dir}")
            _pulp_test_data_rel(_r "${_a}")
            if(NOT _r OR NOT EXISTS "${_a}")
                continue()
            endif()
            list(APPEND _rels "${_r}")
            get_source_file_property(_sdefs "${_a}" TARGET_DIRECTORY ${_t} COMPILE_DEFINITIONS)
            if(_sdefs)
                list(APPEND _defs ${_sdefs})
            endif()
        endforeach()
        if(NOT _rels)
            continue()
        endif()
        list(REMOVE_DUPLICATES _rels)
        list(SORT _rels)
        _pulp_test_data_tree_defines(_names "${_defs}")
        _pulp_test_data_runtime_targets(_runtime _unbound ${_t} "${_defs}")
        list(APPEND _spawn_errors ${_unbound})
        # Every edge pulp_test_spawns() added must reach the index; one that
        # does not (a tool that is not an executable or module, or an edge
        # added after this ran) would otherwise vanish from the record.
        get_property(_declared_spawns GLOBAL PROPERTY PULP_TEST_SPAWNS_OF_${_t})
        foreach(_tool IN LISTS _declared_spawns)
            if(NOT _tool IN_LIST _runtime)
                list(APPEND _spawn_errors "${_t} declares ${_tool}, which is not a recorded runtime target")
            endif()
        endforeach()
        if(_names)
            list(REMOVE_DUPLICATES _names)
            list(SORT _names)
        endif()
        _pulp_test_data_json_list(_jsrc ${_rels})
        _pulp_test_data_json_list(_jdef ${_names})
        _pulp_test_data_json_list(_jrun ${_runtime})
        set(_jnone false)
        if(_t IN_LIST _spawns_none)
            set(_jnone true)
            if(_runtime)
                list(APPEND _spawn_errors "${_t} is declared pulp_test_spawns(NONE) but depends on ${_runtime}")
            endif()
        endif()
        list(APPEND _rows "  \"${_t}\": {\"sources\": ${_jsrc}, \"tree_defines\": ${_jdef}, \"runtime_targets\": ${_jrun}, \"spawns_none\": ${_jnone}}")
    endforeach()
    if(_spawn_errors)
        list(JOIN _spawn_errors "\n  " _spawn_text)
        message(FATAL_ERROR
            "A test's runtime spawn edges are inconsistent. Either it names a built target through "
            "$<TARGET_FILE:...> without depending on it, so the build graph cannot see that it runs "
            "or loads that target, or it is declared NONE yet depends on one:\n  ${_spawn_text}\n"
            "Declare the edge with pulp_test_spawns(<test> <target>) (tools/cmake/PulpTestData.cmake).")
    endif()
    list(JOIN _rows ",\n" _body)
    file(CONFIGURE OUTPUT "${PULP_TEST_DATA_DIR}/executables.json"
        CONTENT "{\"schema\": \"pulp-test-executables/v1\", \"executables\": {\n${_body}\n}}\n"
        @ONLY)

    get_property(_declared GLOBAL PROPERTY PULP_TEST_DATA_EXECUTABLES)
    file(GLOB _stale "${PULP_TEST_DATA_DIR}/*.inputs.json")
    foreach(_exe IN LISTS _declared)
        get_property(_s GLOBAL PROPERTY PULP_TEST_DATA_SOURCES_${_exe})
        get_property(_p GLOBAL PROPERTY PULP_TEST_DATA_PATHS_${_exe})
        list(REMOVE_DUPLICATES _s)
        list(SORT _s)
        list(REMOVE_DUPLICATES _p)
        list(SORT _p)
        _pulp_test_data_json_list(_js ${_s})
        _pulp_test_data_json_list(_jp ${_p})
        set(_out "${PULP_TEST_DATA_DIR}/${_exe}.inputs.json")
        list(REMOVE_ITEM _stale "${_out}")
        file(CONFIGURE OUTPUT "${_out}"
            CONTENT "{\"schema\": \"pulp-test-data-inputs/v1\", \"executable\": \"${_exe}\", \"kind\": \"compiled\", \"sources\": ${_js}, \"inputs\": ${_jp}}\n"
            @ONLY)
    endforeach()
    if(_stale)
        file(REMOVE ${_stale})
    endif()
endfunction()

function(pulp_test_spawns TEST)
    if(ARGN STREQUAL "NONE")
        set_property(GLOBAL APPEND PROPERTY PULP_TEST_SPAWNS_NONE "${TEST}")
        return()
    endif()
    foreach(_tool IN LISTS ARGN)
        # A deferred call evaluates its arguments when it runs, after this
        # function's variables are gone, so the names are written in literally.
        cmake_language(EVAL CODE
            "cmake_language(DEFER DIRECTORY [[${CMAKE_SOURCE_DIR}]] CALL _pulp_test_spawns_apply [[${TEST}]] [[${_tool}]])")
    endforeach()
endfunction()

# A test or tool this configuration does not build (a platform-specific suite,
# an optional tool) has no edge to add.
function(_pulp_test_spawns_apply TEST TOOL)
    if(TARGET ${TEST} AND TARGET ${TOOL})
        add_dependencies(${TEST} ${TOOL})
        set_property(GLOBAL APPEND PROPERTY PULP_TEST_SPAWNS_OF_${TEST} "${TOOL}")
    endif()
endfunction()

function(pulp_test_data_arm)
    get_property(_armed GLOBAL PROPERTY PULP_TEST_DATA_ARMED)
    if(NOT _armed)
        set_property(GLOBAL PROPERTY PULP_TEST_DATA_ARMED TRUE)
        # At the end of the top-level directory, once every tool directory
        # has defined its targets.
        cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _pulp_test_data_finalize_after_spawns)
    endif()
endfunction()

# pulp_test_spawns() edges are deferred to the same point and queued after
# this call; deferring once more runs the finalize after all of them.
function(_pulp_test_data_finalize_after_spawns)
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _pulp_test_data_finalize)
endfunction()
