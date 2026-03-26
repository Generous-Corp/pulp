add_library(pulp-audio)
add_library(pulp::audio ALIAS pulp-audio)

target_include_directories(pulp-audio
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-audio PRIVATE
    src/audio_file.cpp
)

target_link_libraries(pulp-audio PUBLIC pulp::runtime)

# CHOC headers (audio file I/O)
target_include_directories(pulp-audio PRIVATE
    $<BUILD_INTERFACE:${choc_SOURCE_DIR}>
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
        src/placeholder.cpp
    )
    file(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/src/placeholder.cpp
        "// pulp-audio: WASAPI backend — Phase 4+\nnamespace pulp::audio {}\n"
    )
elseif(UNIX AND NOT APPLE)
    target_sources(pulp-audio PRIVATE
        platform/linux/alsa_device.cpp
    )
    find_package(ALSA REQUIRED)
    target_link_libraries(pulp-audio PRIVATE ALSA::ALSA)
endif()
