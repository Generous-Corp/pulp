# Canonical graph implementation for isolated sample-region web builds.
include_guard(GLOBAL)

function(pulp_sample_region_web_sources ROOT OUT)
    set(_sources
        "${ROOT}/core/host/src/sample_region_parameters.cpp"
        "${ROOT}/core/host/src/sample_region_runtime.cpp"
        "${ROOT}/core/host/src/sample_region_plan.cpp"
        "${ROOT}/core/host/src/sample_region_proof.cpp"
        "${ROOT}/core/host/src/signal_graph.cpp"
        "${ROOT}/core/host/src/signal_graph_prepared_topology_edit.cpp"
        "${ROOT}/core/host/src/signal_graph_live_swap.cpp"
        "${ROOT}/core/host/src/signal_graph_reference_walk.cpp"
        "${ROOT}/core/host/src/signal_graph_executor_routing.cpp"
        "${ROOT}/core/host/src/graph_serializer.cpp"
        "${ROOT}/core/host/src/anticipation_eligibility.cpp"
        "${ROOT}/core/host/src/anticipation_partition.cpp"
        "${ROOT}/core/host/src/anticipation_subgraph.cpp"
        "${ROOT}/core/host/src/anticipation_lane.cpp"
        "${ROOT}/core/host/src/plugin_slot.cpp"
        "${ROOT}/core/host/src/timeline_device_resolver.cpp"
        "${ROOT}/core/format/src/graph_runtime_executor.cpp"
        "${ROOT}/core/format/src/graph_runtime_worker_pool.cpp"
        "${ROOT}/core/format/src/processor_node_adapter.cpp"
        "${ROOT}/core/format/src/processor_block_adapter.cpp"
        "${ROOT}/core/format/src/descriptor_validation.cpp"
        "${ROOT}/core/graph/src/graph_runtime_plan.cpp"
        "${ROOT}/core/graph/src/graph_runtime_buffer_assignment.cpp"
        "${ROOT}/core/graph/src/graph_runtime_levelization.cpp"
        "${ROOT}/core/audio/src/live_dsp_telemetry.cpp"
        "${ROOT}/core/audio/src/planar_audio_ring_buffer.cpp"
        "${ROOT}/core/runtime/src/scoped_no_alloc.cpp"
    )
    set(${OUT} ${_sources} PARENT_SCOPE)
endfunction()

function(pulp_sample_region_web_profile TARGET)
    if(CMAKE_INTERPROCEDURAL_OPTIMIZATION OR
       "${CMAKE_CXX_FLAGS} ${CMAKE_EXE_LINKER_FLAGS}" MATCHES "-flto")
        message(FATAL_ERROR "Sample-region web builds require a complete non-LTO EH closure")
    endif()
    set_property(TARGET ${TARGET} PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
    target_compile_options(${TARGET} PRIVATE -frtti -fwasm-exceptions -fno-lto)
    target_link_options(${TARGET} PRIVATE -fwasm-exceptions -fno-lto)
    if(EMSCRIPTEN)
        target_link_options(${TARGET} PRIVATE -sWASM_LEGACY_EXCEPTIONS=0 -sABORTING_MALLOC=0)
        target_compile_options(${TARGET} PRIVATE -sWASM_LEGACY_EXCEPTIONS=0)
    else()
        target_compile_options(${TARGET} PRIVATE "SHELL:-mllvm -wasm-use-legacy-eh=false")
        target_link_libraries(${TARGET} PRIVATE unwind)
    endif()
endfunction()
