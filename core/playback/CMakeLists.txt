add_library(pulp-playback
    src/audio_renderer.cpp
    src/automation_cursor.cpp
    src/automation_program.cpp
    src/automation_program_compiler.cpp
    src/compile_executor.cpp
    src/note_renderer.cpp
    src/program.cpp
    src/program_compiler.cpp
    src/schedule_ahead.cpp
    src/stable_renderer_shell.cpp
    src/track_automation_compiler.cpp
    src/track_automation_program.cpp
    src/track_automation_renderer.cpp
    src/transport.cpp
)
add_library(pulp::playback ALIAS pulp-playback)

target_compile_features(pulp-playback PUBLIC cxx_std_20)

target_include_directories(pulp-playback
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(pulp-playback
    PUBLIC
        pulp::runtime
        pulp::timebase
        pulp::timeline
        pulp::audio
        pulp::midi
)

if(CMAKE_SYSTEM_NAME STREQUAL "WASI" OR
   CMAKE_SYSTEM_NAME STREQUAL "Emscripten" OR EMSCRIPTEN)
    target_compile_definitions(pulp-playback PRIVATE
        PULP_COMPILE_EXECUTOR_DISABLE_THREADS=1)
endif()

# Compile the complete portable playback lane exactly as threadless WebAssembly
# consumers do. This object target is the native compile-only mirror of the
# curated WAM/WebCLAP closure.
if(PULP_BUILD_TESTS AND NOT MSVC)
    add_library(timeline-program-threadless-no-exceptions-check OBJECT
        src/audio_renderer.cpp
        src/automation_cursor.cpp
        src/automation_program.cpp
        src/automation_program_compiler.cpp
        src/compile_executor.cpp
        src/note_renderer.cpp
        src/program.cpp
        src/program_compiler.cpp
        src/schedule_ahead.cpp
        src/stable_renderer_shell.cpp
        src/track_automation_compiler.cpp
        src/track_automation_program.cpp
        src/track_automation_renderer.cpp
        src/transport.cpp
    )
    target_compile_features(timeline-program-threadless-no-exceptions-check PRIVATE cxx_std_20)
    target_include_directories(timeline-program-threadless-no-exceptions-check PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(timeline-program-threadless-no-exceptions-check PRIVATE
        pulp::runtime pulp::timebase pulp::timeline pulp::audio pulp::midi)
    target_compile_definitions(timeline-program-threadless-no-exceptions-check PRIVATE
        PULP_COMPILE_EXECUTOR_DISABLE_THREADS=1)
    target_compile_options(timeline-program-threadless-no-exceptions-check PRIVATE
        -fno-exceptions -fno-rtti)
endif()
