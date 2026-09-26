# Shared test-support objects: compile a support source once, link it many times.
#
# Some test-support translation units are listed in the SOURCES of hundreds of
# test executables, so every one of them compiled its own copy. Two of them
# are pure link-time support whose object does not depend on the consumer:
#
#   * test/harness/rt_allocation_probe.cpp — the counting global operator
#     new/delete behind RtAllocationProbe;
#   * test/native_components/rt_intercept_test_support.cpp (Unix) — the
#     trapping operator new/delete, pthread lock overrides, and the strong
#     pulp_rt_trap_if_no_alloc_scope.
#
# Neither reads a consumer-supplied macro: the probe includes only its own
# header and the standard library (plus <malloc.h> on _WIN32); the
# interceptor adds native_core.h, scoped_no_alloc.hpp and rt_test_scope.hpp,
# none of which test a configurable macro. So each is compiled once into an
# OBJECT library here, and the directory's finalize pass swaps every listing
# of the source for that library's objects. The objects are still linked into
# each executable individually, exactly as the per-target copies were: the
# global operator new replacement and the strong-over-weak trap symbol resolve
# the same way, and each executable keeps its own probe state.
#
# The swap works on the target's SOURCES, so manifests keep listing the
# source as before (plainly, or conditionally as
# `$<$<BOOL:${UNIX}>:.../rt_intercept_test_support.cpp>`) and a new manifest
# gets the shared object without knowing about this file.
#
# A source whose object DOES depend on consumer macros must not be added
# here: it would silently compile with none of them. The
# `test-shared-objects` ctest asserts each listed source is compiled exactly
# once in the generated build and that no consumer compiles its own copy.
include_guard(GLOBAL)

# Pairs of "<object library>|<source path relative to CMAKE_SOURCE_DIR>".
set(_PULP_TEST_SHARED_OBJECTS
    "pulp-test-rt-allocation-probe|test/harness/rt_allocation_probe.cpp")
if(UNIX)
    list(APPEND _PULP_TEST_SHARED_OBJECTS
        "pulp-test-rt-intercept|test/native_components/rt_intercept_test_support.cpp")
endif()

# Creates the object libraries (once) in the calling directory.
function(_pulp_test_shared_objects_define)
    if(TARGET pulp-test-rt-allocation-probe)
        return()
    endif()
    add_library(pulp-test-rt-allocation-probe OBJECT EXCLUDE_FROM_ALL
        "${CMAKE_SOURCE_DIR}/test/harness/rt_allocation_probe.cpp")
    target_include_directories(pulp-test-rt-allocation-probe PRIVATE
        "${CMAKE_SOURCE_DIR}/test/harness")
    set_target_properties(pulp-test-rt-allocation-probe PROPERTIES FOLDER "test-support")
    if(UNIX)
        add_library(pulp-test-rt-intercept OBJECT EXCLUDE_FROM_ALL
            "${CMAKE_SOURCE_DIR}/test/native_components/rt_intercept_test_support.cpp")
        # Compile-side usage requirements only: the objects reach consumers
        # through $<TARGET_OBJECTS>, and each consumer already links what
        # they reference, as it did when it compiled the source itself.
        target_include_directories(pulp-test-rt-intercept PRIVATE
            "${CMAKE_SOURCE_DIR}/test/native_components"
            "${CMAKE_SOURCE_DIR}/test/harness")
        target_link_libraries(pulp-test-rt-intercept PRIVATE
            pulp::native-components pulp::runtime)
        set_target_properties(pulp-test-rt-intercept PROPERTIES FOLDER "test-support")
    endif()
endfunction()

# Returns in ${out} the SOURCES list ${sources} with every listing of a shared
# source replaced by its object library's objects, and in ${out}_changed
# whether anything was replaced.
function(pulp_test_shared_objects_rewrite out sources)
    set(_result "")
    set(_changed FALSE)
    set(_plain_libs "")
    foreach(_entry IN LISTS sources)
        set(_new "${_entry}")
        foreach(_pair IN LISTS _PULP_TEST_SHARED_OBJECTS)
            string(REPLACE "|" ";" _pair "${_pair}")
            list(GET _pair 0 _lib)
            list(GET _pair 1 _rel)
            get_filename_component(_name "${_rel}" NAME)
            string(REPLACE "." "\\." _name_re "${_name}")
            if(NOT _entry MATCHES "\\$<")
                get_filename_component(_entry_name "${_entry}" NAME)
                if(_entry_name STREQUAL _name)
                    set(_new "$<TARGET_OBJECTS:${_lib}>")
                    list(APPEND _plain_libs "${_lib}")
                endif()
            elseif(_entry MATCHES ">:([^;<>]*/)?${_name_re}>$")
                # $<$<cond>:path> keeps its condition around the objects.
                string(REGEX REPLACE ">:([^;<>]*/)?${_name_re}>$"
                    ">:$<TARGET_OBJECTS:${_lib}>>" _new "${_entry}")
            endif()
        endforeach()
        if(NOT _new STREQUAL _entry)
            set(_changed TRUE)
        endif()
        list(APPEND _result "${_new}")
    endforeach()
    # A plain listing already links the objects unconditionally; a
    # conditional listing of the same library would link them twice.
    foreach(_lib IN LISTS _plain_libs)
        list(FILTER _result EXCLUDE REGEX "^\\$<.+>:\\$<TARGET_OBJECTS:${_lib}>>$")
    endforeach()
    list(REMOVE_DUPLICATES _result)
    set(${out} "${_result}" PARENT_SCOPE)
    set(${out}_changed ${_changed} PARENT_SCOPE)
endfunction()

# Deferred to the end of the directory so it sees every manifest's targets
# (including sources a manifest appends after declaring the target).
function(_pulp_test_shared_objects_finalize)
    get_property(_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_t IN LISTS _targets)
        if(_t MATCHES "^pulp-test-rt-(allocation-probe|intercept)$")
            continue()
        endif()
        get_target_property(_type ${_t} TYPE)
        if(_type STREQUAL "INTERFACE_LIBRARY" OR _type STREQUAL "UTILITY")
            continue()
        endif()
        get_target_property(_sources ${_t} SOURCES)
        if(NOT _sources)
            continue()
        endif()
        pulp_test_shared_objects_rewrite(_rewritten "${_sources}")
        if(_rewritten_changed)
            set_property(TARGET ${_t} PROPERTY SOURCES "${_rewritten}")
        endif()
    endforeach()
endfunction()

# Arms the pass for the including directory (test/).
function(pulp_test_shared_objects_arm)
    get_property(_armed DIRECTORY PROPERTY _PULP_TEST_SHARED_OBJECTS_ARMED)
    if(_armed)
        return()
    endif()
    set_property(DIRECTORY PROPERTY _PULP_TEST_SHARED_OBJECTS_ARMED TRUE)
    _pulp_test_shared_objects_define()
    cmake_language(DEFER CALL _pulp_test_shared_objects_finalize)
endfunction()
