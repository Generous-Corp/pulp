#pragma once

/// @file headless.hpp
/// Headless processing adapter for programmatic plugin use without UI or
/// audio device. Use cases: CI validation, batch rendering, golden-file
/// tests, benchmarks.

#include <pulp/format/processor.hpp>

namespace pulp::format {

/// Headless plugin host for testing and offline processing.
///
/// Wraps a Processor and its StateStore so you can drive audio processing
/// entirely from code. No audio device, no UI, no host DAW required.
///
/// @code
/// HeadlessHost host(MyPlugin::create);
/// host.prepare(48000, 512);
/// host.state().set_value(kGainID, -6.0f);
///
/// audio::Buffer<float> in(2, 512), out(2, 512);
/// auto in_view = in.view();
/// auto out_view = out.view();
/// host.process(out_view, in_view);
/// @endcode
class HeadlessHost {
public:
    /// Construct from a factory function that creates a Processor instance.
    explicit HeadlessHost(ProcessorFactory factory);

    /// Prepare the processor for audio rendering. Call before process().
    /// @param sample_rate       Sample rate in Hz.
    /// @param max_buffer_size   Maximum samples per block.
    /// @param input_channels    Number of input audio channels.
    /// @param output_channels   Number of output audio channels.
    void prepare(double sample_rate, int max_buffer_size,
                 int input_channels = 2, int output_channels = 2);

    /// Process a block of audio (no MIDI).
    /// @p input and @p output may alias for in-place processing.
    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input);

    /// Process a block of audio with explicit transport/timeline context.
    /// Any zero-valued `sample_rate` / `num_samples` fields are defaulted from
    /// the prepared host configuration and current buffer size.
    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 ProcessContext context);

    /// Process a block of audio with MIDI input and output.
    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out);

    /// Process a block of audio with MIDI and explicit transport/timeline
    /// context. This is the low-level seam for deterministic offline stepping.
    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out,
                 ProcessContext context);

    /// Release processing resources. Safe to call multiple times.
    void release();

    /// Access the parameter store for reading/writing parameter values.
    state::StateStore& state() { return store_; }
    /// @copydoc state()
    const state::StateStore& state() const { return store_; }

    /// The plugin's static descriptor (name, vendor, bus layout, etc.).
    const PluginDescriptor& descriptor() const { return descriptor_; }

    /// Serialize the current parameter state to a binary blob.
    std::vector<uint8_t> save_state() const { return store_.serialize(); }

    /// Restore parameter state from a binary blob.
    /// @return True on success.
    bool load_state(std::span<const uint8_t> data) { return store_.deserialize(data); }

private:
    std::unique_ptr<Processor> processor_;
    state::StateStore store_;
    PluginDescriptor descriptor_;
    double sample_rate_ = 48000.0;
};

} // namespace pulp::format
