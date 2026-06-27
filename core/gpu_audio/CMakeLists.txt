add_library(pulp-gpu-audio)
add_library(pulp::gpu-audio ALIAS pulp-gpu-audio)

target_include_directories(pulp-gpu-audio
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-gpu-audio
    PRIVATE
        src/gpu_audio_transport.cpp
)

target_compile_features(pulp-gpu-audio PUBLIC cxx_std_20)

# GPU-agnostic real-time transport: depends only on the audio buffer/ring
# primitives and the lock-free runtime. Later slices add pulp::signal (CPU
# fallbacks like PartitionedConvolver) and pulp::render (GPU nodes) when the
# code actually uses them.
find_package(Threads REQUIRED)
target_link_libraries(pulp-gpu-audio
    PUBLIC
        pulp::audio
        pulp::runtime
    PRIVATE
        Threads::Threads
)
