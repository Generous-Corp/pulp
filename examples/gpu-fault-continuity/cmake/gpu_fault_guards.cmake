# The render_force_loss hook reaches the editor's render device through the
# pre-Vellum GpuSurface accessor. Once the render slice is provided by Vellum
# (TARGET Vellum::Gpu), that hook would be built against a surface it no longer
# owns, so the plug-in refuses to configure rather than test the wrong thing.
function(pulp_gpu_fault_refuse_vellum)
    if(TARGET Vellum::Gpu)
        message(FATAL_ERROR
            "gpu-fault-continuity refuses to build when Vellum::Gpu provides the render "
            "slice: its render_force_loss hook targets the pre-Vellum GpuSurface and must "
            "move to Vellum first. Configure with -DPULP_GPU_AUDIO_FAULT_HOOKS=OFF.")
    endif()
endfunction()
