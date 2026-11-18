#pragma once

/// @file standalone_config.hpp
/// StandaloneConfig: what a standalone app runs with (devices, rate, buffer,
/// transport, test and capture options). Plain data with no dependency on the
/// processor or view headers, so a settings UI can name it without pulling in
/// the whole standalone app (standalone.hpp includes this header).

#include <functional>
#include <string>
#include <vector>

namespace pulp::format {

struct StandaloneConfig {
    /// `midi_input_id` value that opens no MIDI input at all. An empty id opens
    /// the first port.
    static constexpr const char* kNoMidiInput = "pulp:no-midi-input";

    std::string audio_device_id;
    std::string midi_input_id;
    double sample_rate = 48000.0;
    int buffer_size = 256;
    int output_channels = 2;
    int input_channels = 0;
    // Optional host constraints. When non-empty, the standalone settings UI
    // only offers these values and persisted settings outside the list are
    // clamped back to the first allowed value.
    std::vector<double> allowed_sample_rates;
    std::vector<int> allowed_buffer_sizes;
    // Capability flag for apps/instruments that do not consume audio input.
    // The standalone host derives this from the processor descriptor at
    // startup, and the settings UI uses it to avoid presenting an unusable
    // input-device workflow for instrument-only apps.
    bool supports_audio_input = true;
    // Effects usually want the test signal as input. Instruments have no
    // audio input, so their test tone should exercise the selected output
    // device directly.
    bool route_test_signal_to_output = false;
    // When true, run_with_editor() avoids showing/activating the native
    // window. Use with screenshot_path for CI/test smoke runs.
    bool headless = false;
    // When false, run_with_editor() hosts the editor directly and omits the
    // built-in Settings tab.
    bool show_settings_tab = true;

    // Remember the user's audio/MIDI device selection (+ sample rate, buffer, transport)
    // across launches, keyed by the plugin name. On by default; a developer can set this
    // false to always start from the configured defaults. Saved whenever settings change,
    // restored at startup (the first launch keeps the configured defaults).
    bool persist_settings = true;

    // Development Inspector activation. Empty/"off" is inert, "local" owns
    // only the in-window overlay, and agent profiles start an authenticated
    // endpoint.
    // Capability ids are only used with the "custom" profile. Kept as plain
    // strings so public standalone headers remain independent of inspector SDK
    // types when inspector support is compiled out.
    std::string inspector_profile;
    std::vector<std::string> inspector_capabilities;
    // When non-empty, run_with_editor() installs a one-shot idle callback
    // that captures the first painted frame via WindowHost::capture_png()
    // and writes to this path, then closes the window. Codified in the SDK
    // (not per-app) so every pulp::format::StandaloneApp consumer gets
    // headless screenshot capture for free. Set via set_config() or by
    // parsing `--screenshot=PATH` from argv in main().
    std::string screenshot_path;
    // Frames to wait before capture. Default 30 (~0.5s @60fps) gives the
    // first React-driven layout + effects pass time to settle.
    int screenshot_frame_delay = 30;

    // A screenshot-only launch creates NO audio system and NO audio device: it
    // paints a few frames, writes the PNG, and exits, so the render callback
    // could only ever push silence at whoever is at the machine. Set this true
    // (or export PULP_SCREENSHOT_KEEP_AUDIO=1) when the capture must show a UI
    // driven by live audio, a meter or scope reading real signal. Requesting
    // any live readout (audio_probe_json_path, audio_scope_json_path,
    // audio_capture_wav_path, audio_capture_rolling_path) keeps audio on its
    // own; this flag is for the case where only the pixels need it.
    bool screenshot_keeps_audio = false;

    // Synthetic key-sequence driver for the test harness. When non-empty,
    // run_with_editor() presses each key in turn through the host's REAL key
    // path once the UI has settled, so a keyboard-driven behaviour can be
    // proven on the shipping surface instead of only in a unit test. The
    // spec is comma-separated and accepts modifiers, e.g.
    // "down,down,cmd+a,return". Set via set_config() or PULP_TEST_KEY_SEQUENCE.
    // An unparseable spec is rejected at startup rather than partially run.
    std::string test_key_sequence;
    // Frames to wait before the first key and between consecutive keys. The
    // gap has to outlast whatever the UI does in response to a press
    // (animation, relayout, a script tick) or the next key lands mid-update.
    int test_key_frame_delay = 20;
    // The contract editor's window: the usable screen area (points) it must
    // fit into when non-zero (zero reads the main screen), a hook called with
    // the native window (an NSWindow* on macOS) before the loop runs, and a
    // hook called once per frame of the loop. They let a test drive the
    // window's resize delegate without a visible screen.
    double test_screen_visible_width = 0.0;
    double test_screen_visible_height = 0.0;
    std::function<void(void* native_window)> test_on_editor_window;
    std::function<void()> test_on_editor_frame;
    // When non-empty, capture a PNG into this directory after every key,
    // named "key-<NN>-<name>.png", plus "key-00-initial.png" before the first
    // press. That per-press series is what turns "the sequence ran" into
    // evidence of what each individual press did.
    std::string test_key_shot_dir;

    // When non-empty, run_with_editor() arms the same one-shot frame-delay
    // path as `screenshot_path` and, after the delay, writes the live output
    // probe's latest snapshot (peak/RMS/dBFS/clip/NaN/silence counters) as a
    // JSON object to this path, then exits. This is the programmatic readout
    // of the live Audio Inspector for agents and CI, distinct from the
    // offline `pulp audio validate` Doctor. Set via set_config() or by reading
    // PULP_AUDIO_PROBE_JSON. Only meaningful when PULP_ENABLE_AUDIO_PROBES is
    // ON; probes-off standalone builds reject this request before opening the
    // editor so callers do not mistake an unsupported binary for silence.
    std::string audio_probe_json_path;

    // Programmatic live scope capture over the output-boundary probe's copied
    // capture ring. Like audio_probe_json_path, this is a dev/agent readout and
    // is only meaningful when PULP_ENABLE_AUDIO_PROBES is ON.
    std::string audio_scope_json_path;
    int audio_scope_window_samples = 2048;
    std::string audio_scope_trigger = "rising-zero";
    int audio_scope_channel = 0;

    // Programmatic live capture-to-WAV over the same output-boundary probe ring.
    // Writes a WAV the offline `pulp audio validate` verbs can read, then the
    // one-shot exits. Only meaningful when PULP_ENABLE_AUDIO_PROBES is ON.
    // `audio_capture_wav_frames` is the ring window in samples (0 = as much as the
    // ring holds, clamped to the capture cap). Captures all output channels.
    std::string audio_capture_wav_path;
    int audio_capture_wav_frames = 0;

    // Programmatic live ROLLING capture-to-WAV. Unlike audio_capture_wav_path
    // (which dumps the probe FIFO's EARLIEST window), this taps the output
    // boundary into a RollingAudioCaptureBuffer and writes the LAST
    // `audio_capture_rolling_frames` frames as a float WAV the offline `pulp
    // audio validate` verbs can read, the steady-state window `doctor`/`compare`
    // want, with no int16 quantization floor. The one-shot then exits. Only
    // meaningful when PULP_ENABLE_AUDIO_PROBES is ON. Captures all output channels.
    std::string audio_capture_rolling_path;
    int audio_capture_rolling_frames = 0;
    // Rolling capture sample format: false → float32 (default, full precision),
    // true → int24 (≈ −144 dBFS floor, smaller file, universal DAW compatibility).
    bool audio_capture_rolling_int24 = false;

    // Built-in tempo source. The standalone host has no DAW providing
    // transport, so it acts as one: it surfaces `tempo_bpm` / time signature on
    // every ProcessContext block, and advances `position_beats` from
    // `position_samples` when the transport is rolling. Plugins that branch on
    // `is_playing` or read `position_beats` therefore behave the same way in
    // `pulp run` as they do in a host without any per-plugin glue.
    double tempo_bpm = 120.0;
    int time_sig_numerator = 4;
    int time_sig_denominator = 4;
    bool transport_playing = true; // default-on so MIDI/tempo plugins are immediately useful
    bool transport_recording = false;

    // Adds Pulp's floating Musical Typing Keyboard to this standalone app.
    //
    // OPT-IN, AND OFF BY DEFAULT. This flag gates the ENTIRE feature, not just
    // the keyboard's visibility: leave it false and the app registers no menu
    // command and installs no key handler, so Cmd+K is inert and nothing
    // appears in any menu. That combination is the tell, a broken shortcut
    // would still leave the menu item behind, so a missing menu item means
    // this flag, not a handler bug. Said plainly because reading it as
    // "keyboard starts hidden" costs an afternoon debugging a key route that
    // was never installed.
    //
    // Once enabled, the keyboard stays hidden until the user picks
    // "Musical Typing Keyboard" from the application or Window menu, or presses Cmd+K
    // (Ctrl+K off Apple). Notes enter through the standalone host's lock-free
    // UI MIDI path, so they reach the processor only if it accepts MIDI input;
    // on a processor that ignores MIDI the keys are silent by construction.
    bool enable_musical_typing_keyboard = false;

    // Separate high-risk acknowledgement for arbitrary evaluation in the live
    // scripted UI realm. No profile or persisted setting implies this bit.
    // Appended to preserve every legacy positional aggregate initializer.
    bool inspector_runtime_eval = false;

    // Appended to preserve every legacy positional aggregate initializer.
    // Remember the plug-in's own state (parameters plus anything it saves with
    // serialize_plugin_state) across launches, in the same settings file as the
    // device selection. Saved when the app quits and shortly after the state
    // stops changing; restored at startup before the editor opens. Applies only
    // while `persist_settings` is on, and never to a headless or screenshot
    // launch, which must stay reproducible. A developer who wants device memory
    // without state memory sets this false. States over 8 MiB are not saved.
    bool persist_plugin_state = true;
    // How long the state must stay unchanged before it is saved, in
    // milliseconds. A knob being dragged keeps pushing the save back; one that
    // has been changing for ten seconds is saved regardless.
    int state_save_debounce_ms = 1500;
};

} // namespace pulp::format
