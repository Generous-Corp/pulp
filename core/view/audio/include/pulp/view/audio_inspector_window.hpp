#pragma once

/// @file audio_inspector_window.hpp
/// Separate developer Audio Inspector tool window — live signal observability
/// (meters, probe-stage status, copied waveform, L/R level-match/balance,
/// device summary) built on the realtime output-boundary probe.
///
/// This is a SIBLING of `InspectorWindow` (the layout inspector), not a tab
/// inside it. The two tools share window / command / visual primitives but own
/// independent state models. Both dispatch through a shell-owned `CommandRegistry`; this
/// window never assigns `View::on_global_key` directly.
///
/// Mirrors `InspectorWindow`'s structure exactly:
///   * privately implements `CommandHandler`;
///   * a stable toggle `CommandID` (`kToggleAudioInspector`, ASCII 'PLAI');
///   * `register_command_handler(CommandRegistry&)` with RAII removal in the
///     destructor;
///   * `secondary_window = true`, reusing `WindowType::inspector`.
///
/// Data path: `poll()` reads `AudioProbe::latest()` exactly once per UI tick
/// into a copied snapshot and pushes it to the panel. No FFT, allocation,
/// logging, or file I/O runs on the realtime audio thread — the probe already
/// guarantees that, and this window only consumes the published summary.

#include <pulp/audio/audio_probe.hpp>
#include <pulp/audio/audio_scope.hpp>
#include <pulp/audio/audio_stats.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/state/properties_file.hpp>
#include <pulp/view/audio_inspector_panel.hpp>
#include <pulp/view/command_registry.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/view/window_manager.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace pulp::view {

/// Floating Audio Inspector window. Construct, point it at an `AudioProbe`
/// (optional — without one it shows the honest "no probe" state), register its
/// toggle command, and call `poll()` from the UI timer to refresh.
class AudioInspectorWindow : private CommandHandler {
public:
    using HostFactory = std::function<std::unique_ptr<WindowHost>(
        View& root, const WindowOptions& options)>;

    /// Command id for "Toggle Audio Inspector". Stable so apps can reference it
    /// from menus or a `KeyMappingEditor`. ASCII 'PLAI' — distinct from the
    /// layout inspector's 'PLPI' so the two tools never collide.
    static constexpr CommandID kToggleAudioInspector = 0x504C4149;

    /// @param mgr          Optional WindowManager for registration.
    /// @param parent       Optional parent WindowHost (positions relative to it).
    /// @param host_factory Optional host creation override for tests / embedders.
    explicit AudioInspectorWindow(WindowManager* mgr = nullptr,
                                  WindowHost* parent = nullptr,
                                  HostFactory host_factory = {},
                                  std::string app_name = "Pulp");
    ~AudioInspectorWindow();

    AudioInspectorWindow(const AudioInspectorWindow&) = delete;
    AudioInspectorWindow& operator=(const AudioInspectorWindow&) = delete;

    /// Point the window at a probe to observe. May be null (clears the source
    /// and the next `poll()` shows the "no probe" state). The probe must
    /// outlive this window, or be cleared first. Resets stale tracking so the
    /// first `poll()` after a swap is judged fresh.
    void set_probe(audio::AudioProbe* probe);
    audio::AudioProbe* probe() const { return probe_; }

    /// Mirror device/backend counters the probe must NOT shadow (xruns,
    /// overloads). The host owns these (see audio_stats.hpp); the window just
    /// displays them alongside the probe-owned signal counters.
    void set_device_stats(const audio::AudioStats& device_stats);

    /// Poll the probe once: read the latest snapshot, detect stale (sequence
    /// not advancing), copy any captured waveform, and refresh the panel.
    /// Safe to call when hidden or when no probe is set. Reads
    /// `AudioProbe::latest()` exactly once per call (single-consumer
    /// TripleBuffer contract).
    void poll();

    void show();
    void hide();
    void toggle();
    bool is_visible() const;

    /// Register the toggle command (`kToggleAudioInspector`, default chord
    /// Cmd+Shift+A / Ctrl+Shift+A) with a caller-owned `CommandRegistry`.
    /// RAII: the handler is removed in the destructor, so dispatch after this
    /// window is destroyed finds no handler and returns false. The registry
    /// must outlive this window. Re-registering moves the handler to the new
    /// registry. Does NOT touch `on_global_key` — the shell routes the root
    /// key path once with `route_global_keys(root, registry)`.
    void register_command_handler(CommandRegistry& registry);

    /// Panel accessor — for headless state assertions.
    const AudioInspectorPanel& panel() const { return *panel_; }

    void set_mode(AudioInspectorMode mode);
    AudioInspectorMode mode() const { return mode_; }

    /// Optional test/embedding hook. Production windows use
    /// ApplicationProperties, but tests can inject an in-memory PropertiesFile
    /// to assert persistence without touching user preference paths.
    void set_preferences(state::PropertiesFile* preferences);

    /// The window's own `WindowHost`, or null until `show()` has created one
    /// (and after a host_factory that returned null). Exposed so a headless
    /// host can `capture_png()` the inspector's surface for a screenshot proof,
    /// the same way the standalone host captures its main window. Ownership
    /// stays with this window — the caller must not retain the pointer past the
    /// window's lifetime.
    WindowHost* window_host() const { return window_host_.get(); }

private:
    // CommandHandler (private base) — registry dispatch entry points.
    std::vector<CommandID> commands() const override;
    bool perform_command(CommandID id) override;

    void build_ui();
    void load_mode_preference();
    void save_mode_preference();
    void apply_mode(AudioInspectorMode mode, bool persist);
    audio::AudioScopeResult build_scope_result(
        const audio::AudioProbeSnapshot& snap,
        audio::Buffer<float>& capture,
        int frames) const;

    WindowManager* manager_ = nullptr;
    WindowHost* parent_host_ = nullptr;
    HostFactory host_factory_;
    CommandRegistry* registry_ = nullptr;
    std::string app_name_;
    std::unique_ptr<state::ApplicationProperties> app_properties_;
    state::PropertiesFile* preferences_ = nullptr;
    AudioInspectorMode mode_ = AudioInspectorMode::kSignal;
    bool applying_mode_ = false;

    audio::AudioProbe* probe_ = nullptr;
    audio::AudioStats device_stats_{};

    // Stale detection: the last sequence number we saw advance, and whether a
    // probe has produced at least one snapshot since it was wired.
    std::uint64_t last_sequence_ = 0;
    bool ever_observed_ = false;

    // Copied waveform scratch (non-RT; sized to the panel's display capacity).
    std::vector<float> waveform_scratch_;
    audio::Buffer<float> scope_capture_;

    std::unique_ptr<View> root_;
    AudioInspectorPanel* panel_ = nullptr;

    std::unique_ptr<WindowHost> window_host_;
    WindowId window_id_ = 0;
};

}  // namespace pulp::view
