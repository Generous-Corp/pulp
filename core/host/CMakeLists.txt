add_library(pulp-host)
add_library(pulp::host ALIAS pulp-host)

target_include_directories(pulp-host
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-host PRIVATE
    src/scanner.cpp
    src/signal_graph.cpp
    src/plugin_slot.cpp
    src/graph_serializer.cpp
    src/scan_blacklist.cpp
)

target_link_libraries(pulp-host PUBLIC pulp::audio pulp::midi pulp::runtime)

# CLAP plugin loader + scanner (requires CLAP SDK)
if(PULP_HAS_CLAP)
    target_sources(pulp-host PRIVATE
        src/plugin_slot_clap.cpp
        src/scanner_clap.cpp
    )
    target_link_libraries(pulp-host PRIVATE clap)
    target_compile_definitions(pulp-host PRIVATE PULP_HOST_HAS_CLAP=1)
endif()

# VST3 loader (requires VST3 SDK)
if(PULP_HAS_VST3)
    target_sources(pulp-host PRIVATE src/plugin_slot_vst3.cpp)
    target_link_libraries(pulp-host PRIVATE vst3-sdk)
    target_compile_definitions(pulp-host PRIVATE PULP_HOST_HAS_VST3=1)
endif()

# LV2 loader (requires LV2 headers)
if(TARGET lv2-headers)
    target_sources(pulp-host PRIVATE src/plugin_slot_lv2.cpp)
    target_link_libraries(pulp-host PRIVATE lv2-headers)
    target_compile_definitions(pulp-host PRIVATE PULP_HOST_HAS_LV2=1)
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
    target_compile_definitions(pulp-host PRIVATE PULP_HOST_HAS_AU=1)
endif()
