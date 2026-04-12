#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/mpe_buffer.hpp>
#include <pulp/state/store.hpp>
#include <string>
#include <memory>
#include <vector>

namespace pulp::format {

/// Plugin category — determines bus layout expectations and DAW behavior.
enum class PluginCategory {
    Effect,      ///< Audio effect (takes input, produces output)
    Instrument,  ///< Synth/sampler (receives MIDI, produces audio)
    MidiEffect,  ///< MIDI processor (receives MIDI, produces MIDI)
};

/// Audio bus description — for multi-bus plugins (sidechain, aux, etc.)
///
/// Each bus has a name, default channel count, and whether it's optional.
/// Optional buses (like sidechains) can be deactivated by the host.
struct BusInfo {
    std::string name;
    int default_channels = 2;
    bool optional = false;  ///< true for sidechain buses that can be deactivated
};

/// Plugin metadata — declared once, immutable.
///
/// Every Processor subclass returns a PluginDescriptor from descriptor().
/// Format adapters use this to register the plugin with the host.
///
/// @code
/// PluginDescriptor descriptor() const override {
///     return {
///         .name = "MyGain",
///         .manufacturer = "Example",
///         .bundle_id = "com.example.mygain",
///         .version = "1.0.0",
///         .category = PluginCategory::Effect,
///     };
/// }
/// @endcode
struct PluginDescriptor {
    std::string name;
    std::string manufacturer;
    std::string bundle_id;
    std::string version;       ///< Semantic version string, e.g. "1.0.0"
    PluginCategory category = PluginCategory::Effect;

    /// Bus configuration — defaults to single stereo in/out for compatibility.
    /// Override input_buses/output_buses for multi-bus (sidechain, aux).
    std::vector<BusInfo> input_buses = {{"Main In", 2, false}};
    std::vector<BusInfo> output_buses = {{"Main Out", 2, false}};

    bool accepts_midi = false;   ///< true if plugin receives MIDI input
    bool produces_midi = false;  ///< true if plugin sends MIDI output

    /// Opt in to MPE (MIDI Polyphonic Expression). When true, format
    /// adapters that recognise MPE will run the inbound MIDI stream
    /// through an MpeVoiceTracker, build an MpeBuffer for the block, and
    /// make it available via Processor::mpe_input() during process().
    /// The standard process() signature is unchanged; plugins that don't
    /// set this flag see no MPE-specific behaviour.
    bool supports_mpe = false;

    /// Tail time in samples (0 = no tail, -1 = infinite).
    /// Used by hosts to flush reverb/delay tails after playback stops.
    int tail_samples = 0;

    /// Channel count of the first (main) input bus.
    int default_input_channels() const {
        return input_buses.empty() ? 0 : input_buses[0].default_channels;
    }
    /// Channel count of the first (main) output bus.
    int default_output_channels() const {
        return output_buses.empty() ? 0 : output_buses[0].default_channels;
    }
};

/// Prepare context — passed once before processing starts.
///
/// Contains the host's audio configuration. Use this to allocate
/// buffers, initialize filters at the correct sample rate, etc.
struct PrepareContext {
    double sample_rate = 48000.0;
    int max_buffer_size = 512;
    int input_channels = 2;
    int output_channels = 2;
};

/// Process context — passed every audio callback with transport state.
///
/// Fields are populated by the host. Not all hosts provide all fields —
/// check is_playing before using position data.
struct ProcessContext {
    double sample_rate = 0;
    int num_samples = 0;
    bool is_playing = false;
    bool is_recording = false;
    double tempo_bpm = 120.0;
    double position_beats = 0.0;   ///< Position in quarter notes
    int64_t position_samples = 0;
    int time_sig_numerator = 4;
    int time_sig_denominator = 4;
};

/// The plugin processor interface.
///
/// This is the central abstraction in Pulp. Plugin developers subclass
/// Processor and implement four methods:
///
/// - descriptor() — returns plugin metadata
/// - define_parameters() — registers parameters with the state store
/// - prepare() — allocates resources at the given sample rate
/// - process() — real-time audio callback
///
/// Format adapters (VST3, AU, CLAP) wrap a Processor instance and
/// translate between the host API and Pulp's interface. The developer
/// writes one processor; the build system creates multiple format targets.
///
/// @code
/// class MyGain : public pulp::format::Processor {
///     PluginDescriptor descriptor() const override { ... }
///     void define_parameters(state::StateStore& store) override { ... }
///     void prepare(const PrepareContext& ctx) override { ... }
///     void process(BufferView<float>& out, const BufferView<const float>& in,
///                  MidiBuffer& midi_in, MidiBuffer& midi_out,
///                  const ProcessContext& ctx) override { ... }
/// };
/// @endcode
///
/// ## Thread Safety
///
/// - process() is called on the **audio thread**. No allocation, no locks,
///   no exceptions, no I/O.
/// - prepare() and release() are called on the **host thread** with the
///   audio thread stopped.
/// - define_parameters() is called once during construction on the host thread.
class Processor {
public:
    virtual ~Processor() = default;

    /// Return the plugin's metadata. Called once during initialization.
    virtual PluginDescriptor descriptor() const = 0;

    /// Register parameters with the state store.
    /// Called once during construction. Add all automatable parameters here.
    virtual void define_parameters(state::StateStore& store) = 0;

    /// Prepare for processing. Allocate buffers, initialize filters.
    /// Called on the host thread with the audio thread stopped.
    virtual void prepare(const PrepareContext& context) = 0;

    /// Release resources. Called on the host thread with audio stopped.
    virtual void release() {}

    /// Latency in samples introduced by this processor (default 0).
    /// Override for plugins that buffer or lookahead (e.g., compressors,
    /// linear-phase EQs). Hosts use this for delay compensation.
    virtual int latency_samples() const { return 0; }

    /// Process one buffer of audio. Called on the real-time audio thread.
    ///
    /// @param audio_output  Output buffer to fill (main bus)
    /// @param audio_input   Input buffer to read (main bus)
    /// @param midi_in       Incoming MIDI events (sample-accurate)
    /// @param midi_out      Outgoing MIDI events (for MIDI effects)
    /// @param context       Transport state and timing
    ///
    /// For sidechain input, call sidechain_input() which returns nullptr
    /// if no sidechain is connected.
    virtual void process(
        audio::BufferView<float>& audio_output,
        const audio::BufferView<const float>& audio_input,
        midi::MidiBuffer& midi_in,
        midi::MidiBuffer& midi_out,
        const ProcessContext& context) = 0;

    /// Editor support. By default, all processors have an auto-generated editor
    /// built from their parameter definitions (using AutoUi). Override these to
    /// customize or disable the editor.

    /// Whether this processor has a GUI editor. Default true (AutoUi).
    virtual bool has_editor() const { return true; }

    /// Preferred editor window size in logical pixels.
    virtual std::pair<uint32_t, uint32_t> editor_size() const { return {400, 300}; }

    /// Access the parameter state store.
    /// Use state().get_value(id) to read parameter values in process().
    state::StateStore& state() { return *state_store_; }
    const state::StateStore& state() const { return *state_store_; }

    /// Access sidechain input buffer (set by format adapters before process).
    /// Returns nullptr if no sidechain is connected or the bus is inactive.
    const audio::BufferView<const float>* sidechain_input() const { return sidechain_; }

    /// Access the per-note MPE expression buffer for this block. Returns
    /// nullptr unless the plugin declared PluginDescriptor::supports_mpe = true
    /// and the host/format adapter populated it.
    const midi::MpeBuffer* mpe_input() const { return mpe_input_; }

    /// @internal Framework sets these during initialization / processing.
    void set_state_store(state::StateStore* store) { state_store_ = store; }
    /// @internal
    void set_sidechain(const audio::BufferView<const float>* sc) { sidechain_ = sc; }
    /// @internal Called by format adapters before process() when MPE is on.
    void set_mpe_input(const midi::MpeBuffer* mpe) { mpe_input_ = mpe; }

private:
    state::StateStore* state_store_ = nullptr;
    const audio::BufferView<const float>* sidechain_ = nullptr;
    const midi::MpeBuffer* mpe_input_ = nullptr;
};

/// Factory function type — plugins provide this to create processor instances.
using ProcessorFactory = std::unique_ptr<Processor>(*)();

} // namespace pulp::format
