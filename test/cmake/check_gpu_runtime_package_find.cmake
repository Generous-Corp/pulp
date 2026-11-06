# Installs the gpu-runtime subproject this build fetched into a scratch prefix
# and drives pulp_find_gpu_runtime_package() against it from child projects:
#   function  : the SDK's own find succeeds and yields the device targets;
#   components: find_package() naming components fails, the package behaviour
#               the function exists to avoid (if the package starts marking its
#               components found, this case flips and the function can request
#               them again);
#   no-dawn   : without the locked Dawn build the function refuses.
#   cmake -DMODULE=<PulpGpuRuntimeRecord.cmake> -DRUNTIME_BUILD_DIR=<dir>
#         -DSKIA_DIR=<dir> -DVERSION=<x.y.z> -DWORK_DIR=<dir> -P check_gpu_runtime_package_find.cmake
cmake_minimum_required(VERSION 3.24)
file(REMOVE_RECURSE "${WORK_DIR}")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${RUNTIME_BUILD_DIR}" --prefix "${WORK_DIR}/prefix"
    RESULT_VARIABLE _result OUTPUT_QUIET ERROR_VARIABLE _error)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "installing gpu-runtime from ${RUNTIME_BUILD_DIR} failed: ${_error}")
endif()
set(_package_dir "${WORK_DIR}/prefix/lib/cmake/VellumGpuRuntime")
foreach(_case function components no-dawn)
    set(_source "${WORK_DIR}/${_case}")
    file(MAKE_DIRECTORY "${_source}")
    if(_case STREQUAL "no-dawn")
        set(_skia "")
    else()
        set(_skia "${SKIA_DIR}")
    endif()
    if(_case STREQUAL "components")
        set(_find "find_package(VellumGpuRuntime ${VERSION} EXACT CONFIG REQUIRED COMPONENTS Device SkiaLogBridge PATHS \"${_package_dir}\" NO_DEFAULT_PATH)")
    else()
        set(_find "pulp_find_gpu_runtime_package(${VERSION} \"${_package_dir}\" _error)\nif(_error)\n    message(FATAL_ERROR \"\${_error}\")\nendif()\nif(NOT TARGET VellumGpuRuntime::Device)\n    message(FATAL_ERROR \"no device target\")\nendif()")
    endif()
    file(WRITE "${_source}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.24)\nproject(gpu_runtime_find_${_case} LANGUAGES NONE)\n"
        "include(\"${MODULE}\")\nset(GPU_RUNTIME_SKIA_DIR \"${_skia}\")\n${_find}\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${_source}" -B "${_source}/build"
        RESULT_VARIABLE _result OUTPUT_QUIET ERROR_VARIABLE _error)
    if(_case STREQUAL "function" AND NOT _result EQUAL 0)
        message(FATAL_ERROR "the SDK's gpu-runtime find failed: ${_error}")
    elseif(NOT _case STREQUAL "function" AND _result EQUAL 0)
        message(FATAL_ERROR "case ${_case} was accepted")
    endif()
endforeach()
message(STATUS "gpu-runtime package find: ok")
