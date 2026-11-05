# Asserts the device-layer mode is consistent across the targets that carry
# it: PULP_GPU_RUNTIME is on pulp::platform's and pulp::gpu-device-api's
# interface exactly when this build forwards to gpu-runtime, so a
# PULP_USE_GPU_RUNTIME=OFF build leaks no gpu-runtime define into a consumer.
#
#   cmake -DHAS_GPU_RUNTIME=<bool> -DPLATFORM_DEFS=<list> -DAPI_DEFS=<list>
#         -P check_gpu_runtime_mode.cmake
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
message(STATUS "gpu-runtime mode consistent (forwarding: ${HAS_GPU_RUNTIME})")
