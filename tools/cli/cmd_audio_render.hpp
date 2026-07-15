#pragma once

// pulp audio render — offline scenario render of a plugin bundle.
//
// Sibling of `pulp audio validate` (which keeps its no-plugin contract): render
// loads an explicit `--plugin <bundle>` through pulp::host::PluginSlot, drives it
// offline from declarative flags, and writes a WAV plus the same metrics JSON
// emitted by `pulp audio validate summarize --json`, with no DAW and no audio
// device.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pulp::cli {

// ── Parsed render request (separated from execution so it is unit-testable) ──

// `Noise` and `Impulse` exist for the latency probe: a delay cannot be measured
// out of silence, and a tone whose period is a whole number of samples is
// ambiguous (delays one period apart look identical). Broadband, aperiodic
// stimuli are what the probe policies need.
enum class AudioRenderInputKind { Silence, Sine, Wav, Noise, Impulse };
enum class AudioRenderWavFormat { Int16, Int24, Float32 };

/// How `--latency-report` turns the render into a delay measurement.
enum class AudioRenderLatencyPolicy {
    None,
    /// Null the output against the input delayed by D, sweeping D. Requires the
    /// plugin to be in a pass-through / bypass / fully-dry mode for the render
    /// (arrange it with `--param`). Checks every sample.
    DelayedNull,
    /// Locate a single impulse in the output and subtract its position in the
    /// input. Works for plugins that reshape the signal, where nulling is
    /// meaningless.
    Marker,
};

// A `--param <id>=<value>[@frame]` request. `value` is in the PLAIN parameter
// domain (the parameter's native min..max), matching PluginSlot::set_parameter
// and ParameterEvent::value — NOT normalized [0,1]. (The base-class arg is
// misleadingly named `normalized_value`, but every loader treats it as plain:
// VST3 converts plain→normalized internally, LV2/CLAP store the plain value.)
struct AudioRenderParam {
    std::uint32_t id = 0;
    float value = 0.0f;
    std::uint64_t frame = 0;  ///< Absolute render frame the change takes effect.
};

// A `--midi note:<note>,<vel>,<on_frame>[,<off_frame>]` request.
struct AudioRenderMidi {
    std::uint8_t channel = 0;
    std::uint8_t note = 0;
    std::uint8_t velocity = 0;
    std::uint64_t on_frame = 0;
    bool has_off = false;
    std::uint64_t off_frame = 0;
};

struct ParseAudioRenderResult {
    bool ok = false;
    bool help = false;
    int exit_code = 0;
    std::string error;  ///< Human-readable; caller prints to stderr.

    std::string plugin_path;
    std::string format = "clap";
    std::string unique_id;
    double sample_rate = 48000.0;
    std::uint32_t block = 512;
    std::uint32_t in_channels = 2;
    std::uint32_t out_channels = 2;
    std::uint64_t duration_frames = 0;  ///< Resolved from --duration-ms / --duration-frames.
    std::uint64_t tail_frames = 0;      ///< Additional silent-input capture after duration.

    AudioRenderInputKind input_kind = AudioRenderInputKind::Silence;
    std::string input_wav;
    double sine_hz = 0.0;
    double sine_dbfs = -6.0;
    std::uint64_t noise_seed = 1;      ///< `--input-signal noise[:<seed>]`.
    std::uint64_t impulse_frame = 0;   ///< `--input-signal impulse[:<frame>]`.

    /// `--latency-report <file>`: write a latency-evidence artifact there. This
    /// is a separate, versioned artifact — it never changes the shape of the
    /// `--manifest` metrics file or `--json` stdout.
    std::string latency_report_path;
    AudioRenderLatencyPolicy latency_policy = AudioRenderLatencyPolicy::None;
    std::int64_t latency_tolerance = 0;
    /// `--latency-intrinsic <n>`: delay the plugin adds for reasons that are not
    /// latency (leading silence in a known IR). Marker policy only.
    std::int64_t latency_intrinsic = 0;
    /// `--latency-expect <n>`: the INTENDED latency, pinned independently of what
    /// the plugin reports. Without it the proof is self-consistency only — a
    /// plugin whose true delay AND report both moved together still passes.
    std::optional<int> latency_expect;

    std::vector<AudioRenderParam> params;
    std::vector<AudioRenderParam> initial_params;
    std::vector<AudioRenderMidi> midi;

    /// Wall-clock/audio pre-roll before measured events. When omitted, the
    /// driver chooses a licensed-AU-safe default and zero for other formats.
    std::optional<std::uint32_t> warmup_ms;
    /// Discarded processing after --initial-param writes.
    std::optional<std::uint32_t> settle_ms;
    std::uint32_t timeout_ms = 0;  ///< 0 derives a bounded coordinator timeout.
    AudioRenderWavFormat wav_format = AudioRenderWavFormat::Int16;
    std::string out_wav;
    std::string manifest_path;
    bool json = false;
};

// Parse `pulp audio render` arguments. Never touches the filesystem or loads a
// plugin — pure string handling so the grammar is unit-testable. On a usage
// error, returns {ok=false, exit_code=2, error=...}; `--help`/`-h` returns
// {help=true, exit_code=0}.
ParseAudioRenderResult parse_audio_render_args(const std::vector<std::string>& args);

}  // namespace pulp::cli

// Entry point wired into `cmd_audio` dispatch.
int cmd_audio_render(const std::vector<std::string>& args);
int cmd_audio_render_worker(const std::vector<std::string>& args);
