# Drives pulp_check_gpu_runtime_benchmark_flavour() through every pairing of
# gpu-runtime's recorded flavour and PULP_BENCHMARK; each case runs in a child
# cmake so a refusal's FATAL_ERROR does not end this script.
#   cmake -DMODULE=<PulpGpuRuntimeRecord.cmake> -P check_gpu_runtime_benchmark_flavour.cmake
cmake_minimum_required(VERSION 3.24)
include("${MODULE}")
if(MODE STREQUAL "check")
    pulp_check_gpu_runtime_benchmark_flavour("${DEFS}" "${BENCH}")
    return()
endif()
# <name>|<interface definitions>|<PULP_BENCHMARK>|<pass or refuse>
set(_cases
    "empty-production||OFF|pass"
    "production|PULP_UNRELATED=1|OFF|pass"
    "benchmark|GPU_RUNTIME_BENCHMARK=1|ON|pass"
    "bare-benchmark|GPU_RUNTIME_BENCHMARK|ON|pass"
    "benchmark-package-production-pulp|GPU_RUNTIME_BENCHMARK=1|OFF|refuse"
    "production-package-benchmark-pulp||ON|refuse"
    "explicit-zero|GPU_RUNTIME_BENCHMARK=0|OFF|pass"
    "explicit-zero-benchmark-pulp|GPU_RUNTIME_BENCHMARK=0|ON|refuse")
foreach(_case IN LISTS _cases)
    string(REPLACE "|" ";" _fields "${_case}")
    list(LENGTH _fields _count)
    list(GET _fields 0 _name)
    if(_count EQUAL 4)
        list(GET _fields 1 _defs)
        list(GET _fields 2 _bench)
        list(GET _fields 3 _expect)
    else()
        set(_defs "")
        list(GET _fields 1 _bench)
        list(GET _fields 2 _expect)
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -DMODE=check -DMODULE=${MODULE}
            "-DDEFS=${_defs}" -DBENCH=${_bench} -P "${CMAKE_CURRENT_LIST_FILE}"
        RESULT_VARIABLE _result OUTPUT_QUIET ERROR_VARIABLE _error)
    if(_expect STREQUAL "pass" AND NOT _result EQUAL 0)
        message(FATAL_ERROR "case ${_name} was refused: ${_error}")
    elseif(_expect STREQUAL "refuse" AND _result EQUAL 0)
        message(FATAL_ERROR "case ${_name} was accepted")
    endif()
endforeach()
message(STATUS "gpu-runtime benchmark flavour check: ok")
