add_library(pulp-host)
add_library(pulp::host ALIAS pulp-host)

target_include_directories(pulp-host
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${PULP_ROOT_DIR}/core/format/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-host PRIVATE
    src/scanner.cpp
    src/signal_graph.cpp
    src/signal_graph_mailbox.cpp
    src/signal_graph_authoring.cpp
    src/spectral_graph.cpp
    src/signal_graph_subgraph.cpp
    src/signal_graph_process.cpp
    src/signal_graph_diagnostics.cpp
    src/signal_graph_control.cpp
    src/processor_signal_graph_binding.cpp
    src/processor_restore.cpp
    src/trigger_graph.cpp
    src/signal_graph_prepare.cpp
    src/signal_graph_reference_walk.cpp
    src/signal_graph_live_swap.cpp
    src/signal_graph_live_swap_catalog.cpp
    src/signal_graph_prepared_topology_edit.cpp
    src/prepared_description.cpp
    src/offline_signal_graph_host.cpp
    src/plugin_slot.cpp
    src/graph_serializer.cpp
    src/graph_identity.cpp
    src/voice_identity.cpp
    src/voice_template.cpp
    src/voice_bank_binding.cpp
    src/scan_blacklist.cpp
    src/scan_cache.cpp
    src/background_scanner.cpp
    src/isolated_scanner.cpp
    src/node_pack.cpp
    src/forge_descriptor_audit.cpp
    src/forge_catalog_export.cpp
    src/forge_catalog_export_sample_region.cpp
    src/forge_catalog_export_effects.cpp
    src/forge_catalog_export_eurorack_drums.cpp
    src/forge_catalog_export_dynamics.cpp
    src/forge_dynamics_catalog.cpp
    src/forge_distortion_catalog.cpp
    src/forge_synthesis_catalog.cpp
    src/forge_catalog_export_modulation.cpp
    src/forge_modulation_catalog.cpp
    src/forge_effect_modulation_catalog.cpp
    src/forge_catalog_export_lofi.cpp
    src/forge_lofi_catalog.cpp
    src/forge_catalog_export_pitch_sequencing.cpp
    src/forge_pitch_catalog.cpp
    src/forge_sequencing_catalog.cpp
    src/forge_catalog_export_space_synthesis.cpp
    src/forge_space_catalog.cpp
    src/forge_tape_catalog.cpp
    src/forge_catalog_json.cpp
    src/signal_graph_executor_routing.cpp
    src/graph_rt_report.cpp
    src/graph_timing_contract.cpp
    src/baked_graph_processor.cpp
    src/baked_graph_param_injection.cpp
    src/baked_codec.cpp
    src/anticipation_eligibility.cpp
    src/anticipation_partition.cpp
    src/anticipation_subgraph.cpp
    src/anticipation_lane.cpp
    src/timeline_automation_delivery.cpp
    src/timeline_device_resolver.cpp
    src/timeline_graph_binding.cpp
    src/timeline_offline_graph.cpp
    src/timeline_offline_renderer.cpp
    src/timeline_graph_binding_candidate.cpp
    src/timeline_graph_binding_routes.cpp
    src/timeline_graph_stretch_policy.cpp
    src/sample_region_plan.cpp
    src/sample_region_proof.cpp
    src/sample_region_parameters.cpp
    src/sample_region_runtime.cpp
    src/bounded_delay_descriptor.cpp
)

target_link_libraries(pulp-host PUBLIC pulp::audio pulp::format pulp::graph pulp::midi pulp::playback pulp::runtime pulp::state)
target_link_libraries(pulp-host PUBLIC pulp::native-components)
# GraphEditorView, the audio-UI consumer of the host graph, lives in
# pulp-view-graph-editor (core/view/audio), so the host does not depend on view-core.
target_link_libraries(pulp-host PRIVATE pulp::platform)

# The GPU convolution realization is an explicit opt-in.  Keeping this edge
# disabled by default leaves the CPU catalog (and its zero-latency contract)
# unchanged for ordinary SDK/Forge consumers.  When enabled, the host owns the
# catalog factory and links the SDK's two-lane authenticated route.
option(PULP_HOST_ENABLE_GPU_CONVOLUTION
       "Expose the opt-in authenticated GPU convolution Forge realization" OFF)
if(PULP_HOST_ENABLE_GPU_CONVOLUTION)
    if(NOT TARGET pulp::gpu-audio)
        message(FATAL_ERROR
            "PULP_HOST_ENABLE_GPU_CONVOLUTION requires the pulp::gpu-audio target")
    endif()
    target_compile_definitions(pulp-host PUBLIC PULP_HOST_ENABLE_GPU_CONVOLUTION=1)
    target_link_libraries(pulp-host PUBLIC pulp::gpu-audio)
endif()
# node_pack.cpp dlopen()s signed node packs (desktop + Android). iOS never builds
# pulp-host. Link the dynamic-loader libs where they are a separate library.
if(NOT WIN32)
    target_link_libraries(pulp-host PRIVATE ${CMAKE_DL_LIBS})
endif()

# CLAP plugin loader + scanner (requires CLAP SDK)
if(PULP_HAS_CLAP)
    target_sources(pulp-host PRIVATE
        src/plugin_slot_clap.cpp
        src/scanner_clap.cpp
    )
    target_link_libraries(pulp-host PRIVATE clap)
    target_compile_definitions(pulp-host PRIVATE PULP_HOST_HAS_CLAP=1)
endif()

# scanner_vst3.cpp reads Contents/Resources/moduleinfo.json via
# choc::json (pure JSON, no VST3 SDK required). choc headers come
# transitively through pulp::runtime's PUBLIC include dir. Always
# compiled so plugins that ship moduleinfo.json get stable FUIDs
# even on non-VST3-SDK builds.
target_sources(pulp-host PRIVATE src/scanner_vst3.cpp)

# VST3 loader (requires VST3 SDK)
if(PULP_HAS_VST3)
    target_sources(pulp-host PRIVATE src/plugin_slot_vst3.cpp)
    target_link_libraries(pulp-host PRIVATE vst3-sdk)
    target_compile_definitions(pulp-host PUBLIC PULP_HOST_HAS_VST3=1)
endif()

# LV2 loader (requires LV2 headers). Plug-in descriptions, instantiation, and
# state go through lilv, vendored with serd, sord, sratom, and zix under
# external/lilv (ISC). They compile into pulp-host as hidden-visibility objects,
# so pulp-host gains no new link or install dependency and a hosted plug-in that
# carries its own lilv cannot interpose on ours.
if(TARGET lv2-headers)
    set(_lilv_root "${PULP_ROOT_DIR}/external/lilv")
    set(_lilv_sources
        zix/src/allocator.c zix/src/btree.c zix/src/bump_allocator.c
        zix/src/digest.c zix/src/errno_status.c zix/src/filesystem.c
        zix/src/hash.c zix/src/path.c zix/src/ring.c zix/src/status.c
        zix/src/string_view.c zix/src/system.c zix/src/tree.c
        serd/src/base64.c serd/src/byte_source.c serd/src/env.c serd/src/n3.c
        serd/src/node.c serd/src/read_utf8.c serd/src/reader.c serd/src/string.c
        serd/src/system.c serd/src/uri.c serd/src/writer.c
        sord/src/sord.c sord/src/syntax.c
        sratom/src/sratom.c
        lilv/src/collections.c lilv/src/dylib.c lilv/src/instance.c
        lilv/src/lib.c lilv/src/load_skimmer.c lilv/src/node.c
        lilv/src/node_hash.c lilv/src/node_skimmer.c lilv/src/plugin.c
        lilv/src/pluginclass.c lilv/src/port.c lilv/src/query.c
        lilv/src/scalepoint.c lilv/src/state.c lilv/src/string_util.c
        lilv/src/syntax_skimmer.c lilv/src/sys_util.c lilv/src/type_skimmer.c
        lilv/src/ui.c lilv/src/uris.c lilv/src/world.c)
    if(WIN32)
        list(APPEND _lilv_sources
            zix/src/win32/environment_win32.c zix/src/win32/filesystem_win32.c
            zix/src/win32/system_win32.c zix/src/win32/win32_util.c
            zix/src/win32/sem_win32.c zix/src/win32/thread_win32.c)
    else()
        list(APPEND _lilv_sources
            zix/src/posix/environment_posix.c zix/src/posix/filesystem_posix.c
            zix/src/posix/system_posix.c zix/src/posix/thread_posix.c)
        if(APPLE)
            list(APPEND _lilv_sources zix/src/darwin/sem_darwin.c)
        else()
            list(APPEND _lilv_sources zix/src/posix/sem_posix.c)
        endif()
    endif()
    list(TRANSFORM _lilv_sources PREPEND "${_lilv_root}/")
    add_library(pulp-host-lilv OBJECT ${_lilv_sources})
    set_target_properties(pulp-host-lilv PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        C_VISIBILITY_PRESET hidden
        POSITION_INDEPENDENT_CODE ON)
    target_include_directories(pulp-host-lilv
        PUBLIC
            $<BUILD_INTERFACE:${_lilv_root}/zix/include>
            $<BUILD_INTERFACE:${_lilv_root}/serd/include>
            $<BUILD_INTERFACE:${_lilv_root}/sord/include>
            $<BUILD_INTERFACE:${_lilv_root}/sratom/include>
            $<BUILD_INTERFACE:${_lilv_root}/lilv/include>
        PRIVATE
            ${_lilv_root}/zix/src
            ${_lilv_root}/serd/src
            ${_lilv_root}/sord/src
            ${_lilv_root}/lilv/src)
    target_compile_definitions(pulp-host-lilv
        PUBLIC ZIX_STATIC SERD_STATIC SORD_STATIC SRATOM_STATIC LILV_STATIC
        PRIVATE
            ZIX_INTERNAL SERD_INTERNAL SORD_INTERNAL SRATOM_INTERNAL LILV_INTERNAL
            SERD_VERSION="0.32.10" SORD_VERSION="0.16.22" LILV_VERSION="0.28.0")
    if(APPLE)
        # macOS reports a POSIX version but has no posix_fadvise; the upstream
        # build detects that by probing, so say it here.
        target_compile_definitions(pulp-host-lilv PRIVATE
            _DARWIN_C_SOURCE HAVE_POSIX_FADVISE=0)
    elseif(WIN32)
        target_compile_definitions(pulp-host-lilv PRIVATE
            WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    elseif(CMAKE_SYSTEM_NAME MATCHES "Linux|GNU")
        target_compile_definitions(pulp-host-lilv PRIVATE
            _GNU_SOURCE _POSIX_C_SOURCE=200809L _XOPEN_SOURCE=600)
    else()
        target_compile_definitions(pulp-host-lilv PRIVATE
            _POSIX_C_SOURCE=200809L _XOPEN_SOURCE=600)
    endif()
    # Third-party C: build it as its author does, without Pulp's warning set.
    if(MSVC)
        target_compile_options(pulp-host-lilv PRIVATE /W0)
    else()
        target_compile_options(pulp-host-lilv PRIVATE -w)
    endif()
    target_link_libraries(pulp-host-lilv PRIVATE lv2-headers)

    target_sources(pulp-host PRIVATE src/plugin_slot_lv2.cpp
        $<TARGET_OBJECTS:pulp-host-lilv>)
    target_include_directories(pulp-host PRIVATE
        $<TARGET_PROPERTY:pulp-host-lilv,INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_definitions(pulp-host PRIVATE
        $<TARGET_PROPERTY:pulp-host-lilv,INTERFACE_COMPILE_DEFINITIONS>)
    target_link_libraries(pulp-host PRIVATE lv2-headers)
    if(UNIX AND NOT APPLE)
        find_package(Threads REQUIRED)
        target_link_libraries(pulp-host PRIVATE Threads::Threads m)
    endif()
    target_compile_definitions(pulp-host PUBLIC PULP_HOST_HAS_LV2=1)
    if(UNIX)
        target_link_libraries(pulp-host PRIVATE ${CMAKE_DL_LIBS})
    endif()
endif()

# Audio Unit loader + API-based scanner (macOS only, no external SDK)
if(APPLE)
    target_sources(pulp-host PRIVATE
        src/plugin_slot_au.mm
        src/plugin_slot_auv3.mm
        src/scanner_au.mm
    )
    target_link_libraries(pulp-host PRIVATE
        "-framework AVFoundation"
        "-framework AudioToolbox"
        "-framework AudioUnit"
        "-framework CoreAudio"
        "-framework CoreAudioKit"
        "-framework CoreFoundation"
    )
    target_compile_definitions(pulp-host PUBLIC PULP_HOST_HAS_AU=1)
endif()

# Host-owned container for an embedded plugin editor. Real on macOS; every other
# platform gets the stub, so the per-format negotiation code still compiles and
# create_hosted_editor() degrades to "no editor".
if(APPLE)
    target_sources(pulp-host PRIVATE src/hosted_editor_container_mac.mm)
    target_link_libraries(pulp-host PRIVATE "-framework AppKit")
    target_compile_definitions(pulp-host PUBLIC PULP_HOST_HAS_EDITOR_CONTAINER=1)
else()
    target_sources(pulp-host PRIVATE src/hosted_editor_container_stub.cpp)
endif()

# Public sample-region authoring example and catalog schemas ship in the SDK
# even when its producer disables examples and tests.
install(FILES
    "${PULP_ROOT_DIR}/examples/sample-region-allpass/CMakeLists.txt"
    "${PULP_ROOT_DIR}/examples/sample-region-allpass/README.md"
    "${PULP_ROOT_DIR}/examples/sample-region-allpass/allpass_graph.hpp"
    "${PULP_ROOT_DIR}/examples/sample-region-allpass/allpass_graph.cpp"
    "${PULP_ROOT_DIR}/examples/sample-region-allpass/allpass_processor.hpp"
    "${PULP_ROOT_DIR}/examples/sample-region-allpass/allpass_processor.cpp"
    "${PULP_ROOT_DIR}/examples/sample-region-allpass/installed_consumer.cpp"
    DESTINATION share/pulp/examples/sample-region-allpass)
install(FILES
    "${PULP_ROOT_DIR}/docs/status/dsp-capabilities.schema.json"
    "${PULP_ROOT_DIR}/docs/status/forge-catalog.schema.json"
    DESTINATION share/pulp)
