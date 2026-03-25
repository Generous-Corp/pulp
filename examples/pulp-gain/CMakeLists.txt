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
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.pulp.gain"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0.0"
    )

    if(APPLE)
        set_target_properties(PulpGain_VST3 PROPERTIES
            XCODE_ATTRIBUTE_GENERATE_INFOPLIST_FILE YES
            XCODE_ATTRIBUTE_WRAPPER_EXTENSION "vst3"
        )
    endif()
endif()

# Audio Unit component bundle (macOS)
if(APPLE)
    add_library(PulpGain_AU MODULE
        au_register.cpp
        ${CMAKE_SOURCE_DIR}/core/format/src/au_adapter.mm
        ${CMAKE_SOURCE_DIR}/core/format/src/au_entry.mm
    )
    target_link_libraries(PulpGain_AU PRIVATE
        pulp::format
        "-framework AudioToolbox"
        "-framework AVFoundation"
        "-framework Foundation"
        "-framework CoreFoundation"
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

if(APPLE)
    # auval test — validates the AU component
    # auval -v aufx PGan Pulp
    add_test(
        NAME auval-PulpGain
        COMMAND ${CMAKE_COMMAND} -E env
            bash -c "cp -R ${CMAKE_BINARY_DIR}/AU/PulpGain.component ~/Library/Audio/Plug-Ins/Components/ && auval -v aufx PGan Pulp"
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
