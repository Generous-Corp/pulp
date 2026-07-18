add_library(pulp-audio)
add_library(pulp::audio ALIAS pulp-audio)

target_include_directories(pulp-audio
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-audio PRIVATE
    src/analyzer_provider.cpp
    src/audio_file.cpp
    src/wav_decoder.cpp
    src/audio_focus.cpp
    src/audio_probe_json.cpp
    src/audio_scope.cpp
    src/audio_scope_json.cpp
    src/window_enumerator.cpp
    src/format_registry.cpp
    src/codecs.c
    src/stb_vorbis_impl.c
    src/ogg_reader.cpp
    src/aiff_reader.cpp
    src/system_volume.cpp
    src/channel_set.cpp
    src/offline_processor.cpp
    src/streaming_writer.cpp
    src/voice_modulation_buffer.cpp
    src/voice_sum_mixer.cpp
    src/mmap_reader.cpp
    src/buffer_ops.cpp
    src/built_in_key_tempo_analyzer.cpp
    src/built_in_transient_classifier.cpp
    src/audio_stream_handoff.cpp
    src/instrument_envelope.cpp
    src/instrument_runtime.cpp
    src/instrument_voice_allocator.cpp
    src/live_dsp_telemetry.cpp
    src/live_dsp_telemetry_json.cpp
    src/loop_point_analyzer.cpp
    src/loop_playback_cursor.cpp
    src/loop_reader.cpp
    src/loop_renderer.cpp
    src/loop_types.cpp
    src/onset_detector.cpp
    src/planar_audio_ring_buffer.cpp
    src/published_sample_store.cpp
    src/streaming_sample_source.cpp
    src/zone_layer_select.cpp
    src/realtime_sample_recorder.cpp
    src/rolling_audio_capture_buffer.cpp
    src/sample_asset_io.cpp
    src/sample_heritage_json.cpp
    src/sample_heritage_schema.cpp
    src/sample_heritage_src.cpp
    src/sample_edit_document.cpp
    src/sample_key_map.cpp
    src/sample_mip_manifest.cpp
    src/sample_mip_sidecar.cpp
    src/sample_mip_sidecar_staging.cpp
    src/sample_pool.cpp
    src/sample_stream_service.cpp
    src/sample_stream_window.cpp
    src/sample_voice_renderer.cpp
    src/sample_zone_map.cpp
    src/sample_slot_materializer.cpp
    src/sample_slot_bank.cpp
    src/sampler_looper_metrics.cpp
    src/slice_map.cpp
    src/slice_point_analyzer.cpp
    src/signalsmith_time_pitch_processor.cpp
    src/waveform_overview_build.cpp
    src/waveform_overview_cache.cpp
    src/waveform_overview_serialization.cpp
    src/wav_metadata.cpp
    src/audio_device_manager.cpp
    src/audio_probe.cpp
    src/audio_boundary_report.cpp
)

# CoreAudioFormat reader (macOS only — AAC, ALAC, CAF via ExtAudioFile)
if(APPLE)
    target_sources(pulp-audio PRIVATE src/coreaudio_format.mm)
endif()

# Optional codec writers — conditionally compiled when packages are installed.
# Install via: pulp add libflac, pulp add lame --accept-license LGPL-2.0, etc.
if(TARGET FLAC)
    target_sources(pulp-audio PRIVATE src/flac_writer.cpp)
    target_link_libraries(pulp-audio PRIVATE FLAC)
    target_compile_definitions(pulp-audio PRIVATE PULP_HAS_LIBFLAC=1)
    message(STATUS "Pulp: FLAC write enabled (libflac)")
endif()

if(TARGET mp3lame)
    target_sources(pulp-audio PRIVATE src/mp3_writer.cpp)
    target_link_libraries(pulp-audio PRIVATE mp3lame)
    target_compile_definitions(pulp-audio PRIVATE PULP_HAS_LAME=1)
    message(STATUS "Pulp: MP3 write enabled (LAME — LGPL)")
endif()

if(TARGET fdk-aac)
    target_sources(pulp-audio PRIVATE src/aac_writer.cpp)
    target_link_libraries(pulp-audio PRIVATE fdk-aac)
    target_compile_definitions(pulp-audio PRIVATE PULP_HAS_FDK_AAC=1)
    message(STATUS "Pulp: AAC write enabled (FDK AAC)")
endif()

if(TARGET alac)
    target_sources(pulp-audio PRIVATE src/alac_writer.cpp)
    target_link_libraries(pulp-audio PRIVATE alac)
    target_compile_definitions(pulp-audio PRIVATE PULP_HAS_ALAC=1)
    message(STATUS "Pulp: ALAC write enabled (Apple ALAC — Apache 2.0)")
endif()

if(TARGET signalsmith-stretch)
    target_link_libraries(pulp-audio PRIVATE signalsmith-stretch)
    target_compile_definitions(pulp-audio PUBLIC PULP_HAS_SIGNALSMITH_STRETCH=1)
    message(STATUS "Pulp: Signalsmith Stretch time/pitch processor enabled")
endif()

target_link_libraries(pulp-audio PUBLIC pulp::runtime pulp::midi pulp::state pulp::signal)

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
        Advapi32
        winmm
    )
elseif(UNIX AND NOT APPLE AND NOT ANDROID)
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
        # Surface JACK availability to sibling subdirectories (e.g. test/)
        # so the JACK-only test target is registered only when the backend
        # was actually compiled into pulp-audio.
        set(PULP_JACK_AVAILABLE TRUE CACHE INTERNAL "JACK audio backend compiled into pulp-audio")
        message(STATUS "Pulp: JACK audio support enabled")
    else()
        message(STATUS "Pulp: JACK not found — ALSA-only audio")
    endif()
endif()
