cmake_minimum_required(VERSION 3.24)

# Installed-SDK consumer proof: a tiny CLAP plugin links the public GPU-audio
# transport and compiles a custom node. This deliberately exercises the SDK
# export and plugin helper, not a full product implementation.
if(NOT DEFINED PULP_BUILD_DIR)
    message(FATAL_ERROR "PULP_BUILD_DIR is required")
endif()

set(_root "${PULP_BUILD_DIR}/gpu-audio-sdk-consumer")
set(_prefix "${_root}/prefix")
set(_build "${_root}/build")
file(REMOVE_RECURSE "${_root}")
file(MAKE_DIRECTORY "${_root}/src")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${PULP_BUILD_DIR}"
    --prefix "${_prefix}" --config Release
    RESULT_VARIABLE _install_rc)
if(NOT _install_rc EQUAL 0)
    message(FATAL_ERROR "GPU-audio SDK install failed: ${_install_rc}")
endif()

file(WRITE "${_root}/src/consumer.cpp" [=[
#include <pulp/format/processor.hpp>
#include <pulp/gpu_audio/gpu_audio_node.hpp>

namespace {
class ProbeNode final : public pulp::gpu_audio::GpuAudioNode {
public:
    pulp::gpu_audio::GpuAudioNodeDescriptor descriptor() const override {
        pulp::gpu_audio::GpuAudioNodeDescriptor d;
        d.name = "sdk-probe"; d.input_channels = 1; d.output_channels = 1;
        d.block_size = 32; d.sample_rate = 48000; d.latency_blocks = 1;
        d.miss_policy = pulp::gpu_audio::MissPolicy::CpuFallback;
        d.supports_cpu_fallback = true; return d;
    }
    bool prepare() override { return true; }
    void process_block(const pulp::audio::BufferView<const float>& in,
                       pulp::audio::BufferView<float>& out, uint32_t n) override {
        for (uint32_t i = 0; i < n; ++i) out.channel_ptr(0)[i] = in.channel_ptr(0)[i];
    }
    void process_cpu_fallback(const pulp::audio::BufferView<const float>& in,
                              pulp::audio::BufferView<float>& out, uint32_t n) noexcept override {
        for (uint32_t i = 0; i < n; ++i) out.channel_ptr(0)[i] = in.channel_ptr(0)[i];
    }
};

class Consumer final : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {.name="GpuAudioSdkConsumer", .manufacturer="PulpSmoke",
                .bundle_id="com.pulp.gpu-audio-sdk-consumer", .version="0.1.0",
                .category=pulp::format::PluginCategory::Effect,
                .input_buses={{"In",1}}, .output_buses={{"Out",1}}};
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override { node_.prepare(); }
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        for (size_t i=0; i<out.num_samples(); ++i) out.channel_ptr(0)[i]=in.channel_ptr(0)[i];
    }
private: ProbeNode node_;
};
}
std::unique_ptr<pulp::format::Processor> create_consumer() {
    return std::make_unique<Consumer>();
}
]=])

file(WRITE "${_root}/clap_entry.cpp" [=[
#include <pulp/format/clap_entry.hpp>
#include <pulp/format/processor.hpp>
#include <memory>
std::unique_ptr<pulp::format::Processor> create_consumer();
PULP_CLAP_PLUGIN(create_consumer)
]=])

file(WRITE "${_root}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(GpuAudioSdkConsumer LANGUAGES CXX)
find_package(Pulp CONFIG REQUIRED)
if(NOT TARGET Pulp::gpu-audio)
  message(FATAL_ERROR "installed SDK does not export Pulp::gpu-audio")
endif()
pulp_add_plugin(GpuAudioSdkConsumer FORMATS CLAP SOURCES src/consumer.cpp
  PROCESSOR_FACTORY create_consumer PLUGIN_NAME "GpuAudioSdkConsumer"
  MANUFACTURER "PulpSmoke" BUNDLE_ID "com.pulp.gpu-audio-sdk-consumer"
  VERSION "0.1.0" CATEGORY Effect PLUGIN_CODE "GAsc" MANUFACTURER_CODE "Pulp")
target_link_libraries(GpuAudioSdkConsumer_Core PUBLIC Pulp::gpu-audio)
]=])
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${_root}" -B "${_build}"
    -DCMAKE_PREFIX_PATH=${_prefix} -DPulp_DIR=${_prefix}/lib/cmake/Pulp
    -DCMAKE_BUILD_TYPE=Release RESULT_VARIABLE _configure_rc
    OUTPUT_VARIABLE _configure_out ERROR_VARIABLE _configure_err)
if(NOT _configure_rc EQUAL 0)
    message(FATAL_ERROR "GPU-audio SDK consumer configure failed:\n${_configure_out}\n${_configure_err}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${_build}" --config Release
    --target GpuAudioSdkConsumer_CLAP --parallel 2 RESULT_VARIABLE _build_rc
    OUTPUT_VARIABLE _build_out ERROR_VARIABLE _build_err)
if(NOT _build_rc EQUAL 0)
    message(FATAL_ERROR "GPU-audio SDK consumer build failed:\n${_build_out}\n${_build_err}")
endif()
message(STATUS "Installed GPU-audio SDK consumer CLAP built successfully")
