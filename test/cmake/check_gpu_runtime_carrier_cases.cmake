# Configures test/fixtures/gpu_runtime_carriers once per case and runs
# check_gpu_runtime_mode.cmake on the carrier list it writes; nothing is built.
#   cmake -DFIXTURE=<dir> -DCHECK=<check_gpu_runtime_mode.cmake> -DWORK_DIR=<dir>
#         -DGENERATOR=<generator> -P check_gpu_runtime_carrier_cases.cmake
cmake_minimum_required(VERSION 3.24)
get_filename_component(_module "${CHECK}" DIRECTORY)
set(_module "${_module}/GpuRuntimeCarriers.cmake")
# <case>|<pass or refuse>
set(_cases
    "empty|pass"
    "object-check|pass"
    "static-check|refuse"
    "static-zero|refuse"
    "static-option|refuse"
    "generator-expression|refuse"
    "interface|refuse"
    "linked|refuse"
    "objects|refuse"
    "gpu-runtime-without-flavour|refuse"
    "gpu-runtime-with-flavour|pass"
    "gpu-runtime-zero-flavour|pass")
file(REMOVE_RECURSE "${WORK_DIR}")
foreach(_entry IN LISTS _cases)
    string(REPLACE "|" ";" _fields "${_entry}")
    list(GET _fields 0 _case)
    list(GET _fields 1 _expect)
    set(_build "${WORK_DIR}/${_case}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${FIXTURE}" -B "${_build}" -G "${GENERATOR}"
            -DCASE=${_case} "-DCARRIERS_MODULE=${_module}"
        RESULT_VARIABLE _result OUTPUT_QUIET ERROR_VARIABLE _error)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "fixture case ${_case} did not configure: ${_error}")
    endif()
    if(_case MATCHES "^gpu-runtime-")
        set(_mode ON)
        set(_defs "PULP_GPU_RUNTIME=1")
    else()
        set(_mode OFF)
        set(_defs "")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -DHAS_GPU_RUNTIME=${_mode} "-DPLATFORM_DEFS=${_defs}"
            "-DAPI_DEFS=${_defs}" "-DCARRIERS_FILE=${_build}/gpu-runtime-carriers.txt"
            -P "${CHECK}"
        RESULT_VARIABLE _result OUTPUT_QUIET ERROR_VARIABLE _error)
    if(_expect STREQUAL "pass" AND NOT _result EQUAL 0)
        message(FATAL_ERROR "case ${_case} was refused: ${_error}")
    elseif(_expect STREQUAL "refuse" AND _result EQUAL 0)
        message(FATAL_ERROR "case ${_case} was accepted")
    endif()
endforeach()
message(STATUS "gpu-runtime carrier cases: ok")
