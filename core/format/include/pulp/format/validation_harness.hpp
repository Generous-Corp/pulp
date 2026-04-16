#pragma once

/// @file validation_harness.hpp
/// Deterministic validation harness that bundles HeadlessHost, screenshot
/// capture, view inspection, and report generation into one stable API.
///
/// Produces validation reports conforming to validation-report-v1.schema.json.

#include <pulp/format/headless.hpp>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace pulp::format {

/// Result status for individual validation steps.
enum class ValidationStatus { pass, fail, skip, error };

/// Convert status to its JSON string representation.
inline const char* to_string(ValidationStatus s) {
    switch (s) {
        case ValidationStatus::pass:  return "pass";
        case ValidationStatus::fail:  return "fail";
        case ValidationStatus::skip:  return "skip";
        case ValidationStatus::error: return "error";
    }
    return "error";
}

/// One entry in the validation report (matches report schema).
struct ReportEntry {
    std::string type;               // screenshot, screenshot_diff, inspector, validator, sanitizer, test_suite
    ValidationStatus status = ValidationStatus::pass;
    std::string target;             // plugin name, test name, view ID
    double duration_ms = 0;
    std::string error_message;

    // Type-specific payloads stored as pre-formatted JSON fragments.
    // The harness populates these when generating entries.
    std::string payload_json;
};

/// Options for a validation run.
struct ValidationRunOptions {
    std::filesystem::path output_dir;       // Where to write artifacts
    double sample_rate = 48000.0;
    int buffer_size = 512;
    int input_channels = 2;
    int output_channels = 2;
    uint32_t screenshot_width = 800;
    uint32_t screenshot_height = 600;
    float screenshot_scale = 2.0f;
    std::string screenshot_backend = "default";

    // Diff settings
    uint8_t diff_tolerance = 32;
    float diff_threshold = 0.85f;

    // Git metadata (optional)
    std::string git_ref;
    std::string run_id;
};

/// Deterministic validation harness.
///
/// Wraps HeadlessHost with screenshot, inspector, and report generation
/// to provide a single stable control surface for validation workflows.
///
/// @code
/// ValidationHarness harness(MyPlugin::create);
/// harness.prepare();
/// harness.set_param(kGain, -6.0f);
/// harness.process_blocks(10);
/// harness.capture_screenshot("/tmp/shot.png");
/// auto report = harness.generate_report();
/// @endcode
class ValidationHarness {
public:
    /// Construct from a processor factory.
    explicit ValidationHarness(ProcessorFactory factory);

    /// Configure the harness with run options.
    void configure(const ValidationRunOptions& opts);

    // ── Audio control surface ───────────────────────────────────────────

    /// Prepare the processor for audio. Uses options from configure().
    void prepare();

    /// Set a parameter value by ID.
    void set_param(state::ParamID id, float value);

    /// Get a parameter value by ID.
    float get_param(state::ParamID id) const;

    /// Send MIDI note-on into the next process() call.
    void send_midi_note_on(int channel, int note, int velocity);

    /// Send MIDI note-off into the next process() call.
    void send_midi_note_off(int channel, int note);

    /// Send MIDI CC into the next process() call.
    void send_midi_cc(int channel, int controller, int value);

    /// Process N blocks of silence, returning the final output buffer contents.
    /// Useful for letting tail ring out or stabilising state.
    std::vector<float> process_blocks(int num_blocks);

    /// Process a single block of audio from the given input buffer.
    /// Input is interleaved: [ch0_s0, ch1_s0, ch0_s1, ch1_s1, ...].
    /// Returns interleaved output.
    std::vector<float> process_buffer(const std::vector<float>& interleaved_input,
                                       int num_channels, int num_samples);

    /// Save current state to binary blob.
    std::vector<uint8_t> save_state() const;

    /// Load state from binary blob.
    bool load_state(std::span<const uint8_t> data);

    // ── Capture ─────────────────────────────────────────────────────────
    //
    // Screenshot and inspector capture are delegated to an external
    // provider callback so the harness stays free of a direct view-
    // library dependency (#298). Register a provider before calling
    // capture_screenshot()/capture_inspector() — without one, the
    // entry is reported as `skip` with a clear "no provider attached"
    // message. Callers must never confuse skip with pass.

    /// Callback that captures a screenshot of the current view/render
    /// state into `output_path`. Returns true on success.
    using CaptureScreenshotProvider = std::function<bool(
        const std::filesystem::path& output_path,
        uint32_t width, uint32_t height, float scale,
        const std::string& backend)>;

    /// Callback that produces a JSON snapshot of the view tree.
    /// Returns the JSON string; empty string means "no tree available".
    using CaptureInspectorProvider = std::function<std::string()>;

    /// Install the screenshot capture provider. Pass an empty
    /// std::function to remove.
    void set_capture_screenshot_provider(CaptureScreenshotProvider p);

    /// Install the inspector capture provider.
    void set_capture_inspector_provider(CaptureInspectorProvider p);

    /// Capture a screenshot to the given path. Returns a ReportEntry.
    /// Requires a capture-screenshot provider installed via
    /// set_capture_screenshot_provider(); otherwise reports `skip`.
    ReportEntry capture_screenshot(const std::filesystem::path& output_path);

    /// Compare two screenshot files and generate a diff entry.
    /// Today this does a byte-level file comparison — pixel-level
    /// diffing (with a tolerance) is a follow-up once a PNG codec
    /// is available to the format layer. A real pixel diff always
    /// produces pass/fail, never skip.
    ReportEntry compare_screenshots(const std::filesystem::path& reference,
                                     const std::filesystem::path& rendered);

    /// Capture the view inspector tree as JSON. Returns a ReportEntry.
    /// Requires a capture-inspector provider installed via
    /// set_capture_inspector_provider(); otherwise reports `skip`.
    ReportEntry capture_inspector();

    // ── External validators ─────────────────────────────────────────────

    /// Run an external validator tool on a plugin bundle.
    /// Supported tools: "pluginval", "clap-validator", "auval", "vstvalidator".
    /// Returns skip status if the tool is not installed.
    ReportEntry run_validator(const std::string& tool,
                               const std::filesystem::path& plugin_path,
                               const std::string& format = "");

    // ── Report generation ───────────────────────────────────────────────

    /// Add a report entry to the accumulator.
    void add_entry(ReportEntry entry);

    /// Generate the full validation report as JSON (conforming to schema v1).
    std::string generate_report() const;

    /// Write the report JSON to a file.
    bool write_report(const std::filesystem::path& output_path) const;

    /// Get all accumulated report entries.
    const std::vector<ReportEntry>& entries() const { return entries_; }

    /// Clear all accumulated entries.
    void clear_entries() { entries_.clear(); }

    /// Access the underlying HeadlessHost for advanced use.
    HeadlessHost& host() { return host_; }
    const HeadlessHost& host() const { return host_; }

    /// Access the plugin descriptor.
    const PluginDescriptor& descriptor() const { return host_.descriptor(); }

private:
    HeadlessHost host_;
    ValidationRunOptions opts_;
    std::vector<ReportEntry> entries_;
    midi::MidiBuffer pending_midi_in_;
    bool prepared_ = false;

    // #298 capture delegation. Unset by default; callers register a
    // provider backed by the view layer (or a test fake) before
    // invoking capture_screenshot / capture_inspector.
    CaptureScreenshotProvider screenshot_provider_;
    CaptureInspectorProvider  inspector_provider_;

    std::string platform_string() const;
    std::string iso_timestamp() const;
    std::string escape_json(const std::string& s) const;
};

} // namespace pulp::format
