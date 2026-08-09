# PulpAppTargets.cmake — standalone audio app + generic app targets.
#
# App-target helpers shared by source builds and installed SDK consumers.
# Both `_pulp_add_standalone` (the standalone audio-host shell used by
# `pulp_add_plugin(... FORMATS standalone)`) and `pulp_add_app`
# (generic non-plugin Pulp apps) live here.
function(_pulp_add_standalone target name bundle_id version processor_factory)
    if(NOT _PULP_STANDALONE_TARGET)
        message(FATAL_ERROR "pulp_add_plugin(${target}): Standalone requested but Pulp::standalone is unavailable")
    endif()

    # Find standalone entry (convention: main.cpp in source dir)
    set(standalone_entry "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
        set(standalone_entry "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
    endif()

    if(APPLE)
        add_executable(${target}_Standalone MACOSX_BUNDLE
            ${PULP_${target}_CORE_OBJECTS}
            ${standalone_entry}
        )
    else()
        add_executable(${target}_Standalone
            ${PULP_${target}_CORE_OBJECTS}
            ${standalone_entry}
        )
    endif()
    set(_standalone_target "${_PULP_STANDALONE_TARGET}")
    if(NOT _standalone_target)
        message(FATAL_ERROR
            "pulp_add_plugin(${target}): declared inspector shipping component is unavailable; configure/install Pulp with Inspector support")
    endif()
    target_link_libraries(${target}_Standalone PRIVATE ${target}_Core ${_standalone_target})
    if(PULP_${target}_CONTROL_CAPABILITIES)
        set(_control_factory_source
            "${CMAKE_CURRENT_BINARY_DIR}/${target}_standalone_control_factory.cpp")
        file(GENERATE OUTPUT "${_control_factory_source}" CONTENT
            "#include <pulp/format/standalone_control_host.hpp>\n#include <pulp/inspect/control_standalone_host.hpp>\nnamespace { [[maybe_unused]] const bool pulp_control_factory_installed = pulp::format::detail::install_standalone_control_host_factory(&pulp::inspect::make_control_standalone_host); }\n")
        target_sources(${target}_Standalone PRIVATE "${_control_factory_source}")
        target_link_libraries(${target}_Standalone PRIVATE
            ${_PULP_CONTROL_STANDALONE_TARGET})

        if("dev.pulp.ui/capture@1" IN_LIST PULP_${target}_CONTROL_CAPABILITIES OR
           "dev.pulp.ui/input@1" IN_LIST PULP_${target}_CONTROL_CAPABILITIES)
            target_link_libraries(${target}_Standalone PRIVATE ${_PULP_CONTROL_UI_TARGET})
        endif()
        if("dev.pulp.trace/control@1" IN_LIST PULP_${target}_CONTROL_CAPABILITIES OR
           "dev.pulp.trace/session-control@1" IN_LIST PULP_${target}_CONTROL_CAPABILITIES OR
           "dev.pulp.telemetry/subscribe@1" IN_LIST PULP_${target}_CONTROL_CAPABILITIES)
            target_link_libraries(${target}_Standalone PRIVATE ${_PULP_CONTROL_INSPECT_TARGET})
        endif()
        if("dev.pulp.runtime/evaluate@1" IN_LIST PULP_${target}_CONTROL_CAPABILITIES)
            set(_control_runtime_eval_factory_source
                "${CMAKE_CURRENT_BINARY_DIR}/${target}_standalone_runtime_eval_factory.cpp")
            file(GENERATE OUTPUT "${_control_runtime_eval_factory_source}" CONTENT
                "#include <pulp/format/processor.hpp>\n#include <pulp/format/view_bridge.hpp>\n#include <pulp/inspect/control_standalone_host.hpp>\n#include <pulp/inspect/runtime_eval_component.hpp>\n#include <memory>\nnamespace { std::shared_ptr<pulp::inspect::RuntimeEvaluator> pulp_make_control_runtime_evaluator(pulp::format::Processor&, pulp::format::ViewBridge& bridge) { return std::shared_ptr<pulp::inspect::RuntimeEvaluator>(pulp::inspect::make_script_runtime_evaluator([&bridge](const auto& visitor) { bridge.visit_scripted_ui(visitor); })); } [[maybe_unused]] const bool pulp_control_runtime_eval_factory_installed = pulp::inspect::detail::install_standalone_runtime_evaluator_factory(&pulp_make_control_runtime_evaluator); }\n")
            target_sources(${target}_Standalone PRIVATE
                "${_control_runtime_eval_factory_source}")
            target_link_libraries(${target}_Standalone PRIVATE
                ${_PULP_CONTROL_RUNTIME_EVAL_TARGET})
        endif()

        # The visible Standalone remains an ordinary application bundle. The
        # broker launches a separate flat, host-only realization because the
        # trusted inventory deliberately rejects bundle-internal executables.
        if(NOT processor_factory)
            message(FATAL_ERROR
                "pulp_add_plugin(${target}): CONTROL_CAPABILITIES require PROCESSOR_FACTORY for the dedicated host")
        endif()
        set(_control_host_entry
            "${CMAKE_CURRENT_BINARY_DIR}/${target}_control_host_entry.cpp")
        file(GENERATE OUTPUT "${_control_host_entry}" CONTENT
            "#include <pulp/format/headless.hpp>\n#include <pulp/format/standalone.hpp>\n#include <pulp/inspect/control_standalone_host.hpp>\n#include <algorithm>\n#include <atomic>\n#include <chrono>\n#include <cmath>\n#include <csignal>\n#include <memory>\n#include <thread>\nstd::unique_ptr<pulp::format::Processor> ${processor_factory}();\nnamespace { constexpr double sample_rate = 48000.0; constexpr int block_size = 64; std::atomic<bool> stopping{false}; void stop(int) { stopping.store(true, std::memory_order_relaxed); } }\nint main() { pulp::format::HeadlessHost app(&${processor_factory}); if (!app.valid() || app.processor() == nullptr) return 64; const auto& descriptor = app.descriptor(); const int input_channels = descriptor.input_buses.empty() ? 0 : std::max(descriptor.input_buses.front().default_channels, 0); const int output_channels = descriptor.output_buses.empty() ? 0 : std::max(descriptor.output_buses.front().default_channels, 0); app.prepare(sample_rate, block_size, input_channels, output_channels); pulp::format::detail::StandaloneTestInputHost test_input; test_input.prepare(true, 120.0); pulp::audio::Buffer<float> input(static_cast<std::size_t>(input_channels), block_size); pulp::audio::Buffer<float> output(static_cast<std::size_t>(output_channels), block_size); pulp::midi::MidiBuffer midi_in; pulp::midi::MidiBuffer midi_out; std::signal(SIGINT, stop); std::signal(SIGTERM, stop); auto control = pulp::inspect::make_control_standalone_host(); if (!control || !control->start(*app.processor(), app.state(), &test_input, sample_rate)) return 65; while (!stopping.load(std::memory_order_relaxed)) { control->poll(); midi_in.clear(); midi_in.clear_sysex(); midi_out.clear(); midi_out.clear_sysex(); const auto transport = test_input.begin_audio_block(); test_input.drain_midi_into(midi_in, block_size); midi_in.sort(); pulp::format::ProcessContext context; context.sample_rate = sample_rate; context.num_samples = block_size; context.process_mode = pulp::format::ProcessMode::Realtime; context.render_speed_hint = pulp::format::RenderSpeedHint::Realtime; context.is_playing = transport.playing; context.tempo_bpm = transport.tempo_bpm; context.position_samples = transport.position_samples; context.position_beats = static_cast<double>(transport.position_samples) / sample_rate * transport.tempo_bpm / 60.0; context.bar = static_cast<std::int64_t>(std::floor(context.position_beats / 4.0)); auto const_input_view = static_cast<const pulp::audio::Buffer<float>&>(input).view(); auto output_view = output.view(); app.process(output_view, const_input_view, midi_in, midi_out, context); test_input.end_audio_block(block_size); std::this_thread::sleep_for(std::chrono::milliseconds(1)); } control->stop(); test_input.release_test_input(); app.release(); return 0; }\n")
        add_executable(${target}_ControlHost
            ${PULP_${target}_CORE_OBJECTS}
            "${_control_host_entry}")
        target_link_libraries(${target}_ControlHost PRIVATE
            ${target}_Core ${_standalone_target} ${_PULP_CONTROL_STANDALONE_TARGET})
        if("dev.pulp.ui/capture@1" IN_LIST PULP_${target}_CONTROL_CAPABILITIES OR
           "dev.pulp.ui/input@1" IN_LIST PULP_${target}_CONTROL_CAPABILITIES)
            target_link_libraries(${target}_ControlHost PRIVATE ${_PULP_CONTROL_UI_TARGET})
        endif()
        if("dev.pulp.trace/control@1" IN_LIST PULP_${target}_CONTROL_CAPABILITIES OR
           "dev.pulp.trace/session-control@1" IN_LIST PULP_${target}_CONTROL_CAPABILITIES OR
           "dev.pulp.telemetry/subscribe@1" IN_LIST PULP_${target}_CONTROL_CAPABILITIES)
            target_link_libraries(${target}_ControlHost PRIVATE ${_PULP_CONTROL_INSPECT_TARGET})
        endif()
        if("dev.pulp.runtime/evaluate@1" IN_LIST PULP_${target}_CONTROL_CAPABILITIES)
            target_sources(${target}_ControlHost PRIVATE
                "${_control_runtime_eval_factory_source}")
            target_link_libraries(${target}_ControlHost PRIVATE
                ${_PULP_CONTROL_RUNTIME_EVAL_TARGET})
        endif()
        if(APPLE)
            target_link_options(${target}_ControlHost PRIVATE
                "LINKER:-dead_strip_dylibs")
        endif()
        target_include_directories(${target}_ControlHost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        _pulp_apply_ui_script_definition(${target}_ControlHost "${PULP_${target}_UI_SCRIPT}")
        _pulp_apply_view_mac_objc_suffix(${target}_ControlHost)
        set_target_properties(${target}_ControlHost PROPERTIES
            OUTPUT_NAME host
            RUNTIME_OUTPUT_DIRECTORY
                "${CMAKE_CURRENT_BINARY_DIR}/${target}_control_host")
        pulp_stage_runtime_dependencies(${target}_ControlHost)
        pulp_assert_runtime_dependencies_staged(${target}_ControlHost)
        include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PulpPortable.cmake")
        pulp_assert_portable_bundle(${target}_ControlHost)
        _pulp_attach_control_shipping(${target} ${target}_ControlHost Standalone
            "${target}.ControlHost.Standalone")

        # A bundle identifier is globally stable across target renames and
        # avoids unrelated projects replacing one another's catalog entries.
        string(TOLOWER "${bundle_id}" _pulp_control_host_id)
        string(REGEX REPLACE "[^a-z0-9_-]" "-" _pulp_control_host_id
            "${_pulp_control_host_id}")
        string(LENGTH "${_pulp_control_host_id}" _pulp_control_host_id_length)
        if(_pulp_control_host_id_length GREATER 111)
            string(SUBSTRING "${_pulp_control_host_id}" 0 111
                _pulp_control_host_id)
        endif()
        string(SHA256 _pulp_control_host_id_digest "${bundle_id}")
        string(SUBSTRING "${_pulp_control_host_id_digest}" 0 16
            _pulp_control_host_id_digest)
        string(APPEND _pulp_control_host_id "-${_pulp_control_host_id_digest}")
        if(NOT CMAKE_INSTALL_LIBEXECDIR)
            set(CMAKE_INSTALL_LIBEXECDIR libexec)
        endif()
        if(PULP_CONTROL_BROKER_PREFIX)
            set(_pulp_control_broker_prefix "${PULP_CONTROL_BROKER_PREFIX}")
        else()
            set(_pulp_control_broker_prefix "${CMAKE_INSTALL_PREFIX}")
        endif()
        set(_control_install_script
            "${CMAKE_CURRENT_BINARY_DIR}/${target}_install_control_host_$<CONFIG>.cmake")
        file(GENERATE OUTPUT "${_control_install_script}" CONTENT
            "set(PULP_CONTROL_HOST_ID \"${_pulp_control_host_id}\")\nset(PULP_CONTROL_HOST_SOURCE \"$<TARGET_FILE:${target}_ControlHost>\")\nset(PULP_CONTROL_HOST_MANIFEST \"$<TARGET_FILE_DIR:${target}_ControlHost>/${target}.inspector-capabilities.json\")\nset(PULP_CONTROL_HOST_RUNTIME_DIR \"$<TARGET_FILE_DIR:${target}_ControlHost>\")\nset(PULP_CONTROL_HOST_ROOT \"\$ENV{DESTDIR}${_pulp_control_broker_prefix}/${CMAKE_INSTALL_LIBEXECDIR}/pulp/control-hosts\")\ninclude(\"${CMAKE_CURRENT_FUNCTION_LIST_DIR}/install_control_host.cmake\")\n")
        install(SCRIPT "${_control_install_script}" COMPONENT Runtime)
        add_custom_target(${target}_RemoveControlHost
            COMMAND "${CMAKE_COMMAND}"
                -DPULP_CONTROL_HOST_ID=${_pulp_control_host_id}
                -DPULP_CONTROL_HOST_ROOT=${_pulp_control_broker_prefix}/${CMAKE_INSTALL_LIBEXECDIR}/pulp/control-hosts
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/remove_control_host.cmake"
            VERBATIM)
        set(PULP_${target}_CONTROL_HOST_ID "${_pulp_control_host_id}"
            CACHE INTERNAL "" FORCE)
    endif()
    _pulp_apply_ui_script_definition(${target}_Standalone "${PULP_${target}_UI_SCRIPT}")
    _pulp_apply_view_mac_objc_suffix(${target}_Standalone)
    target_include_directories(${target}_Standalone PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(${target}_Standalone PROPERTIES
        OUTPUT_NAME "${name}"
    )
    if(APPLE)
        set_target_properties(${target}_Standalone PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_BUNDLE_NAME "${name}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${bundle_id}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${version}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${version}"
        )
    endif()
    # Runtime sidecars: the wgpu runtime, the Apple @loader_path rpath, and
    # Skia's icudtl.dat on Windows. Unguarded on purpose —
    # pulp_stage_runtime_dependencies() is defined in both the source and
    # installed-SDK builds, and the old `if(COMMAND ...)` guard is what let
    # a missing definition silently skip staging entirely.
    pulp_stage_runtime_dependencies(${target}_Standalone)
    pulp_assert_runtime_dependencies_staged(${target}_Standalone)
    # Portability guard (runs AFTER the WebGPU dylib is bundled, so a correctly
    # set-up standalone is clean): fail/warn if the binary bakes a build-tree
    # absolute path — the "works on the build box, breaks when shared" footgun.
    include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PulpPortable.cmake")
    pulp_assert_portable_bundle(${target}_Standalone)
    _pulp_attach_plugin_runtime_manifest(${target} ${target}_Standalone)
    _pulp_attach_control_shipping(${target} ${target}_Standalone Standalone)
    # Linux+GNU-ld link-order fix: libskia.a → fontconfig. Same helper
    # used for pulp-cli (#1986) and pulp-import-design (#2018). Standalone
    # transitively pulls in pulp::view → pulp::canvas → libskia.a, which
    # references Fc* symbols. Re-mention fontconfig AFTER the archive so
    # the linker resolves them. No-op on macOS/Windows/Android.
    #
    # CMAKE_CURRENT_FUNCTION_LIST_DIR (NOT CMAKE_CURRENT_LIST_DIR or
    # CMAKE_SOURCE_DIR) — CMake's CMAKE_CURRENT_LIST_DIR inside a function
    # body resolves to the *caller's* dir, not where the function was
    # defined. CMAKE_CURRENT_FUNCTION_LIST_DIR (CMake 3.17+; Pulp pins
    # 3.24) is the one that consistently resolves to PulpUtils.cmake's
    # own dir — i.e., the in-tree tools/cmake/ during a source build,
    # and ~/.pulp/sdk/<ver>/lib/cmake/Pulp/ after install. The sibling
    # PulpLinkFontconfig.cmake ships alongside via root CMakeLists.txt's
    # install(FILES) block. Using CMAKE_SOURCE_DIR here broke Linux/Android
    # consumers because it resolved to the consumer's source tree.
    include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PulpLinkFontconfig.cmake")
    pulp_link_fontconfig_after_skia(${target}_Standalone)
endfunction()

# ── pulp_add_app ────────────────────────────────────────────────────────
# Usage:
#   pulp_add_app(MyApp
#       APP_NAME "My App"
#       BUNDLE_ID "com.mycompany.myapp"
#       VERSION "1.0.0"
#   )
function(pulp_add_app target)
    cmake_parse_arguments(APP
        ""
        "APP_NAME;BUNDLE_ID;VERSION"
        ""
        ${ARGN}
    )

    if(APPLE)
        add_executable(${target} MACOSX_BUNDLE)
    else()
        add_executable(${target})
    endif()

    if(APP_APP_NAME)
        target_compile_definitions(${target} PRIVATE
            PULP_APP_NAME="${APP_APP_NAME}"
        )
    endif()

    if(APP_BUNDLE_ID)
        target_compile_definitions(${target} PRIVATE
            PULP_BUNDLE_ID="${APP_BUNDLE_ID}"
        )
    endif()

    message(STATUS "Pulp app: ${target}")
endfunction()

# pulp_add_binary_data() lives in PulpEmbedData.cmake so callers (e.g.
# core/canvas — bundled-font registration, #932) can include it before the
# Pulp targets exist. The function definition is unchanged; this PulpUtils
# include() preserves the existing public surface of `include(PulpUtils)`.
include("${CMAKE_CURRENT_LIST_DIR}/PulpEmbedData.cmake")

# pulp_register_font() — public font-registration macro for plugin authors.
# Lives in its own file so SDK consumers who only want
# pulp_add_binary_data don't pay for the extra includes/parsing, and so the
# install layout can ship the font macro alongside the rest of the Pulp
# CMake helpers.
include("${CMAKE_CURRENT_LIST_DIR}/PulpFonts.cmake")
