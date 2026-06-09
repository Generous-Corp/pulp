#include <pulp/audio/signalsmith_time_pitch_processor.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#ifdef PULP_HAS_SIGNALSMITH_STRETCH
#include "signalsmith-stretch.h"
#endif

namespace pulp::audio {

namespace {

AnalyzerDescriptor make_signalsmith_descriptor() {
    PackageAnalyzerDescriptorInput input;
    input.provider_id = "package.signalsmith-stretch.time-pitch";
    input.package_id = "signalsmith-stretch";
    input.display_name = "Signalsmith Stretch Time/Pitch";
    input.version = "1.1.0";
    input.license_id = "MIT";
    input.url = "https://github.com/Signalsmith-Audio/signalsmith-stretch";
    input.capabilities = {AnalyzerCapability::TimeStretch,
                          AnalyzerCapability::PitchShift};
#ifdef PULP_HAS_SIGNALSMITH_STRETCH
    input.installed = true;
#else
    input.installed = false;
#endif
    input.platform_supported = true;
    input.license_accepted = true;
    input.execution_context = AnalyzerExecutionContext::BackgroundThread;
    input.supports_streaming_input = true;
    input.supports_offline_buffers = true;
    return make_package_analyzer_descriptor(input);
}

bool valid_prepare_config(const TimePitchPrepareConfig& config) noexcept {
    constexpr auto int_max = static_cast<std::uint64_t>(std::numeric_limits<int>::max());
    if (config.channels == 0 || static_cast<std::uint64_t>(config.channels) > int_max) {
        return false;
    }
    if (!(config.sample_rate > 0.0) || !std::isfinite(config.sample_rate) ||
        config.max_input_frames == 0 || config.max_output_frames == 0 ||
        config.max_input_frames > int_max || config.max_output_frames > int_max) {
        return false;
    }

    const auto channels = static_cast<std::size_t>(config.channels);
    const auto max_input_frames = static_cast<std::size_t>(config.max_input_frames);
    return max_input_frames <= std::numeric_limits<std::size_t>::max() / channels;
}

bool valid_process_spec(const TimePitchProcessSpec& spec) noexcept {
    return spec.time_ratio > 0.0 && std::isfinite(spec.time_ratio) &&
           std::isfinite(spec.pitch_shift_semitones) &&
           (spec.source_sample_rate == 0.0 ||
            (spec.source_sample_rate > 0.0 && std::isfinite(spec.source_sample_rate)));
}

std::uint64_t desired_output_frames(std::uint64_t input_frames,
                                    const TimePitchProcessSpec& spec) noexcept {
    const auto desired = std::ceil(static_cast<double>(input_frames) * spec.time_ratio);
    if (!(desired > 0.0) ||
        desired > static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
        return 0;
    }
    return static_cast<std::uint64_t>(desired);
}

TimePitchPrepareResult make_prepare_result(const AnalyzerDescriptor& descriptor,
                                             TimePitchPrepareStatus status) {
    TimePitchPrepareResult result;
    result.status = status;
    result.ok = status == TimePitchPrepareStatus::ok;
    result.provenance = analyzer_provenance_from_descriptor(descriptor,
                                                            "signalsmith-time-pitch");
    return result;
}

TimePitchProcessResult make_result(const AnalyzerDescriptor& descriptor,
                                   TimePitchProcessStatus status) {
    TimePitchProcessResult result;
    result.status = status;
    result.ok = status == TimePitchProcessStatus::ok;
    result.provenance = analyzer_provenance_from_descriptor(descriptor,
                                                            "signalsmith-time-pitch");
    return result;
}

}  // namespace

struct SignalsmithTimePitchProcessor::Impl {
#ifdef PULP_HAS_SIGNALSMITH_STRETCH
    std::unique_ptr<signalsmith::stretch::SignalsmithStretch<float>> stretch;
#endif
    TimePitchPrepareConfig config;
    std::vector<float> input_scratch;
    std::vector<float*> input_ptrs;
    std::vector<float*> output_ptrs;
    bool prepared = false;
};

SignalsmithTimePitchProcessor::SignalsmithTimePitchProcessor()
    : descriptor_(make_signalsmith_descriptor())
    , impl_(std::make_unique<Impl>()) {}

SignalsmithTimePitchProcessor::~SignalsmithTimePitchProcessor() = default;
SignalsmithTimePitchProcessor::SignalsmithTimePitchProcessor(
    SignalsmithTimePitchProcessor&&) noexcept = default;
SignalsmithTimePitchProcessor& SignalsmithTimePitchProcessor::operator=(
    SignalsmithTimePitchProcessor&&) noexcept = default;

bool SignalsmithTimePitchProcessor::available() noexcept {
#ifdef PULP_HAS_SIGNALSMITH_STRETCH
    return true;
#else
    return false;
#endif
}

const AnalyzerDescriptor& SignalsmithTimePitchProcessor::descriptor() const noexcept {
    return descriptor_;
}

TimePitchPrepareResult SignalsmithTimePitchProcessor::prepare(
    const TimePitchPrepareConfig& config) {
    if (descriptor_.id.empty()) descriptor_ = make_signalsmith_descriptor();
    release();
    if (!valid_prepare_config(config)) {
        return make_prepare_result(descriptor_, TimePitchPrepareStatus::invalid_config);
    }
    if (!available()) {
        return make_prepare_result(descriptor_, TimePitchPrepareStatus::unavailable);
    }

#ifdef PULP_HAS_SIGNALSMITH_STRETCH
    try {
        if (!impl_) impl_ = std::make_unique<Impl>();
        impl_->input_scratch.assign(static_cast<std::size_t>(config.channels) *
                                        static_cast<std::size_t>(config.max_input_frames),
                                    0.0f);
        impl_->input_ptrs.assign(config.channels, nullptr);
        impl_->output_ptrs.assign(config.channels, nullptr);
        impl_->stretch =
            std::make_unique<signalsmith::stretch::SignalsmithStretch<float>>();
        impl_->stretch->presetDefault(static_cast<int>(config.channels),
                                      static_cast<float>(config.sample_rate));
    } catch (const std::bad_alloc&) {
        release();
        return make_prepare_result(descriptor_, TimePitchPrepareStatus::allocation_failed);
    } catch (...) {
        release();
        return make_prepare_result(descriptor_, TimePitchPrepareStatus::setup_failed);
    }

    impl_->config = config;
    impl_->prepared = true;
    return make_prepare_result(descriptor_, TimePitchPrepareStatus::ok);
#else
    (void)config;
    return make_prepare_result(descriptor_, TimePitchPrepareStatus::unavailable);
#endif
}

void SignalsmithTimePitchProcessor::release() noexcept {
    if (impl_) {
        impl_->prepared = false;
        impl_->config = {};
        impl_->input_scratch.clear();
        impl_->input_ptrs.clear();
        impl_->output_ptrs.clear();
#ifdef PULP_HAS_SIGNALSMITH_STRETCH
        impl_->stretch.reset();
#endif
    }
}

TimePitchProcessResult SignalsmithTimePitchProcessor::process(
    BufferView<const float> input,
    BufferView<float> output,
    const TimePitchProcessSpec& spec) {
    if (!available()) return make_result(descriptor_, TimePitchProcessStatus::unavailable);
    if (!impl_ || !impl_->prepared) {
        return make_result(descriptor_, TimePitchProcessStatus::not_prepared);
    }
    if (!valid_process_spec(spec) || input.empty() || output.empty()) {
        return make_result(descriptor_, TimePitchProcessStatus::invalid_config);
    }
    if (input.num_channels() != impl_->config.channels ||
        output.num_channels() != impl_->config.channels) {
        return make_result(descriptor_, TimePitchProcessStatus::channel_mismatch);
    }
    if (spec.source_sample_rate > 0.0 && spec.source_sample_rate != impl_->config.sample_rate) {
        return make_result(descriptor_, TimePitchProcessStatus::invalid_config);
    }

    auto input_frames = static_cast<std::uint64_t>(input.num_samples());
    if (spec.max_input_frames > 0) input_frames = std::min(input_frames, spec.max_input_frames);
    if (input_frames == 0 || input_frames > impl_->config.max_input_frames) {
        return make_result(descriptor_, TimePitchProcessStatus::frame_budget_exceeded);
    }

    const auto output_frames = desired_output_frames(input_frames, spec);
    if (output_frames == 0 || output_frames > impl_->config.max_output_frames ||
        output_frames > static_cast<std::uint64_t>(output.num_samples()) ||
        (spec.max_output_frames > 0 && output_frames > spec.max_output_frames)) {
        return make_result(descriptor_, TimePitchProcessStatus::frame_budget_exceeded);
    }

#ifdef PULP_HAS_SIGNALSMITH_STRETCH
    if (!impl_->stretch) return make_result(descriptor_, TimePitchProcessStatus::not_prepared);

    const auto max_input_frames = static_cast<std::size_t>(impl_->config.max_input_frames);
    for (std::uint32_t ch = 0; ch < impl_->config.channels; ++ch) {
        auto* scratch = impl_->input_scratch.data() +
                        static_cast<std::size_t>(ch) * max_input_frames;
        std::copy_n(input.channel_ptr(ch), static_cast<std::size_t>(input_frames), scratch);
        impl_->input_ptrs[ch] = scratch;
        impl_->output_ptrs[ch] = output.channel_ptr(ch);
    }

    try {
        impl_->stretch->setTransposeSemitones(static_cast<float>(spec.pitch_shift_semitones));
        impl_->stretch->process(impl_->input_ptrs.data(),
                                static_cast<int>(input_frames),
                                impl_->output_ptrs.data(),
                                static_cast<int>(output_frames));
    } catch (...) {
        return make_result(descriptor_, TimePitchProcessStatus::processing_failed);
    }

    auto result = make_result(descriptor_, TimePitchProcessStatus::ok);
    result.input_frames_consumed = input_frames;
    result.output_frames_produced = output_frames;
    return result;
#else
    return make_result(descriptor_, TimePitchProcessStatus::unavailable);
#endif
}

}  // namespace pulp::audio
