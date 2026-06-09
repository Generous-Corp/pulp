#pragma once

#include <memory>

#include <pulp/audio/analyzer_provider.hpp>

namespace pulp::audio {

// Optional time/pitch processor backed by the `signalsmith-stretch` package
// when that package target is present. The class is always linkable: default
// builds report MissingPackage and return `unavailable` from prepare/process.
class SignalsmithTimePitchProcessor final : public TimePitchProcessor {
public:
    SignalsmithTimePitchProcessor();
    ~SignalsmithTimePitchProcessor() override;

    SignalsmithTimePitchProcessor(const SignalsmithTimePitchProcessor&) = delete;
    SignalsmithTimePitchProcessor& operator=(const SignalsmithTimePitchProcessor&) = delete;
    SignalsmithTimePitchProcessor(SignalsmithTimePitchProcessor&&) noexcept;
    SignalsmithTimePitchProcessor& operator=(SignalsmithTimePitchProcessor&&) noexcept;

    [[nodiscard]] static bool available() noexcept;

    [[nodiscard]] const AnalyzerDescriptor& descriptor() const noexcept override;
    [[nodiscard]] TimePitchPrepareResult prepare(
        const TimePitchPrepareConfig& config) override;
    void release() noexcept override;
    [[nodiscard]] TimePitchProcessResult process(BufferView<const float> input,
                                                 BufferView<float> output,
                                                 const TimePitchProcessSpec& spec) override;

private:
    struct Impl;

    AnalyzerDescriptor descriptor_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pulp::audio
