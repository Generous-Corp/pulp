# Asserts the device-layer mode is consistent across the targets that carry
# it: PULP_GPU_RUNTIME is on pulp::platform's and pulp::gpu-device-api's
# interface exactly when this build forwards to gpu-runtime, so a
# PULP_USE_GPU_RUNTIME=OFF build leaks no gpu-runtime define into a consumer.
#
# Every target that carries PULP_GPU_RUNTIME directly must also link
# VellumGpuRuntime::DeviceApi, whose include directories hold the
# <gpu_runtime/...> headers the define selects. CARRIERS_FILE lists each direct
# carrier as `<target> <links DeviceApi>`, written at the end of configure.
#
#   cmake -DHAS_GPU_RUNTIME=<bool> -DPLATFORM_DEFS=<list> -DAPI_DEFS=<list>
#         -DCARRIERS_FILE=<path> -P check_gpu_runtime_mode.cmake
foreach(_name PLATFORM_DEFS API_DEFS)
    string(REPLACE "|" ";" _defs "${${_name}}")
    if("PULP_GPU_RUNTIME=1" IN_LIST _defs)
        set(_has_${_name} TRUE)
    else()
        set(_has_${_name} FALSE)
    endif()
    foreach(_def IN LISTS _defs)
        if(_def MATCHES "^PULP_GPU_RUNTIME" AND NOT _def STREQUAL "PULP_GPU_RUNTIME=1")
            message(FATAL_ERROR "${_name} carries an unexpected spelling: ${_def}")
        endif()
    endforeach()
endforeach()
if(HAS_GPU_RUNTIME)
    if(NOT _has_PLATFORM_DEFS OR NOT _has_API_DEFS)
        message(FATAL_ERROR
            "this build forwards to gpu-runtime, but PULP_GPU_RUNTIME=1 is missing from "
            "pulp::platform (${_has_PLATFORM_DEFS}) or pulp::gpu-device-api (${_has_API_DEFS})")
    endif()
elseif(_has_PLATFORM_DEFS OR _has_API_DEFS)
    message(FATAL_ERROR
        "this build keeps Pulp's own device sources, but PULP_GPU_RUNTIME leaks through "
        "pulp::platform (${_has_PLATFORM_DEFS}) or pulp::gpu-device-api (${_has_API_DEFS})")
endif()
if(NOT EXISTS "${CARRIERS_FILE}")
    message(FATAL_ERROR "carrier list ${CARRIERS_FILE} is missing; reconfigure")
endif()
file(STRINGS "${CARRIERS_FILE}" _carriers)
set(_carrier_count 0)
foreach(_line IN LISTS _carriers)
    string(REPLACE " " ";" _fields "${_line}")
    list(GET _fields 0 _target)
    # `benchmark-only <target> <type> <names GPU_RUNTIME_BENCHMARK>`: a
    # target-local PULP_BENCHMARK must stay compile-only, and with gpu-runtime
    # it must select gpu-runtime's benchmark declarations itself.
    if(_target STREQUAL "benchmark-only")
        list(GET _fields 1 _bench_target)
        list(GET _fields 2 _bench_type)
        list(GET _fields 3 _bench_grb)
        if(NOT _bench_type STREQUAL "OBJECT_LIBRARY")
            message(FATAL_ERROR
                "${_bench_target} defines PULP_BENCHMARK locally but is a ${_bench_type}; "
                "only a never-linked OBJECT library may, since a linked one would mix "
                "benchmark and production GpuCompute vtables")
        endif()
        if(HAS_GPU_RUNTIME AND NOT _bench_grb)
            message(FATAL_ERROR
                "${_bench_target} defines PULP_BENCHMARK locally without GPU_RUNTIME_BENCHMARK=1, "
                "so gpu-runtime's benchmark declarations are absent")
        endif()
        continue()
    endif()
    list(GET _fields 1 _links)
    math(EXPR _carrier_count "${_carrier_count} + 1")
    if(NOT HAS_GPU_RUNTIME)
        message(FATAL_ERROR "${_target} carries PULP_GPU_RUNTIME in a build that keeps Pulp's own device sources")
    endif()
    if(NOT _links)
        message(FATAL_ERROR
            "${_target} carries PULP_GPU_RUNTIME but does not link VellumGpuRuntime::DeviceApi, "
            "so its consumers see the define without the <gpu_runtime/...> include path")
    endif()
endforeach()
if(HAS_GPU_RUNTIME AND _carrier_count LESS 2)
    message(FATAL_ERROR "expected pulp-platform and pulp-gpu-device-api among the carriers, found ${_carrier_count}")
endif()
message(STATUS "gpu-runtime mode consistent (forwarding: ${HAS_GPU_RUNTIME}; ${_carrier_count} carrier(s), all linking DeviceApi)")
