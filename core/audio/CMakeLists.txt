add_library(pulp-audio)
add_library(pulp::audio ALIAS pulp-audio)

target_include_directories(pulp-audio
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-audio PRIVATE
    src/audio_file.cpp
    src/window_enumerator.cpp
    src/format_registry.cpp
    src/codecs.c
    src/stb_vorbis_impl.c
    src/ogg_reader.cpp
    src/aiff_reader.cpp
    src/system_volume.cpp
    src/channel_set.cpp
    src/offline_processor.cpp
)

# CoreAudioFormat reader (macOS only — AAC, ALAC, CAF via ExtAudioFile)
if(APPLE)
    target_sources(pulp-audio PRIVATE src/coreaudio_format.mm)
endif()

target_link_libraries(pulp-audio PUBLIC pulp::runtime)

# CHOC headers (audio file I/O)
target_include_directories(pulp-audio PRIVATE
    $<BUILD_INTERFACE:${choc_SOURCE_DIR}>
)

# dr_libs and stb_vorbis (vendored, public domain / MIT)
target_include_directories(pulp-audio PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../external/dr_libs
)

# Platform-specific sources
if(APPLE AND NOT IOS)
    target_sources(pulp-audio PRIVATE
        platform/mac/coreaudio_device.mm
    )
    target_link_libraries(pulp-audio PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework CoreFoundation"
    )
elseif(WIN32)
    target_sources(pulp-audio PRIVATE
        platform/win/wasapi_device.cpp
    )
    target_link_libraries(pulp-audio PRIVATE
        ole32
        winmm
    )
elseif(UNIX AND NOT APPLE)
    target_sources(pulp-audio PRIVATE
        platform/linux/alsa_device.cpp
    )
    find_package(ALSA REQUIRED)
    target_link_libraries(pulp-audio PRIVATE ALSA::ALSA)

    # Optional JACK support — preferred when available (lower latency, inter-app routing)
    # Also works with PipeWire's JACK compatibility layer
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(JACK QUIET jack)
    endif()
    if(JACK_FOUND)
        target_sources(pulp-audio PRIVATE
            platform/linux/jack_device.cpp
        )
        target_include_directories(pulp-audio PRIVATE ${JACK_INCLUDE_DIRS})
        target_link_libraries(pulp-audio PRIVATE ${JACK_LIBRARIES})
        target_compile_definitions(pulp-audio PRIVATE PULP_HAS_JACK=1)
        message(STATUS "Pulp: JACK audio support enabled")
    else()
        message(STATUS "Pulp: JACK not found — ALSA-only audio")
    endif()
endif()
