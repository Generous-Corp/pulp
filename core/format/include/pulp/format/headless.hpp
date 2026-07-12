#pragma once

/// @file headless.hpp
/// Headless processing adapter for programmatic plugin use without UI or
/// audio device. Use cases: CI validation, batch rendering, golden-file
/// tests, benchmarks.

#include <pulp/audio/offline_processor.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/state/parameter_event_queue.hpp>

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

    /// Prepare with explicit resource limits.
    ///
    /// Returns false without calling Processor::prepare() when the processor's
    /// estimate exceeds any non-zero limit. This is the fail-closed test and
    /// batch-render path; the legacy prepare() overload remains source
    /// compatible and uses unlimited limits.
    [[nodiscard]] bool try_prepare(
        double sample_rate, int max_buffer_size,
        int input_channels = 2, int output_channels = 2,
        PrepareResourceLimits resource_limits = {});

    /// Prepare one of PluginDescriptor::supported_bus_layouts by index.
    /// Returns false for an undeclared/invalid layout or when resource limits
    /// reject the derived main-bus widths.
    [[nodiscard]] bool try_prepare_bus_layout(
        std::size_t layout_index, double sample_rate, int max_buffer_size,
        PrepareResourceLimits resource_limits = {});

    /// Last prepare-limit failure from try_prepare(). None means the last
    /// checked prepare either fit the limits or no processor exists.
    PrepareResourceLimit last_prepare_limit_failure() const {
        return last_prepare_limit_failure_;
    }

    /// Process a block of audio (no MIDI).
    /// @p input and @p output may alias for in-place processing.
    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input);

    /// Process a block of audio with explicit transport/timeline context.
    /// Any non-positive `sample_rate` / `num_samples` fields are defaulted
    /// from the prepared host configuration and current buffer size.
    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 ProcessContext context);

    /// Process with an independently supplied sidechain bus. The sidechain is
    /// never concatenated with or aliased to the main input; processors reach
    /// it through ProcessBuffers::sidechain_input() / sidechain_input().
    void process_with_sidechain(
        audio::BufferView<float>& output,
        const audio::BufferView<const float>& input,
        const audio::BufferView<const float>& sidechain,
        ProcessContext context = {});

    /// Process a block of audio with MIDI input and output.
    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out);

    /// Process a block with explicit sample-accurate parameter events.
    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 const state::ParameterEventQueue& param_events);

    /// Process a block of audio with MIDI and explicit transport/timeline
    /// context. This is the low-level seam for deterministic offline stepping.
    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out,
                 ProcessContext context);

    void process_with_sidechain(
        audio::BufferView<float>& output,
        const audio::BufferView<const float>& input,
        const audio::BufferView<const float>& sidechain,
        midi::MidiBuffer& midi_in,
        midi::MidiBuffer& midi_out,
        ProcessContext context);

    /// Low-level process call with MIDI, context, and parameter events.
    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out,
                 const state::ParameterEventQueue& param_events,
                 ProcessContext context);

    /// Process a double-precision block of audio (no MIDI).
    /// @p input and @p output may alias for in-place processing.
    void process_f64(audio::BufferView<double>& output,
                     const audio::BufferView<const double>& input);

    /// Process a double-precision block with explicit transport/timeline
    /// context. Any non-positive `sample_rate` / `num_samples` fields are
    /// defaulted from the prepared host configuration and current buffer size.
    void process_f64(audio::BufferView<double>& output,
                     const audio::BufferView<const double>& input,
                     ProcessContext context);

    /// Process a double-precision block with MIDI input and output.
    void process_f64(audio::BufferView<double>& output,
                     const audio::BufferView<const double>& input,
                     midi::MidiBuffer& midi_in,
                     midi::MidiBuffer& midi_out);

    /// Process a double-precision block with explicit sample-accurate
    /// parameter events.
    void process_f64(audio::BufferView<double>& output,
                     const audio::BufferView<const double>& input,
                     const state::ParameterEventQueue& param_events);

    /// Process a double-precision block with MIDI and explicit transport /
    /// timeline context.
    void process_f64(audio::BufferView<double>& output,
                     const audio::BufferView<const double>& input,
                     midi::MidiBuffer& midi_in,
                     midi::MidiBuffer& midi_out,
                     ProcessContext context);

    /// Low-level double-precision process call with MIDI, context, and
    /// parameter events.
    void process_f64(audio::BufferView<double>& output,
                     const audio::BufferView<const double>& input,
                     midi::MidiBuffer& midi_in,
                     midi::MidiBuffer& midi_out,
                     const state::ParameterEventQueue& param_events,
                     ProcessContext context);

    /// Render an effect-shaped in-memory audio file through this processor.
    ///
    /// The host must already be prepared for at least the largest scheduled
    /// block size. The rendered artifact has the same channel count as @p input.
    std::optional<audio::AudioFileData> render_offline(
        const audio::AudioFileData& input,
        const audio::OfflineRenderOptions& options = {});

    /// Offline render with a separate sidechain file. Sample rates and frame
    /// counts must match; channel widths may differ. Tail frames receive a
    /// silent sidechain after the supplied file ends.
    std::optional<audio::AudioFileData> render_offline_with_sidechain(
        const audio::AudioFileData& input,
        const audio::AudioFileData& sidechain,
        const audio::OfflineRenderOptions& options = {});

    /// Release processing resources. Safe to call multiple times.
    void release();

    /// True when the factory produced a processor instance.
    bool valid() const noexcept { return processor_ != nullptr; }

    /// The wrapped processor instance (null when the factory returned nullptr).
    /// Lets tests reach typed processor state — e.g. a custom meter readout —
    /// that the generic host interface does not expose.
    Processor* processor() noexcept { return processor_.get(); }
    /// @copydoc processor()
    const Processor* processor() const noexcept { return processor_.get(); }
    /// Typed access to the wrapped processor; returns nullptr if the dynamic
    /// type does not match @c T.
    template <class T>
    T* processor_as() noexcept { return dynamic_cast<T*>(processor_.get()); }

    /// Access the parameter store for reading/writing parameter values.
    state::StateStore& state() { return store_; }
    /// @copydoc state()
    const state::StateStore& state() const { return store_; }

    /// The plugin's static descriptor (name, vendor, bus layout, etc.).
    const PluginDescriptor& descriptor() const { return descriptor_; }

    /// Serialize the current plugin state to a binary blob.
    ///
    /// Includes StateStore plus any processor-owned plugin state exposed via
    /// Processor::serialize_plugin_state().
    std::vector<uint8_t> save_state() const;

    /// Restore plugin state from a binary blob.
    /// @return True on success.
    bool load_state(std::span<const uint8_t> data);

private:
    std::optional<audio::AudioFileData> render_offline_impl(
        const audio::AudioFileData& input,
        const audio::AudioFileData* sidechain,
        const audio::OfflineRenderOptions& options);
    // The store is declared before the Processor so it is destroyed after it.
    // `Processor::state()` dereferences a pointer to this store, and a Processor
    // may read it from its destructor or from a worker thread that destructor is
    // about to join. Reversing these two lines hands that thread a freed store.
    state::StateStore store_;
    std::unique_ptr<Processor> processor_;
    PluginDescriptor descriptor_;
    double sample_rate_ = 48000.0;
    PrepareResourceLimit last_prepare_limit_failure_ = PrepareResourceLimit::None;
};

} // namespace pulp::format
