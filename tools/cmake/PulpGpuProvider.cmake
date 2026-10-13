# Shared provider-selection seam for the native GPU targets.

include_guard(GLOBAL)

function(pulp_link_gpu_provider target provider_scope definition_scope)
    if(PULP_HAS_WEBGPU AND TARGET webgpu
       AND NOT (PULP_HAS_SKIA AND EXISTS "${DAWN_LIBRARY}"))
        target_link_libraries(${target} ${provider_scope} webgpu)
    endif()
    if(PULP_HAS_WEBGPU)
        target_compile_definitions(${target} ${definition_scope} PULP_HAS_WEBGPU=1)
    endif()
endfunction()
