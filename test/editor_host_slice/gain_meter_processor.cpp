#include "gain_meter_processor.hpp"

#include <pulp/state/store.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::slice {

std::unique_ptr<format::Processor> GainMeterProcessor::create() {
    return std::make_unique<GainMeterProcessor>();
}

format::PluginDescriptor GainMeterProcessor::descriptor() const {
    format::PluginDescriptor d;
    d.name = "EditorHostSlice";
    d.manufacturer = "Pulp";
    d.bundle_id = "dev.pulp.test.editor-host-slice";
    d.version = "1.0.0";
    d.category = format::PluginCategory::Effect;
    d.input_buses = {{"Main In", 2, false}};
    d.output_buses = {{"Main Out", 2, false}};
    return d;
}

void GainMeterProcessor::define_parameters(state::StateStore& store) {
    state::ParamInfo gain;
    gain.id = kGainParam;
    gain.name = "Gain";
    gain.range = {0.0f, 1.0f, kGainDefault};
    store.add_parameter(gain);
}

void GainMeterProcessor::prepare(const format::PrepareContext&) {
    meter_.peak.store(0.0f, std::memory_order_relaxed);
}

void GainMeterProcessor::process(audio::BufferView<float>& output,
                                 const audio::BufferView<const float>& input, midi::MidiBuffer&,
                                 midi::MidiBuffer&, const format::ProcessContext&) {
    const float gain = state().get_value(kGainParam);
    float peak = 0.0f;
    const std::size_t channels = std::min(output.num_channels(), input.num_channels());
    for (std::size_t ch = 0; ch < channels; ++ch) {
        const float* in = input.channel_ptr(ch);
        float* out = output.channel_ptr(ch);
        for (std::size_t i = 0; i < output.num_samples(); ++i) {
            out[i] = in[i] * gain;
            peak = std::max(peak, std::fabs(out[i]));
        }
    }
    meter_.peak.store(peak, std::memory_order_relaxed);
    meter_.publishes.fetch_add(1, std::memory_order_release);
}

std::size_t PeakMeterVisualPort::read_meter(std::size_t index, std::span<float> rms,
                                            std::span<float> peak) noexcept {
    if (index != 0)
        return 0;
    const float p = meter_.peak.load(std::memory_order_relaxed);
    if (!rms.empty())
        rms[0] = p;
    if (!peak.empty())
        peak[0] = p;
    return 1;
}

} // namespace pulp::slice
