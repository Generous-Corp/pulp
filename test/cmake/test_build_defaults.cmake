# Configure-time build defaults: an empty CMAKE_BUILD_TYPE becomes Release for
# single-config generators, and Ninja links run in the bounded `pulp_link` pool
# (PULP_LINK_JOBS overrides the RAM-derived depth; 0 disables the pool).
cmake_minimum_required(VERSION 3.24)

find_program(_ninja ninja)
if(NOT _ninja)
    message(STATUS "SKIP: ninja is not on PATH")
    return()
endif()

file(REMOVE_RECURSE "${FIXTURE_DIR}")
set(_src "${FIXTURE_DIR}/src")
file(MAKE_DIRECTORY "${_src}")
file(WRITE "${_src}/main.cpp" "int main() { return 0; }\n")
file(WRITE "${_src}/CMakeLists.txt" "
cmake_minimum_required(VERSION 3.24)
project(BuildDefaultsFixture LANGUAGES CXX)
include(\"${PULP_SOURCE_DIR}/tools/cmake/PulpDefaultBuildType.cmake\")
include(\"${PULP_SOURCE_DIR}/tools/cmake/PulpLinkPool.cmake\")
add_executable(fixture_exe main.cpp)
")

function(_configure name out_ok)
    set(_bin "${FIXTURE_DIR}/${name}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${_src}" -B "${_bin}" ${ARGN}
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(_rc EQUAL 0)
        set(${out_ok} TRUE PARENT_SCOPE)
    else()
        set(${out_ok} FALSE PARENT_SCOPE)
        set(_last_error "${_out}${_err}" PARENT_SCOPE)
    endif()
endfunction()

function(_require_configured name)
    _configure(${name} _ok ${ARGN})
    if(NOT _ok)
        message(FATAL_ERROR "${name}: configure failed: ${_last_error}")
    endif()
endfunction()

function(_cache_value name key out_var)
    file(STRINGS "${FIXTURE_DIR}/${name}/CMakeCache.txt" _line REGEX "^${key}:")
    string(REGEX REPLACE "^[^=]*=" "" _value "${_line}")
    set(${out_var} "${_value}" PARENT_SCOPE)
endfunction()

# Ninja files; CMake writes the pool declaration to rules.ninja or build.ninja
# depending on version, so read both.
function(_ninja_text name out_var)
    set(_text "")
    foreach(_f build.ninja CMakeFiles/rules.ninja)
        if(EXISTS "${FIXTURE_DIR}/${name}/${_f}")
            file(READ "${FIXTURE_DIR}/${name}/${_f}" _part)
            string(APPEND _text "${_part}")
        endif()
    endforeach()
    set(${out_var} "${_text}" PARENT_SCOPE)
endfunction()

# The link edge for fixture_exe must name the pool.
function(_link_edge_pool name out_var)
    file(READ "${FIXTURE_DIR}/${name}/build.ninja" _build)
    string(REGEX MATCH "build fixture_exe:[^\n]*LINKER[^\n]*\n([ ][^\n]*\n)*" _edge "${_build}")
    if(NOT _edge)
        message(FATAL_ERROR "${name}: no link edge for fixture_exe in build.ninja")
    endif()
    string(REGEX MATCH "pool = [A-Za-z_]+" _pool "${_edge}")
    set(${out_var} "${_pool}" PARENT_SCOPE)
endfunction()

# Default: Release, pooled, depth derived from RAM within [2, 8].
_require_configured(ninja-default -G Ninja)
_cache_value(ninja-default CMAKE_BUILD_TYPE _bt)
if(NOT _bt STREQUAL "Release")
    message(FATAL_ERROR "empty build type was not defaulted to Release: '${_bt}'")
endif()
_link_edge_pool(ninja-default _pool)
if(NOT _pool STREQUAL "pool = pulp_link")
    message(FATAL_ERROR "executable link edge is not in the pulp_link pool: '${_pool}'")
endif()
_ninja_text(ninja-default _text)
string(REGEX MATCH "pool pulp_link\n  depth = ([0-9]+)" _decl "${_text}")
if(NOT _decl OR CMAKE_MATCH_1 LESS 2 OR CMAKE_MATCH_1 GREATER 8)
    message(FATAL_ERROR "derived pulp_link depth is missing or outside [2, 8]: '${_decl}'")
endif()

# An explicit build type is left alone.
_require_configured(ninja-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug)
_cache_value(ninja-debug CMAKE_BUILD_TYPE _bt)
if(NOT _bt STREQUAL "Debug")
    message(FATAL_ERROR "explicit Debug build type was overridden: '${_bt}'")
endif()

# PULP_LINK_JOBS overrides the depth.
_require_configured(ninja-three -G Ninja -DPULP_LINK_JOBS=3)
_ninja_text(ninja-three _text)
string(FIND "${_text}" "pool pulp_link\n  depth = 3\n" _found)
if(_found LESS 0)
    message(FATAL_ERROR "PULP_LINK_JOBS=3 did not set the pool depth")
endif()

# PULP_LINK_JOBS=0 disables the pool.
_require_configured(ninja-off -G Ninja -DPULP_LINK_JOBS=0)
_link_edge_pool(ninja-off _pool)
if(_pool)
    message(FATAL_ERROR "PULP_LINK_JOBS=0 still pooled the link edge: '${_pool}'")
endif()

# A malformed value is rejected rather than silently ignored.
_configure(ninja-bad _ok -G Ninja -DPULP_LINK_JOBS=lots)
if(_ok)
    message(FATAL_ERROR "PULP_LINK_JOBS=lots was accepted")
endif()

# Makefiles have no job pools: the module is a no-op there.
if(NOT CMAKE_HOST_WIN32)
    _require_configured(make -G "Unix Makefiles")
    _cache_value(make CMAKE_BUILD_TYPE _bt)
    if(NOT _bt STREQUAL "Release")
        message(FATAL_ERROR "Makefile build type was not defaulted to Release: '${_bt}'")
    endif()
endif()
