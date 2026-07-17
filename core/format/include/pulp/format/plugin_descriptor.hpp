#pragma once

// Static plugin metadata: the descriptor a Processor returns once at
// initialization, plus the bus, category, capability, and editor-size types it
// is built from. processor.hpp includes this header, so existing consumers keep
// compiling unchanged; new code should include it directly.

#include <cstdint>
#include <string>
#include <vector>

namespace pulp::format {

/// Editor size hints (in logical pixels). preferred is used for the
/// initial window size; min/max bound interactive resizing. A zero
/// max dimension means unbounded in that axis.
struct ViewSize {
    uint32_t preferred_width = 400;
    uint32_t preferred_height = 300;
    uint32_t min_width = 0;
    uint32_t min_height = 0;
    uint32_t max_width = 0;   ///< 0 = unbounded
    uint32_t max_height = 0;  ///< 0 = unbounded

    /// When > 0, the host should hold this aspect ratio (width/height)
    /// during interactive resize. pulp::view::ResizableShell owns the clamp + snap
    /// arithmetic. 0 means "any ratio"; hosts then let the user drag
    /// freely within [min, max]. Typical values: 16.0/9.0, 4.0/3.0,
    /// preferred_width/preferred_height.
    double aspect_ratio = 0.0;
};

/// Build a ViewSize from a design-import preferred size with sensible
/// derived bounds. Used by `Processor::view_size()`'s default when
/// `pulp_add_plugin(... DESIGN_WIDTH N DESIGN_HEIGHT N)` is set, and
/// callable directly by plugins that compute dimensions at runtime.
///
/// Derivation rules (only when the corresponding explicit value is 0):
///   - min = preferred * 2/3
///   - max = preferred * 2
///   - aspect_ratio = preferred_width / preferred_height
///
/// CLAP's `gui_can_resize` requires min > 0 (see clap_entry.hpp), so
/// the derived min is what makes corner-drag resize work across all
/// formats with no per-plugin override. A generated-plugin sidecar can
/// populate these inputs once import-design supplies them.
constexpr ViewSize view_size_from_design(uint32_t preferred_width,
                                          uint32_t preferred_height,
                                          uint32_t min_width = 0,
                                          uint32_t min_height = 0,
                                          uint32_t max_width = 0,
                                          uint32_t max_height = 0) {
    return ViewSize{
        preferred_width,
        preferred_height,
        min_width > 0 ? min_width : (preferred_width * 2) / 3,
        min_height > 0 ? min_height : (preferred_height * 2) / 3,
        max_width > 0 ? max_width : preferred_width * 2,
        max_height > 0 ? max_height : preferred_height * 2,
        preferred_height > 0
            ? static_cast<double>(preferred_width) / static_cast<double>(preferred_height)
            : 0.0,
    };
}

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

/// One complete host-selectable channel configuration. Entry indices match
/// PluginDescriptor::input_buses/output_buses. A descriptor may list multiple
/// configurations with identical bus topology but different widths (for
/// example mono, stereo, and 5.1 variants of the same effect).
struct BusLayoutConfiguration {
    std::vector<int> inputs;
    std::vector<int> outputs;
    std::string name;
};

/// Capability sidecar for the node ABI. New capability bits should be
/// appended here with false defaults so descriptor aggregate initializers
/// remain source-compatible.
struct NodeCapabilities {
    bool supports_mpe = false;
    bool supports_ump = false;
    bool supports_f64_audio = false;
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
    /// adapters that recognize MPE will run the inbound MIDI stream
    /// through an MpeVoiceTracker, build an MpeBuffer for the block, and
    /// make it available via Processor::mpe_input() during process().
    /// The standard process() signature is unchanged; plugins that don't
    /// set this flag see no MPE-specific behavior.
    bool supports_mpe = false;

    /// Opt in to the native MIDI 2.0 UMP sidecar. When true, format
    /// adapters that recognize UMP provide a UmpBuffer of full-resolution
    /// channel-voice packets (16-bit velocity, per-note pitch bend,
    /// per-note CCs) through Processor::ump_input() during process().
    /// Adapters without native UMP transport synthesise the buffer by
    /// converting the inbound MIDI 1.0 stream. `supports_mpe` and
    /// `supports_ump` are independent and can both be set.
    bool supports_ump = false;

    /// iOS-only: true when the plugin renders audio that must continue
    /// while the host app is backgrounded (live synth in AUM, looper
    /// that keeps running while the user switches apps, etc.). The
    /// host-app layer uses this flag to decide whether to set the
    /// `audio` UIBackgroundModes entitlement and keep the AVAudioSession
    /// active in background.
    ///
    /// Default false — most effects don't need background audio and
    /// setting the entitlement unnecessarily attracts App Store review
    /// scrutiny.
    bool ios_requires_background_audio = false;

    /// Tail time in samples (0 = no tail, -1 = infinite).
    /// Used by hosts to flush reverb/delay tails after playback stops.
    int tail_samples = 0;

    /// Optional contact info — appended here so existing positional
    /// aggregate initializers keep working. Surfaced by VST3
    /// PFactoryInfo::url/email, CLAP manufacturer_url/manufacturer_email,
    /// AU kAudioUnitProperty_URL. Leave empty to skip.
    std::string vendor_url;
    std::string vendor_email;

    /// Node ABI capability bits. This is the forward-compatible capability
    /// model; legacy supports_mpe/supports_ump/supports_f64_audio remain accepted and are OR'd
    /// into effective_capabilities().
    NodeCapabilities node_capabilities;

    /// Opt in to native double-precision audio processing. Appended after
    /// the original descriptor fields so positional aggregate initializers
    /// keep their existing meaning. Existing plugins leave this false and
    /// use the adapter-boundary f64->f32 compatibility path when a host
    /// supplies 64-bit buffers. Plugins that set it should override
    /// process_f64() for their real double-precision DSP; the default still
    /// converts through the f32 process() path so an early opt-in remains safe.
    bool supports_f64_audio = false;

    /// Explicit host-selectable layouts. Appended at the true end of the
    /// descriptor so every pre-existing positional aggregate initializer,
    /// including one that supplies supports_f64_audio, retains its meaning.
    /// Empty preserves legacy flexible mono/stereo negotiation.
    std::vector<BusLayoutConfiguration> supported_bus_layouts;

    NodeCapabilities effective_capabilities() const {
        return {
            .supports_mpe = supports_mpe || node_capabilities.supports_mpe,
            .supports_ump = supports_ump || node_capabilities.supports_ump,
            .supports_f64_audio = supports_f64_audio || node_capabilities.supports_f64_audio,
        };
    }

    /// Channel count of the first (main) input bus.
    int default_input_channels() const {
        return input_buses.empty() ? 0 : input_buses[0].default_channels;
    }
    /// Channel count of the first (main) output bus.
    int default_output_channels() const {
        return output_buses.empty() ? 0 : output_buses[0].default_channels;
    }
};

} // namespace pulp::format
