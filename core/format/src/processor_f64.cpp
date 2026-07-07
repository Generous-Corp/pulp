#include <pulp/format/processor.hpp>

#include <algorithm>
#include <span>

namespace pulp::format {

void Processor::prepare_f64_fallback_scratch(const PrepareContext& context) {
    const auto input_channels =
        context.input_channels > 0 ? static_cast<std::size_t>(context.input_channels) : 0u;
    const auto output_channels =
        context.output_channels > 0 ? static_cast<std::size_t>(context.output_channels) : 0u;
    const auto max_frames =
        context.max_buffer_size > 0 ? static_cast<std::size_t>(context.max_buffer_size) : 0u;
    f64_fallback_input_scratch_.resize(input_channels, max_frames);
    f64_fallback_output_scratch_.resize(output_channels, max_frames);

    const auto desc = descriptor();
    for (std::size_t i = 0; i < kF64FallbackMaxBuses; ++i) {
        const auto in_channels = fallback_bus_channels(
            desc.input_buses, i, input_channels);
        const auto out_channels = fallback_bus_channels(
            desc.output_buses, i, output_channels);
        f64_fallback_input_bus_scratch_[i].resize(in_channels, max_frames);
        f64_fallback_output_bus_scratch_[i].resize(out_channels, max_frames);
    }
}

void Processor::process_f64(
    audio::BufferView<double>& audio_output,
    const audio::BufferView<const double>& audio_input,
    midi::MidiBuffer& midi_in,
    midi::MidiBuffer& midi_out,
    const ProcessContext& context) {
    const auto frames = audio_output.num_samples();
    const auto input_channels = audio_input.num_channels();
    const auto output_channels = audio_output.num_channels();
    if (!f64_fallback_capacity_ok(input_channels, output_channels, frames)) {
        audio_output.clear();
        return;
    }

    auto f32_input = f64_fallback_input_scratch_.view().slice(0, frames);
    auto f32_output = f64_fallback_output_scratch_.view().slice(0, frames);

    for (std::size_t ch = 0; ch < input_channels; ++ch) {
        auto dst = f32_input.channel(ch);
        const auto src = audio_input.channel(ch);
        const auto copied = (std::min)(src.size(), dst.size());
        for (std::size_t i = 0; i < copied; ++i)
            dst[i] = static_cast<float>(src[i]);
        std::fill(dst.begin() + static_cast<std::ptrdiff_t>(copied),
                  dst.end(), 0.0f);
    }
    for (std::size_t ch = 0; ch < output_channels; ++ch) {
        std::fill(f32_output.channel(ch).begin(),
                  f32_output.channel(ch).end(), 0.0f);
    }

    const auto& const_input_scratch = f64_fallback_input_scratch_;
    auto f32_const_input = const_input_scratch.view().slice(0, frames);
    process(f32_output, f32_const_input, midi_in, midi_out, context);

    for (std::size_t ch = 0; ch < output_channels; ++ch) {
        const auto src = f32_output.channel(ch);
        auto dst = audio_output.channel(ch);
        const auto copied = (std::min)(src.size(), dst.size());
        for (std::size_t i = 0; i < copied; ++i)
            dst[i] = static_cast<double>(src[i]);
        std::fill(dst.begin() + static_cast<std::ptrdiff_t>(copied),
                  dst.end(), 0.0);
    }
}

void Processor::process_f64(
    ProcessBuffers64& audio,
    midi::MidiBuffer& midi_in,
    midi::MidiBuffer& midi_out,
    const ProcessContext& context) {
    if (requires_rich_f64_fallback(audio)) {
        process_f64_via_f32_process_buffers(audio, midi_in, midi_out, context);
        return;
    }

    auto* output = audio.main_output();
    if (!output) return;

    audio::BufferView<const double> empty_input;
    auto* input = audio.main_input();
    process_f64(*output, input ? *input : empty_input, midi_in, midi_out, context);
}

std::size_t Processor::fallback_bus_channels(const std::vector<BusInfo>& buses,
                                             std::size_t index,
                                             std::size_t prepared_main_channels) {
    std::size_t channels = 0;
    if (index < buses.size() && buses[index].default_channels > 0) {
        channels = static_cast<std::size_t>(buses[index].default_channels);
    }
    if (index == 0) channels = (std::max)(channels, prepared_main_channels);
    return channels;
}

bool Processor::requires_rich_f64_fallback(const ProcessBuffers64& audio) {
    return audio.inputs.active_count() > audio.inputs.active_count(BusRole::Main) ||
           audio.outputs.active_count() > audio.outputs.active_count(BusRole::Main);
}

bool Processor::f64_fallback_capacity_ok(std::size_t input_channels,
                                         std::size_t output_channels,
                                         std::size_t frames) const {
    const bool input_ok =
        input_channels == 0 ||
        (input_channels <= f64_fallback_input_scratch_.num_channels() &&
         frames <= f64_fallback_input_scratch_.num_samples());
    const bool output_ok =
        output_channels == 0 ||
        (output_channels <= f64_fallback_output_scratch_.num_channels() &&
         frames <= f64_fallback_output_scratch_.num_samples());
    return input_ok && output_ok;
}

bool Processor::f64_fallback_process_buffers_capacity_ok(
    const ProcessBuffers64& audio) const {
    if (audio.inputs.size() > kF64FallbackMaxBuses ||
        audio.outputs.size() > kF64FallbackMaxBuses) {
        return false;
    }

    for (std::size_t i = 0; i < audio.inputs.size(); ++i) {
        const auto& bus = audio.inputs[i];
        if (!bus.active()) continue;
        const auto& scratch = f64_fallback_input_bus_scratch_[i];
        if (bus.buffer.num_channels() > scratch.num_channels() ||
            bus.buffer.num_samples() > scratch.num_samples()) {
            return false;
        }
    }

    for (std::size_t i = 0; i < audio.outputs.size(); ++i) {
        const auto& bus = audio.outputs[i];
        if (!bus.active()) continue;
        const auto& scratch = f64_fallback_output_bus_scratch_[i];
        if (bus.buffer.num_channels() > scratch.num_channels() ||
            bus.buffer.num_samples() > scratch.num_samples()) {
            return false;
        }
    }
    return true;
}

void Processor::copy_f64_to_f32(audio::BufferView<float> dst,
                                const audio::BufferView<const double>& src) {
    const auto channels = (std::min)(dst.num_channels(), src.num_channels());
    for (std::size_t ch = 0; ch < channels; ++ch) {
        auto d = dst.channel(ch);
        const auto s = src.channel(ch);
        const auto frames = (std::min)(d.size(), s.size());
        for (std::size_t i = 0; i < frames; ++i)
            d[i] = static_cast<float>(s[i]);
        std::fill(d.begin() + static_cast<std::ptrdiff_t>(frames),
                  d.end(), 0.0f);
    }
    for (std::size_t ch = channels; ch < dst.num_channels(); ++ch) {
        auto d = dst.channel(ch);
        std::fill(d.begin(), d.end(), 0.0f);
    }
}

void Processor::copy_f32_to_f64(audio::BufferView<double> dst,
                                const audio::BufferView<const float>& src) {
    const auto channels = (std::min)(dst.num_channels(), src.num_channels());
    for (std::size_t ch = 0; ch < channels; ++ch) {
        auto d = dst.channel(ch);
        const auto s = src.channel(ch);
        const auto frames = (std::min)(d.size(), s.size());
        for (std::size_t i = 0; i < frames; ++i)
            d[i] = static_cast<double>(s[i]);
        std::fill(d.begin() + static_cast<std::ptrdiff_t>(frames),
                  d.end(), 0.0);
    }
    for (std::size_t ch = channels; ch < dst.num_channels(); ++ch) {
        auto d = dst.channel(ch);
        std::fill(d.begin(), d.end(), 0.0);
    }
}

void Processor::clear_active_outputs(ProcessBuffers64& audio) {
    for (std::size_t i = 0; i < audio.outputs.size(); ++i) {
        auto& bus = audio.outputs[i];
        if (bus.active()) bus.buffer.clear();
    }
}

void Processor::process_f64_via_f32_process_buffers(
    ProcessBuffers64& audio,
    midi::MidiBuffer& midi_in,
    midi::MidiBuffer& midi_out,
    const ProcessContext& context) {
    if (!f64_fallback_process_buffers_capacity_ok(audio)) {
        clear_active_outputs(audio);
        return;
    }

    for (std::size_t i = 0; i < audio.inputs.size(); ++i) {
        const auto& bus = audio.inputs[i];
        f64_fallback_input_views_[i].info = bus.info;
        if (bus.active()) {
            auto scratch =
                f64_fallback_input_bus_scratch_[i].view().slice(0, bus.num_samples());
            copy_f64_to_f32(scratch, bus.buffer);
            const auto& const_scratch = f64_fallback_input_bus_scratch_[i];
            f64_fallback_input_views_[i].buffer =
                const_scratch.view().slice(0, bus.num_samples());
        } else {
            f64_fallback_input_views_[i].buffer = {};
        }
    }

    for (std::size_t i = 0; i < audio.outputs.size(); ++i) {
        const auto& bus = audio.outputs[i];
        f64_fallback_output_views_[i].info = bus.info;
        if (bus.active()) {
            auto scratch =
                f64_fallback_output_bus_scratch_[i].view().slice(0, bus.num_samples());
            scratch.clear();
            f64_fallback_output_views_[i].buffer = scratch;
        } else {
            f64_fallback_output_views_[i].buffer = {};
        }
    }

    ProcessBuffers f32_audio{
        .inputs = ProcessBusBufferSet<const float>(
            std::span(f64_fallback_input_views_.data(), audio.inputs.size())),
        .outputs = ProcessBusBufferSet<float>(
            std::span(f64_fallback_output_views_.data(), audio.outputs.size())),
    };
    process(f32_audio, midi_in, midi_out, context);

    for (std::size_t i = 0; i < audio.outputs.size(); ++i) {
        auto& bus = audio.outputs[i];
        if (!bus.active()) continue;
        const auto& scratch = f64_fallback_output_bus_scratch_[i];
        copy_f32_to_f64(bus.buffer, scratch.view().slice(0, bus.num_samples()));
    }
}

} // namespace pulp::format
