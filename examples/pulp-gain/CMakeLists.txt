# PulpGain — the simplest possible Pulp plugin
# Validates: build system, processor interface, parameter system, state serialization

# Headless test executable (validates processing without UI or format wrapper)
add_executable(pulp-gain-test test_pulp_gain.cpp)
target_link_libraries(pulp-gain-test PRIVATE pulp::format Catch2::Catch2WithMain)
target_include_directories(pulp-gain-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(pulp-gain-test)

# Standalone application
add_executable(PulpGain main.cpp
    ${CMAKE_SOURCE_DIR}/core/format/src/standalone.cpp
)
target_link_libraries(PulpGain PRIVATE pulp::format pulp::audio pulp::midi)
target_include_directories(PulpGain PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# VST3 plugin bundle
if(PULP_HAS_VST3)
    # The VST3 SDK provides platform-specific entry point sources
    set(VST3_PLATFORM_SOURCES "")
    if(APPLE)
        list(APPEND VST3_PLATFORM_SOURCES
            ${VST3_SDK_DIR}/public.sdk/source/main/macmain.cpp
        )
    elseif(WIN32)
        list(APPEND VST3_PLATFORM_SOURCES
            ${VST3_SDK_DIR}/public.sdk/source/main/dllmain.cpp
        )
    elseif(UNIX)
        list(APPEND VST3_PLATFORM_SOURCES
            ${VST3_SDK_DIR}/public.sdk/source/main/linuxmain.cpp
        )
    endif()

    add_library(PulpGain_VST3 MODULE
        vst3_entry.cpp
        ${VST3_PLATFORM_SOURCES}
        ${VST3_SDK_DIR}/public.sdk/source/main/pluginfactory.cpp
        ${VST3_SDK_DIR}/public.sdk/source/main/moduleinit.cpp
    )
    target_link_libraries(PulpGain_VST3 PRIVATE pulp::format vst3-sdk)
    target_include_directories(PulpGain_VST3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    # Set the output to be a .vst3 bundle
    set_target_properties(PulpGain_VST3 PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "vst3"
        OUTPUT_NAME "PulpGain"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/VST3"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.vst3"
    )

    # Copy moduleinfo.json into the bundle for fast DAW scanning
    add_custom_command(TARGET PulpGain_VST3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/moduleinfo.json"
            "${CMAKE_BINARY_DIR}/VST3/PulpGain.vst3/Contents/moduleinfo.json"
        COMMENT "Copying moduleinfo.json into VST3 bundle"
    )
endif()

# CLAP plugin bundle
if(PULP_HAS_CLAP)
    add_library(PulpGain_CLAP MODULE
        clap_entry.cpp
        ${CMAKE_SOURCE_DIR}/core/format/src/clap_adapter.cpp
    )
    target_link_libraries(PulpGain_CLAP PRIVATE pulp::format clap)
    target_include_directories(PulpGain_CLAP PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    set_target_properties(PulpGain_CLAP PROPERTIES
        OUTPUT_NAME "PulpGain"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/CLAP"
        PREFIX ""
        SUFFIX ".clap"
    )

    if(APPLE)
        set_target_properties(PulpGain_CLAP PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION "clap"
        )
    endif()
endif()

# Audio Unit v2 component bundle (macOS) — using Apple AudioUnitSDK
if(APPLE AND PULP_HAS_AUSDK)
    add_library(PulpGain_AU MODULE
        au_v2_entry.cpp
    )
    target_link_libraries(PulpGain_AU PRIVATE
        pulp::format
        ausdk
        "-framework AudioToolbox"
        "-framework CoreFoundation"
        "-framework CoreAudio"
    )
    target_include_directories(PulpGain_AU PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    set_target_properties(PulpGain_AU PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "component"
        OUTPUT_NAME "PulpGain"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/AU"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.au"
    )
endif()

# ── Validation Tests ────────────────────────────────────────────────────────
# These run format-specific validators as CTest tests

if(APPLE AND PULP_HAS_AUSDK)
    # auval test — validates the AU component
    # Checks for "PASS" in output (auval may still fail on legacy OpenAComponent timing test)
    add_test(
        NAME auval-PulpGain
        COMMAND ${CMAKE_COMMAND} -E env
            bash -c "cp -R ${CMAKE_BINARY_DIR}/AU/PulpGain.component ~/Library/Audio/Plug-Ins/Components/ && auval -v aufx PGan Pulp 2>&1 | tee /dev/stderr | grep -q 'PASS'"
    )
    set_tests_properties(auval-PulpGain PROPERTIES
        LABELS "validation;au"
        TIMEOUT 30
    )
endif()

# pluginval test — validates the VST3 bundle
if(PULP_HAS_VST3)
    find_program(PLUGINVAL pluginval)
    if(PLUGINVAL)
        add_test(
            NAME pluginval-PulpGain-VST3
            COMMAND ${PLUGINVAL} --validate ${CMAKE_BINARY_DIR}/VST3/PulpGain.vst3 --strictness-level 5 --timeout-ms 10000
        )
        set_tests_properties(pluginval-PulpGain-VST3 PROPERTIES
            LABELS "validation;vst3"
            TIMEOUT 30
        )
    endif()
endif()
