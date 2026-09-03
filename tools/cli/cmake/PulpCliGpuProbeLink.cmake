include_guard(GLOBAL)

function(pulp_link_cli_gpu_probe_recipes target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR
            "pulp_link_cli_gpu_probe_recipes requires an existing target: ${target}")
    endif()

    if(PULP_LINUX AND PULP_ENABLE_SCENE3D)
        # Keep the recipes archive outside the rescan group. It depends on view
        # and gpu-audio, which both carry render; grouping recipes with render
        # makes CMake replace those render edges with a group that depends back
        # on the same targets. Only the mutually dependent render/scene archives
        # need GNU ld rescanning.
        target_link_libraries(${target} PRIVATE
            pulp::tool-gpu-probe-recipes
            "$<LINK_GROUP:RESCAN,pulp::render,pulp::scene>")
    else()
        target_link_libraries(${target} PRIVATE
            pulp::tool-gpu-probe-recipes)
    endif()
endfunction()
