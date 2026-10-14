# Shared provider-selection seam for the native GPU targets.

include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/PulpSkiaDawnProvider.cmake")

function(pulp_link_gpu_provider target provider_scope definition_scope)
    set(_webgpu_target_exists FALSE)
    if(TARGET webgpu)
        set(_webgpu_target_exists TRUE)
    endif()
    pulp_gpu_links_wgpu_native(_pulp_gpu_links_wgpu
        WEBGPU_TARGET ${_webgpu_target_exists})
    if(_pulp_gpu_links_wgpu)
        target_link_libraries(${target} ${provider_scope} webgpu)
    endif()
    if(PULP_HAS_WEBGPU)
        target_compile_definitions(${target} ${definition_scope} PULP_HAS_WEBGPU=1)
    endif()
endfunction()
