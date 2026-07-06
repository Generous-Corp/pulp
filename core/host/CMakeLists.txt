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
    src/signal_graph_reference_walk.cpp
    src/offline_signal_graph_host.cpp
    src/plugin_slot.cpp
    src/graph_serializer.cpp
    src/scan_blacklist.cpp
    src/scan_cache.cpp
    src/background_scanner.cpp
    src/isolated_scanner.cpp
    src/node_pack.cpp
    src/signal_graph_executor_routing.cpp
    src/baked_graph_processor.cpp
    src/anticipation_eligibility.cpp
    src/anticipation_partition.cpp
    src/anticipation_subgraph.cpp
    src/anticipation_lane.cpp
)

target_link_libraries(pulp-host PUBLIC pulp::audio pulp::format pulp::graph pulp::midi pulp::runtime pulp::state)
target_link_libraries(pulp-host PUBLIC pulp::native-components)
target_link_libraries(pulp-host PRIVATE pulp::platform)
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

# LV2 loader (requires LV2 headers)
if(TARGET lv2-headers)
    target_sources(pulp-host PRIVATE src/plugin_slot_lv2.cpp)
    target_link_libraries(pulp-host PRIVATE lv2-headers)
    target_compile_definitions(pulp-host PUBLIC PULP_HOST_HAS_LV2=1)
    if(UNIX)
        target_link_libraries(pulp-host PRIVATE ${CMAKE_DL_LIBS})
    endif()
endif()

# Audio Unit loader + API-based scanner (macOS only, no external SDK)
if(APPLE)
    target_sources(pulp-host PRIVATE
        src/plugin_slot_au.mm
        src/scanner_au.mm
    )
    target_link_libraries(pulp-host PRIVATE
        "-framework AudioToolbox"
        "-framework AudioUnit"
        "-framework CoreAudio"
        "-framework CoreFoundation"
    )
    target_compile_definitions(pulp-host PUBLIC PULP_HOST_HAS_AU=1)
endif()
