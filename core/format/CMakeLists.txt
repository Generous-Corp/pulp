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
    src/validation_harness.cpp
)

target_link_libraries(pulp-format PUBLIC pulp::audio pulp::midi pulp::state)

# VST3 adapter (when SDK is available)
if(PULP_HAS_VST3)
    target_sources(pulp-format PRIVATE
        src/vst3_adapter.cpp
    )
    target_link_libraries(pulp-format PUBLIC vst3-sdk PRIVATE pulp::view)
    target_compile_definitions(pulp-format PRIVATE PULP_VST3=1 PULP_VST3_GUI=1)
endif()

# CLAP adapter
if(PULP_HAS_CLAP)
    target_sources(pulp-format PRIVATE
        src/clap_adapter.cpp
    )
    target_link_libraries(pulp-format PUBLIC clap)
    target_compile_definitions(pulp-format PRIVATE PULP_CLAP=1)
endif()

# LV2 adapter
if(PULP_HAS_LV2)
    target_sources(pulp-format PRIVATE
        src/lv2_adapter.cpp
    )
    target_link_libraries(pulp-format PUBLIC lv2-headers)
    target_compile_definitions(pulp-format PRIVATE PULP_LV2=1)
endif()

if(PULP_HAS_AUSDK)
    if(APPLE)
        target_sources(pulp-format PRIVATE
            src/au_v2_adapter.cpp
            src/au_v2_instrument.cpp
        )
    endif()
    target_link_libraries(pulp-format PUBLIC ausdk)
endif()

# AU v2 adapter uses Apple AudioUnitSDK — compiled per-plugin in the entry point,
# not in the shared library. The au_adapter.mm (AUv3) works on both macOS and iOS.
if(APPLE)
    target_compile_definitions(pulp-format PRIVATE PULP_AU=1)
    if(PULP_IOS)
        # iOS uses AUv3 only (no AU v2 component manager)
        target_link_libraries(pulp-format PRIVATE
            "-framework AudioToolbox"
            "-framework AVFoundation"
            "-framework UIKit"
        )
        target_compile_definitions(pulp-format PRIVATE PULP_IOS=1)
    endif()
endif()
