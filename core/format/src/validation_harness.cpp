/// @file validation_harness.cpp
/// Implementation of the deterministic validation harness.

#include <pulp/format/validation_harness.hpp>
#include <pulp/platform/child_process.hpp>
#include <pulp/platform/platform.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <utility>

namespace pulp::format {

ValidationHarness::ValidationHarness(ProcessorFactory factory)
    : host_(std::move(factory))
{}

void ValidationHarness::configure(const ValidationRunOptions& opts) {
    opts_ = opts;
    if (!opts_.output_dir.empty()) {
        std::filesystem::create_directories(opts_.output_dir);
    }
}

// ── Audio control surface ───────────────────────────────────────────────────

void ValidationHarness::prepare() {
    host_.prepare(opts_.sample_rate, opts_.buffer_size,
                  opts_.input_channels, opts_.output_channels);
    prepared_ = true;
}

void ValidationHarness::set_param(state::ParamID id, float value) {
    host_.state().set_value(id, value);
}

float ValidationHarness::get_param(state::ParamID id) const {
    return host_.state().get_value(id);
}

void ValidationHarness::send_midi_note_on(int channel, int note, int velocity) {
    pending_midi_in_.add(midi::MidiEvent::note_on(
        static_cast<uint8_t>(channel),
        static_cast<uint8_t>(note),
        static_cast<uint8_t>(velocity)));
}

void ValidationHarness::send_midi_note_off(int channel, int note) {
    pending_midi_in_.add(midi::MidiEvent::note_off(
        static_cast<uint8_t>(channel),
        static_cast<uint8_t>(note)));
}

void ValidationHarness::send_midi_cc(int channel, int controller, int value) {
    pending_midi_in_.add(midi::MidiEvent::cc(
        static_cast<uint8_t>(channel),
        static_cast<uint8_t>(controller),
        static_cast<uint8_t>(value)));
}

std::vector<float> ValidationHarness::process_blocks(int num_blocks) {
    if (!prepared_) prepare();

    auto nch = static_cast<std::size_t>(opts_.output_channels);
    auto nsamp = static_cast<std::size_t>(opts_.buffer_size);

    audio::Buffer<float> in(nch, nsamp);
    audio::Buffer<float> out(nch, nsamp);

    // Zero the input
    for (std::size_t ch = 0; ch < nch; ++ch)
        for (std::size_t i = 0; i < nsamp; ++i)
            in.channel(ch)[i] = 0.0f;

    midi::MidiBuffer midi_out;

    for (int b = 0; b < num_blocks; ++b) {
        const float* in_ptrs[16] = {};
        for (std::size_t ch = 0; ch < nch && ch < 16; ++ch)
            in_ptrs[ch] = in.channel(ch).data();
        audio::BufferView<const float> in_view(in_ptrs, nch, nsamp);
        auto out_view = out.view();

        // Use pending MIDI on first block only
        if (b == 0 && !pending_midi_in_.empty()) {
            pending_midi_in_.sort();
            host_.process(out_view, in_view, pending_midi_in_, midi_out);
            pending_midi_in_.clear();
        } else {
            host_.process(out_view, in_view);
        }
        midi_out.clear();
    }

    // Return final block interleaved
    std::vector<float> result(nch * nsamp);
    for (std::size_t ch = 0; ch < nch; ++ch)
        for (std::size_t i = 0; i < nsamp; ++i)
            result[i * nch + ch] = out.channel(ch)[i];

    return result;
}

std::vector<float> ValidationHarness::process_buffer(
    const std::vector<float>& interleaved_input,
    int num_channels, int num_samples)
{
    if (!prepared_) prepare();

    auto nch = static_cast<std::size_t>(num_channels);
    auto nsamp = static_cast<std::size_t>(num_samples);

    audio::Buffer<float> in(nch, nsamp);
    audio::Buffer<float> out(nch, nsamp);

    // De-interleave input
    for (std::size_t ch = 0; ch < nch; ++ch)
        for (std::size_t i = 0; i < nsamp; ++i)
            in.channel(ch)[i] = interleaved_input[i * nch + ch];

    const float* in_ptrs[16] = {};
    for (std::size_t ch = 0; ch < nch && ch < 16; ++ch)
        in_ptrs[ch] = in.channel(ch).data();
    audio::BufferView<const float> in_view(in_ptrs, nch, nsamp);
    auto out_view = out.view();

    midi::MidiBuffer midi_out;
    if (!pending_midi_in_.empty()) {
        pending_midi_in_.sort();
        host_.process(out_view, in_view, pending_midi_in_, midi_out);
        pending_midi_in_.clear();
    } else {
        host_.process(out_view, in_view);
    }

    // Interleave output
    std::vector<float> result(nch * nsamp);
    for (std::size_t ch = 0; ch < nch; ++ch)
        for (std::size_t i = 0; i < nsamp; ++i)
            result[i * nch + ch] = out.channel(ch)[i];

    return result;
}

std::vector<uint8_t> ValidationHarness::save_state() const {
    return host_.save_state();
}

bool ValidationHarness::load_state(std::span<const uint8_t> data) {
    return host_.load_state(data);
}

// ── Capture ─────────────────────────────────────────────────────────────────

void ValidationHarness::set_capture_screenshot_provider(
    CaptureScreenshotProvider p)
{
    screenshot_provider_ = std::move(p);
}

void ValidationHarness::set_capture_inspector_provider(
    CaptureInspectorProvider p)
{
    inspector_provider_ = std::move(p);
}

ReportEntry ValidationHarness::capture_screenshot(
    const std::filesystem::path& output_path)
{
    // #298: delegate to the registered provider. Without one, the
    // entry is explicitly `skip` so callers cannot confuse the
    // headless state with a successful validation.
    ReportEntry entry;
    entry.type = "screenshot";
    entry.target = descriptor().name;

    std::string view_id = "none";
    if (screenshot_provider_) {
        const bool ok = screenshot_provider_(
            output_path,
            opts_.screenshot_width,
            opts_.screenshot_height,
            opts_.screenshot_scale,
            opts_.screenshot_backend);
        if (ok) {
            entry.status = ValidationStatus::pass;
            view_id = "provider";
        } else {
            entry.status = ValidationStatus::fail;
            entry.error_message =
                "capture_screenshot_provider returned false";
        }
    } else {
        entry.status = ValidationStatus::skip;
        entry.error_message =
            "No capture-screenshot provider attached";
    }

    std::ostringstream payload;
    payload << "{"
            << "\"path\": \"" << escape_json(output_path.string()) << "\","
            << "\"width\": " << opts_.screenshot_width << ","
            << "\"height\": " << opts_.screenshot_height << ","
            << "\"scale\": " << opts_.screenshot_scale << ","
            << "\"backend\": \"" << opts_.screenshot_backend << "\","
            << "\"view_id\": \"" << view_id << "\""
            << "}";
    entry.payload_json = payload.str();

    entries_.push_back(entry);
    return entry;
}

ReportEntry ValidationHarness::compare_screenshots(
    const std::filesystem::path& reference,
    const std::filesystem::path& rendered)
{
    ReportEntry entry;
    entry.type = "screenshot_diff";
    entry.target = descriptor().name;

    // File-existence checks first so missing inputs are always an
    // error (never skip).
    if (!std::filesystem::exists(reference)) {
        entry.status = ValidationStatus::error;
        entry.error_message = "Reference file not found: " + reference.string();
        entries_.push_back(entry);
        return entry;
    }
    if (!std::filesystem::exists(rendered)) {
        entry.status = ValidationStatus::error;
        entry.error_message = "Rendered file not found: " + rendered.string();
        entries_.push_back(entry);
        return entry;
    }

    // Byte-level file comparison. Good enough to detect bit-identical
    // regression-rendered output (the common case in deterministic
    // golden-file tests); pixel-level tolerant diff ships when a PNG
    // codec becomes available to the format layer (#298 follow-up).
    // Produces pass/fail — never skip — so callers can't confuse
    // "images differ" with "comparison not available."
    // #308 Codex P1: std::filesystem::file_size throws on non-regular
    // files (e.g. a directory accidentally passed as an argument) and
    // on some I/O failures. Use the non-throwing overload + is_regular_file
    // so the harness can always return a report entry with
    // ValidationStatus::error instead of propagating an exception that
    // aborts the whole validation run.
    std::error_code ec;
    if (!std::filesystem::is_regular_file(reference, ec) || ec) {
        entry.status = ValidationStatus::error;
        entry.error_message =
            "Reference path is not a regular file: " + reference.string();
        entries_.push_back(entry);
        return entry;
    }
    ec.clear();
    if (!std::filesystem::is_regular_file(rendered, ec) || ec) {
        entry.status = ValidationStatus::error;
        entry.error_message =
            "Rendered path is not a regular file: " + rendered.string();
        entries_.push_back(entry);
        return entry;
    }
    ec.clear();
    const std::uintmax_t ref_size = std::filesystem::file_size(reference, ec);
    if (ec) {
        entry.status = ValidationStatus::error;
        entry.error_message =
            "Failed to stat reference: " + ec.message();
        entries_.push_back(entry);
        return entry;
    }
    ec.clear();
    const std::uintmax_t ren_size = std::filesystem::file_size(rendered, ec);
    if (ec) {
        entry.status = ValidationStatus::error;
        entry.error_message =
            "Failed to stat rendered: " + ec.message();
        entries_.push_back(entry);
        return entry;
    }

    double similarity = 0.0;
    bool identical = false;
    if (ref_size == ren_size) {
        std::ifstream a(reference, std::ios::binary);
        std::ifstream b(rendered, std::ios::binary);
        identical = a.good() && b.good()
            && std::equal(std::istreambuf_iterator<char>(a), {},
                          std::istreambuf_iterator<char>(b));
        similarity = identical ? 1.0 : 0.0;
    }

    entry.status = identical
        ? ValidationStatus::pass
        : ValidationStatus::fail;
    if (!identical) {
        entry.error_message = "Files differ (byte-level comparison)";
    }

    std::ostringstream payload;
    payload << "{"
            << "\"reference_path\": \"" << escape_json(reference.string()) << "\","
            << "\"rendered_path\": \"" << escape_json(rendered.string()) << "\","
            << "\"similarity\": " << similarity << ","
            << "\"total_pixels\": 0,"
            << "\"diff_pixels\": 0,"
            << "\"tolerance\": " << static_cast<int>(opts_.diff_tolerance) << ","
            << "\"threshold\": " << opts_.diff_threshold << ","
            << "\"comparison\": \"byte-level\""
            << "}";
    entry.payload_json = payload.str();

    entries_.push_back(entry);
    return entry;
}

ReportEntry ValidationHarness::capture_inspector() {
    ReportEntry entry;
    entry.type = "inspector";
    entry.target = descriptor().name;

    if (inspector_provider_) {
        std::string tree_json = inspector_provider_();
        if (tree_json.empty()) {
            entry.status = ValidationStatus::fail;
            entry.error_message =
                "capture_inspector_provider returned empty tree";
            entry.payload_json = "{\"view_tree\": {}, \"view_count\": 0}";
        } else {
            entry.status = ValidationStatus::pass;
            std::ostringstream p;
            p << "{\"view_tree\": " << tree_json << "}";
            entry.payload_json = p.str();
        }
    } else {
        entry.status = ValidationStatus::skip;
        entry.error_message =
            "No capture-inspector provider attached";
        entry.payload_json = "{\"view_tree\": {}, \"view_count\": 0}";
    }

    entries_.push_back(entry);
    return entry;
}

// ── External validators ─────────────────────────────────────────────────────

ReportEntry ValidationHarness::run_validator(
    const std::string& tool,
    const std::filesystem::path& plugin_path,
    const std::string& format)
{
    auto start = std::chrono::steady_clock::now();

    ReportEntry entry;
    entry.type = "validator";
    entry.target = plugin_path.filename().string();

    // Check plugin exists
    if (!std::filesystem::exists(plugin_path)) {
        entry.status = ValidationStatus::error;
        entry.error_message = "Plugin not found: " + plugin_path.string();
        entries_.push_back(entry);
        return entry;
    }

    // Check if the tool is available
    auto tool_found = pulp::platform::find_on_path(tool);
    std::string tool_path = tool_found ? tool_found->string() : std::string{};

    if (tool_path.empty()) {
        entry.status = ValidationStatus::skip;
        entry.error_message = tool + " not found in PATH";

        std::ostringstream payload;
        payload << "{"
                << "\"tool\": \"" << tool << "\","
                << "\"plugin_path\": \"" << escape_json(plugin_path.string()) << "\""
                << "}";
        entry.payload_json = payload.str();

        entries_.push_back(entry);
        return entry;
    }

    // Build validation command. Validators can probe a plug-in's editor; keep
    // automation in no-editor mode so validation never opens a native window.
    std::string cmd;
    std::string detected_format = format;
    auto disable_plugin_editor = [](std::string inner) {
#ifdef _WIN32
        return std::string("set \"PULP_DISABLE_PLUGIN_EDITOR=1\" && "
                           "set \"PULP_HEADLESS=1\" && "
                           "set \"PULP_TEST_MODE=1\" && ") + inner;
#else
        return std::string("PULP_DISABLE_PLUGIN_EDITOR=1 "
                           "PULP_HEADLESS=1 "
                           "PULP_TEST_MODE=1 ") + inner;
#endif
    };

    if (tool == "pluginval") {
        if (detected_format.empty()) detected_format = "vst3";
        cmd = disable_plugin_editor(tool + " --strictness-level 5 --timeout-ms 30000 --validate \""
                                    + plugin_path.string() + "\" 2>&1");
    } else if (tool == "clap-validator") {
        if (detected_format.empty()) detected_format = "clap";
        cmd = disable_plugin_editor(tool + " validate \"" + plugin_path.string() + "\" 2>&1");
    } else if (tool == "auval") {
        // auval requires component type/subtype/manufacturer
        if (detected_format.empty()) detected_format = "au";
        cmd = disable_plugin_editor(tool + " -v aufx pulp Pulp 2>&1");  // generic; caller should customize
    } else if (tool == "vstvalidator") {
        if (detected_format.empty()) detected_format = "vst3";
        cmd = disable_plugin_editor(tool + " \"" + plugin_path.string() + "\" 2>&1");
    } else {
        entry.status = ValidationStatus::error;
        entry.error_message = "Unknown validator tool: " + tool;
        entries_.push_back(entry);
        return entry;
    }

    // Run the validator via ChildProcess (with 30s timeout)
#ifdef _WIN32
    auto result = pulp::platform::exec("cmd", {"/c", cmd}, 30000);
#else
    auto result = pulp::platform::exec("/bin/sh", {"-c", cmd}, 30000);
#endif
    {
        auto end = std::chrono::steady_clock::now();
        entry.duration_ms = std::chrono::duration<double, std::milli>(end - start).count();
        int exit_code = result.exit_code;
        std::string output = result.stdout_output + result.stderr_output;

        entry.status = (exit_code == 0) ? ValidationStatus::pass : ValidationStatus::fail;
        if (result.timed_out) {
            entry.status = ValidationStatus::fail;
            entry.error_message = tool + " timed out after 30 seconds";
        } else if (exit_code != 0) {
            entry.error_message = tool + " exited with code " + std::to_string(exit_code);
        }

        // Get tool version
        auto ver_result = pulp::platform::exec(tool, {"--version"}, 5000);
        std::string version = ver_result.stdout_output;
        while (!version.empty() && (version.back() == '\n' || version.back() == '\r'))
            version.pop_back();

        std::ostringstream payload;
        payload << "{"
                << "\"tool\": \"" << tool << "\","
                << "\"tool_version\": \"" << escape_json(version) << "\","
                << "\"plugin_path\": \"" << escape_json(plugin_path.string()) << "\","
                << "\"plugin_format\": \"" << detected_format << "\","
                << "\"exit_code\": " << exit_code << ","
                << "\"stdout\": \"" << escape_json(output) << "\","
                << "\"stderr\": \"\""
                << "}";
        entry.payload_json = payload.str();
    }

    entries_.push_back(entry);
    return entry;
}

// ── Report generation ───────────────────────────────────────────────────────

void ValidationHarness::add_entry(ReportEntry entry) {
    entries_.push_back(std::move(entry));
}

std::string ValidationHarness::generate_report() const {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);

    ss << "{\n";
    ss << "  \"version\": 1,\n";
    ss << "  \"timestamp\": \"" << iso_timestamp() << "\"";

    if (!opts_.run_id.empty())
        ss << ",\n  \"run_id\": \"" << escape_json(opts_.run_id) << "\"";
    if (!opts_.git_ref.empty())
        ss << ",\n  \"git_ref\": \"" << escape_json(opts_.git_ref) << "\"";

    ss << ",\n  \"platform\": \"" << platform_string() << "\"";

    ss << ",\n  \"reports\": [\n";

    for (size_t i = 0; i < entries_.size(); ++i) {
        const auto& e = entries_[i];
        ss << "    {\n";
        ss << "      \"type\": \"" << e.type << "\",\n";
        ss << "      \"status\": \"" << to_string(e.status) << "\"";

        if (!e.target.empty())
            ss << ",\n      \"target\": \"" << escape_json(e.target) << "\"";
        if (e.duration_ms > 0)
            ss << ",\n      \"duration_ms\": " << e.duration_ms;
        if (!e.error_message.empty())
            ss << ",\n      \"error_message\": \"" << escape_json(e.error_message) << "\"";

        // Type-specific payload
        if (!e.payload_json.empty()) {
            std::string payload_key;
            if (e.type == "screenshot")       payload_key = "screenshot";
            else if (e.type == "screenshot_diff") payload_key = "diff";
            else if (e.type == "inspector")   payload_key = "inspector";
            else if (e.type == "validator")   payload_key = "validator";
            else if (e.type == "sanitizer")   payload_key = "sanitizer";
            else if (e.type == "test_suite")  payload_key = "test_suite";

            if (!payload_key.empty())
                ss << ",\n      \"" << payload_key << "\": " << e.payload_json;
        }

        ss << "\n    }";
        if (i + 1 < entries_.size()) ss << ",";
        ss << "\n";
    }

    ss << "  ]\n";
    ss << "}\n";

    return ss.str();
}

bool ValidationHarness::write_report(const std::filesystem::path& output_path) const {
    if (output_path.has_parent_path())
        std::filesystem::create_directories(output_path.parent_path());

    std::ofstream f(output_path);
    if (!f.good()) return false;
    f << generate_report();
    return f.good();
}

// ── Private helpers ─────────────────────────────────────────────────────────

std::string ValidationHarness::platform_string() const {
#if defined(__APPLE__)
#  if defined(__aarch64__)
    return "macos-arm64";
#  else
    return "macos-x86_64";
#  endif
#elif defined(_WIN32)
    return "windows-x64";
#elif defined(__linux__)
    return "linux-x64";
#else
    return "unknown";
#endif
}

std::string ValidationHarness::iso_timestamp() const {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    char buf[64];
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &time_t);
#else
    gmtime_r(&time_t, &tm_buf);
#endif
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return buf;
}

std::string ValidationHarness::escape_json(const std::string& s) const {
    std::string result;
    result.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char hex[8];
                    snprintf(hex, sizeof(hex), "\\u%04x", c);
                    result += hex;
                } else {
                    result += c;
                }
        }
    }
    return result;
}

} // namespace pulp::format
