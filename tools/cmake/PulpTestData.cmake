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
#     PATHS <repo-relative path or glob> ...   # files, directories, or globs
#     [DEFINE <macro>]                         # default PULP_SOURCE_DIR
# )
#
# It defines <macro> as the checkout root for the suite's sources (the
# COMPILE_DEFINITIONS PULP_SOURCE_DIR=... it replaces) and records the paths.
# Every path must match something in the checkout at configure time, so a
# typo or a moved fixture fails the configure instead of declaring nothing.
#
# The suite argument is either a pulp_add_test_suite name (grouped or not;
# only that member's sources are declared) or any executable target.
#
# At the end of the test directory two artifacts are written under
# ${CMAKE_BINARY_DIR}/test/test-data/:
#   <executable>.inputs.json  per executable with a declaration: the sources
#                             that declared and the union of their paths;
#   executables.json          every compiled test executable with its
#                             repo-relative sources and the names of the
#                             compile definitions whose value points into the
#                             checkout.
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
    cmake_parse_arguments(D "" "DEFINE" "PATHS" ${ARGN})
    if(D_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "pulp_test_data(${NAME}): unparsed arguments: ${D_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT D_PATHS)
        message(FATAL_ERROR "pulp_test_data(${NAME}): PATHS is required")
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
    if(_exe STREQUAL NAME)
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

function(_pulp_test_data_finalize)
    file(MAKE_DIRECTORY "${PULP_TEST_DATA_DIR}")
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
        if(_names)
            list(REMOVE_DUPLICATES _names)
            list(SORT _names)
        endif()
        _pulp_test_data_json_list(_jsrc ${_rels})
        _pulp_test_data_json_list(_jdef ${_names})
        list(APPEND _rows "  \"${_t}\": {\"sources\": ${_jsrc}, \"tree_defines\": ${_jdef}}")
    endforeach()
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

function(pulp_test_data_arm)
    get_property(_armed GLOBAL PROPERTY PULP_TEST_DATA_ARMED)
    if(NOT _armed)
        set_property(GLOBAL PROPERTY PULP_TEST_DATA_ARMED TRUE)
        cmake_language(DEFER CALL _pulp_test_data_finalize)
    endif()
endfunction()
