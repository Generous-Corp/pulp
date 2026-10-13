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
treats `stale` as a present Dawn. The static half checks that the renderer and
the two link-floor fixtures read the shared decision instead of DAWN_LIBRARY.
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

# Every consumer reads the shared decision; none re-derives it from DAWN_LIBRARY.
foreach(_consumer
        "core/render/CMakeLists.txt|pulp_gpu_links_wgpu_native("
        "test/fixtures/no_editor_link_floor/CMakeLists.txt|pulp_gpu_links_wgpu_native("
        "test/fixtures/render_provider_floor/CMakeLists.txt|PULP_SKIA_PROVIDES_DAWN")
    string(REPLACE "|" ";" _consumer "${_consumer}")
    list(GET _consumer 0 _path)
    list(GET _consumer 1 _needle)
    file(STRINGS "${PULP_SOURCE_DIR}/${_path}" _lines)
    set(_code "")
    foreach(_line IN LISTS _lines)
        if(NOT _line MATCHES "^[ \t]*#")
            string(APPEND _code "${_line}\n")
        endif()
    endforeach()
    string(FIND "${_code}" "${_needle}" _hit)
    if(_hit EQUAL -1)
        list(APPEND _failures "${_path}: does not read the shared decision (${_needle})")
    endif()
    if(_code MATCHES "DAWN_LIBRARY")
        list(APPEND _failures "${_path}: re-derives the provider from DAWN_LIBRARY")
    endif()
endforeach()

if(_failures)
    string(REPLACE ";" "\n  " _failures "${_failures}")
    message(FATAL_ERROR "Skia Dawn provider decision is wrong:\n  ${_failures}")
endif()
message(STATUS "Skia Dawn provider decision: all rows pass")
