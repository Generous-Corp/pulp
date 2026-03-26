add_library(pulp-format)
add_library(pulp::format ALIAS pulp-format)

target_include_directories(pulp-format
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-format PRIVATE
    src/registry.cpp
    src/headless.cpp
)

target_link_libraries(pulp-format PUBLIC pulp::audio pulp::midi pulp::state)

# VST3 adapter (when SDK is available)
if(PULP_HAS_VST3)
    target_sources(pulp-format PRIVATE
        src/vst3_adapter.cpp
    )
    target_link_libraries(pulp-format PRIVATE vst3-sdk)
    target_compile_definitions(pulp-format PRIVATE PULP_VST3=1)
endif()

# CLAP adapter
if(PULP_HAS_CLAP)
    target_sources(pulp-format PRIVATE
        src/clap_adapter.cpp
    )
    target_link_libraries(pulp-format PRIVATE clap)
    target_compile_definitions(pulp-format PRIVATE PULP_CLAP=1)
endif()

# LV2 adapter
if(PULP_HAS_LV2)
    target_sources(pulp-format PRIVATE
        src/lv2_adapter.cpp
    )
    target_link_libraries(pulp-format PRIVATE lv2-headers)
    target_compile_definitions(pulp-format PRIVATE PULP_LV2=1)
endif()

# AU v2 adapter uses Apple AudioUnitSDK — compiled per-plugin in the entry point,
# not in the shared library. The au_v2_adapter.cpp is included by each plugin's
# au_v2_entry.cpp. The old au_adapter.mm (AUv3 approach) is kept for future AUv3 work.
if(APPLE)
    target_compile_definitions(pulp-format PRIVATE PULP_AU=1)
endif()
