#[[
Switching pulp_add_binary_data's COMPRESS on, and off again, in an existing build regenerates
the embedded source with a Makefile generator: CMake reruns a custom command
whose command line changed. Verified on CMake 3.24.3 (the repository minimum)
and 4.3.3; this test keeps it from regressing silently.

Configures a throwaway project with Unix Makefiles, builds it raw, reconfigures
the same build directory with COMPRESS on and builds again, then requires the
generated source to hold the compressed symbols. Run with:
    cmake -DPULP_SOURCE_DIR=<repo> -P test_pulp_add_binary_data_compress_toggle.cmake
]]

cmake_minimum_required(VERSION 3.20)

if(NOT PULP_SOURCE_DIR)
    get_filename_component(PULP_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
endif()
find_program(_make NAMES make gmake)
if(NOT _make)
    message(STATUS "SKIP: no make program for the Unix Makefiles generator")
    return()
endif()

set(_work "${CMAKE_CURRENT_BINARY_DIR}/pulp-binary-data-compress-toggle")
file(REMOVE_RECURSE "${_work}")
file(MAKE_DIRECTORY "${_work}/src")
file(WRITE "${_work}/src/asset.txt" "compress me compress me compress me\n")
file(WRITE "${_work}/src/main.cpp" "int main() { return 0; }\n")
file(WRITE "${_work}/src/CMakeLists.txt" "
cmake_minimum_required(VERSION 3.20)
project(compress_toggle CXX)
include(\"${PULP_SOURCE_DIR}/tools/cmake/PulpEmbedData.cmake\")
option(USE_COMPRESS \"\" OFF)
if(USE_COMPRESS)
    pulp_add_binary_data(toggle_assets SOURCES asset.txt NAMESPACE toggle COMPRESS)
else()
    pulp_add_binary_data(toggle_assets SOURCES asset.txt NAMESPACE toggle)
endif()
")

function(_step what)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${what} failed (${rc}):\n${out}\n${err}")
    endif()
endfunction()

set(_build "${_work}/build")
set(_generated "${_build}/toggle_assets_data_asset_txt.cpp")

_step("configure raw" ${CMAKE_COMMAND} -S "${_work}/src" -B "${_build}"
      -G "Unix Makefiles" -DUSE_COMPRESS=OFF)
_step("build raw" ${CMAKE_COMMAND} --build "${_build}")
file(READ "${_generated}" _raw)
string(FIND "${_raw}" "asset_txt_zlib" _hit)
if(NOT _hit EQUAL -1)
    message(FATAL_ERROR "raw build already holds compressed symbols:\n${_raw}")
endif()

_step("configure compressed" ${CMAKE_COMMAND} -S "${_work}/src" -B "${_build}" -DUSE_COMPRESS=ON)
_step("build compressed" ${CMAKE_COMMAND} --build "${_build}")
file(READ "${_generated}" _compressed)
string(FIND "${_compressed}" "extern const unsigned char asset_txt_zlib[]" _hit)
if(_hit EQUAL -1)
    message(FATAL_ERROR
        "turning COMPRESS on did not regenerate the embedded source:\n${_compressed}")
endif()

# And back: switching COMPRESS off again regenerates the raw array.
_step("configure raw again" ${CMAKE_COMMAND} -S "${_work}/src" -B "${_build}" -DUSE_COMPRESS=OFF)
_step("build raw again" ${CMAKE_COMMAND} --build "${_build}")
file(READ "${_generated}" _raw_again)
string(FIND "${_raw_again}" "extern const unsigned char asset_txt[]" _hit)
string(FIND "${_raw_again}" "asset_txt_zlib" _zlib_hit)
if(_hit EQUAL -1 OR NOT _zlib_hit EQUAL -1)
    message(FATAL_ERROR
        "turning COMPRESS off did not regenerate the raw source:\n${_raw_again}")
endif()

message(STATUS "pulp_add_binary_data: toggling COMPRESS on and off regenerates under Unix Makefiles")
