#[[
Proves the one Skia-supplies-Dawn decision (tools/cmake/PulpSkiaDawnProvider.cmake)
picks wgpu-native exactly when Skia does not supply Dawn, whatever state
DAWN_LIBRARY is in where the decision is read.

Each row records the provider from a Dawn archive candidate as FindSkia would,
then reads the link decision in a nested function scope where DAWN_LIBRARY
holds a row-specific value, as a consumer directory might see it:

  existing  archive on disk; consumer scope has DAWN_LIBRARY unset  -> no wgpu
  notfound  DAWN_LIBRARY-NOTFOUND sentinel                          -> wgpu
  unset     no candidate;    consumer scope has DAWN_LIBRARY unset  -> wgpu
  stale     absolute path to an archive that does not exist         -> wgpu

A predicate re-derived from DAWN_LIBRARY fails a row: `EXISTS DAWN_LIBRARY`
links wgpu beside Skia's Dawn in `existing`, and a NOTFOUND-sentinel compare
treats `stale` as a present Dawn. Every row also checks PULP_SKIA_DAWN_ARCHIVE
names the archive exactly when the decision is TRUE.

The static half scans every CMake file in the source tree (outside FindSkia,
which resolves DAWN_LIBRARY) for a condition built on DAWN_LIBRARY: EXISTS on
it, a STREQUAL compare of it, or its bare truthiness. The known consumers must
also name the shared decision, so the scan is shown to be reading them.
]]

cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED PULP_SOURCE_DIR OR NOT DEFINED FIXTURE_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR and FIXTURE_DIR are required")
endif()

file(REMOVE_RECURSE "${FIXTURE_DIR}")
file(MAKE_DIRECTORY "${FIXTURE_DIR}")
set(_archive "${FIXTURE_DIR}/libdawn_combined.a")
file(WRITE "${_archive}" "fixture")
set(_stale "${FIXTURE_DIR}/missing/libdawn_combined.a")

include("${PULP_SOURCE_DIR}/tools/cmake/PulpSkiaDawnProvider.cmake")

set(_failures "")

# Reads the decision in its own scope with the consumer's view of DAWN_LIBRARY.
function(_read_decision out consumer_dawn_library)
    unset(DAWN_LIBRARY)
    if(NOT consumer_dawn_library STREQUAL "<unset>")
        set(DAWN_LIBRARY "${consumer_dawn_library}")
    endif()
    pulp_gpu_links_wgpu_native(_links WEBGPU_TARGET TRUE)
    set(${out} ${_links} PARENT_SCOPE)
endfunction()

function(_check_row name candidate consumer_dawn_library expect_provides expect_wgpu)
    set(PULP_HAS_SKIA TRUE)
    set(PULP_HAS_WEBGPU TRUE)
    if(candidate STREQUAL "")
        pulp_record_skia_dawn_provider(SKIA_FOUND TRUE)
    else()
        pulp_record_skia_dawn_provider(SKIA_FOUND TRUE DAWN_ARCHIVE "${candidate}")
    endif()
    _read_decision(_links "${consumer_dawn_library}")
    set(_row_failures "")
    if(NOT PULP_SKIA_PROVIDES_DAWN STREQUAL expect_provides)
        list(APPEND _row_failures
            "${name}: PULP_SKIA_PROVIDES_DAWN=${PULP_SKIA_PROVIDES_DAWN}, expected ${expect_provides}")
    endif()
    set(_expect_archive "")
    if(expect_provides)
        set(_expect_archive "${candidate}")
    endif()
    if(NOT PULP_SKIA_DAWN_ARCHIVE STREQUAL _expect_archive)
        list(APPEND _row_failures
            "${name}: PULP_SKIA_DAWN_ARCHIVE='${PULP_SKIA_DAWN_ARCHIVE}', expected '${_expect_archive}'")
    endif()
    if(NOT _links STREQUAL expect_wgpu)
        list(APPEND _row_failures
            "${name}: links wgpu-native=${_links}, expected ${expect_wgpu}")
    endif()
    set(_failures ${_failures} ${_row_failures} PARENT_SCOPE)
endfunction()

_check_row(existing "${_archive}" "<unset>" TRUE FALSE)
_check_row(notfound "DAWN_LIBRARY-NOTFOUND" "DAWN_LIBRARY-NOTFOUND" FALSE TRUE)
_check_row(unset "" "<unset>" FALSE TRUE)
_check_row(stale "${_stale}" "${_stale}" FALSE TRUE)

# Control rows: Skia absent records no provider even with an archive on disk,
# and no wgpu-native is chosen without WebGPU or without its target.
pulp_record_skia_dawn_provider(SKIA_FOUND FALSE DAWN_ARCHIVE "${_archive}")
if(PULP_SKIA_PROVIDES_DAWN)
    list(APPEND _failures "skia-absent: recorded a Dawn provider")
endif()
set(PULP_HAS_WEBGPU FALSE)
pulp_gpu_links_wgpu_native(_links WEBGPU_TARGET TRUE)
if(_links)
    list(APPEND _failures "webgpu-off: chose wgpu-native")
endif()
set(PULP_HAS_WEBGPU TRUE)
pulp_gpu_links_wgpu_native(_links WEBGPU_TARGET FALSE)
if(_links)
    list(APPEND _failures "no-webgpu-target: chose wgpu-native")
endif()

# Code text of a CMake file on one line: full-line and trailing `#` comments
# removed, so prose that names DAWN_LIBRARY is never read as a condition, and
# every whitespace run (including line breaks) collapsed to one space.
function(_cmake_code out path)
    file(STRINGS "${path}" _lines)
    set(_code "")
    foreach(_line IN LISTS _lines)
        if(_line MATCHES "^[ \t]*#")
            continue()
        endif()
        string(REGEX REPLACE "[ \t]#[^\"]*$" "" _line "${_line}")
        string(APPEND _code " ${_line}")
    endforeach()
    string(REGEX REPLACE "[ \t\r\n]+" " " _code "${_code}")
    set(${out} "${_code}" PARENT_SCOPE)
endfunction()

# A yes/no "Skia supplies Dawn" condition re-derived from DAWN_LIBRARY.
set(_rederive_exists [=[EXISTS "?\${DAWN_LIBRARY}]=])
set(_rederive_compare [=[DAWN_LIBRARY}?"? STREQUAL]=])
set(_rederive_truthy [=[(\(|(AND|OR|NOT) ) ?DAWN_LIBRARY[ )]]=])
function(_rederives out code)
    set(_hit FALSE)
    foreach(_pattern IN ITEMS "${_rederive_exists}" "${_rederive_compare}" "${_rederive_truthy}")
        if(code MATCHES "${_pattern}")
            set(_hit TRUE)
        endif()
    endforeach()
    set(${out} ${_hit} PARENT_SCOPE)
endfunction()

# Positive control: each pattern fires on the predicates it exists to reject.
foreach(_bad
        "if(PULP_HAS_SKIA AND EXISTS \"\${DAWN_LIBRARY}\")"
        "AND NOT (PULP_HAS_SKIA AND\n   EXISTS \"\${DAWN_LIBRARY}\"))"
        "OR DAWN_LIBRARY STREQUAL \"DAWN_LIBRARY-NOTFOUND\")"
        "AND NOT (PULP_HAS_SKIA AND DAWN_LIBRARY\n AND NOT x)")
    string(REGEX REPLACE "[ \t\r\n]+" " " _bad_code "${_bad}")
    _rederives(_hit "${_bad_code}")
    if(NOT _hit)
        list(APPEND _failures "scanner control: missed `${_bad}`")
    endif()
endforeach()
foreach(_fine
        "if(PULP_SKIA_PROVIDES_DAWN)"
        "    --dawn-library \"\${PULP_SKIA_DAWN_ARCHIVE}\"")
    _rederives(_hit "${_fine}")
    if(_hit)
        list(APPEND _failures "scanner control: flagged `${_fine}`")
    endif()
endforeach()

set(_scan_files "${PULP_SOURCE_DIR}/CMakeLists.txt")
foreach(_root core apple inspect ship tools test examples templates)
    if(IS_DIRECTORY "${PULP_SOURCE_DIR}/${_root}")
        file(GLOB_RECURSE _found LIST_DIRECTORIES false
            "${PULP_SOURCE_DIR}/${_root}/CMakeLists.txt"
            "${PULP_SOURCE_DIR}/${_root}/*.cmake"
            "${PULP_SOURCE_DIR}/${_root}/*.cmake.in")
        list(APPEND _scan_files ${_found})
    endif()
endforeach()
list(FILTER _scan_files EXCLUDE REGEX "/(build[^/]*|_deps|node_modules)/")
list(FILTER _scan_files EXCLUDE REGEX "/tools/cmake/FindSkia\\.cmake$")
list(FILTER _scan_files EXCLUDE REGEX "/test/cmake/test_skia_dawn_provider\\.cmake$")
set(_scanned 0)
foreach(_file IN LISTS _scan_files)
    _cmake_code(_code "${_file}")
    _rederives(_hit "${_code}")
    if(_hit)
        file(RELATIVE_PATH _rel "${PULP_SOURCE_DIR}" "${_file}")
        list(APPEND _failures
            "${_rel}: re-derives the Skia Dawn provider from DAWN_LIBRARY; read PULP_SKIA_PROVIDES_DAWN")
    endif()
    math(EXPR _scanned "${_scanned} + 1")
endforeach()

# The scan reached the known consumers, and each names the shared decision.
foreach(_consumer
        "core/render/CMakeLists.txt|pulp_gpu_links_wgpu_native("
        "core/gpu_audio/CMakeLists.txt|PULP_SKIA_PROVIDES_DAWN"
        "tools/cmake/PulpGpuAudioProviderIdentity.cmake|PULP_SKIA_PROVIDES_DAWN"
        "test/cmake/render_gpu_surface_tests.cmake|PULP_SKIA_PROVIDES_DAWN"
        "test/fixtures/native_ui_link_floor/CMakeLists.txt|PULP_SKIA_PROVIDES_DAWN"
        "test/fixtures/no_editor_link_floor/CMakeLists.txt|pulp_gpu_links_wgpu_native("
        "test/fixtures/render_provider_floor/CMakeLists.txt|PULP_SKIA_PROVIDES_DAWN")
    string(REPLACE "|" ";" _consumer "${_consumer}")
    list(GET _consumer 0 _path)
    list(GET _consumer 1 _needle)
    if(NOT "${PULP_SOURCE_DIR}/${_path}" IN_LIST _scan_files)
        list(APPEND _failures "${_path}: not reached by the tree scan")
        continue()
    endif()
    _cmake_code(_code "${PULP_SOURCE_DIR}/${_path}")
    string(FIND "${_code}" "${_needle}" _at)
    if(_at EQUAL -1)
        list(APPEND _failures "${_path}: does not read the shared decision (${_needle})")
    endif()
endforeach()

if(_failures)
    string(REPLACE ";" "\n  " _failures "${_failures}")
    message(FATAL_ERROR "Skia Dawn provider decision is wrong:\n  ${_failures}")
endif()
message(STATUS "Skia Dawn provider decision: all rows pass; scanned ${_scanned} CMake files")
