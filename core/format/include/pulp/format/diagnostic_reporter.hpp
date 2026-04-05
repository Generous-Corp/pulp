#pragma once

/// @file diagnostic_reporter.hpp
/// System and plugin diagnostic information for troubleshooting.

#include <pulp/format/processor.hpp>
#include <pulp/runtime/system.hpp>
#include <pulp/state/store.hpp>
#include <pulp/platform/platform.hpp>
#include <string>
#include <sstream>
#include <vector>
#include <iomanip>
#include <ctime>

namespace pulp::format {

/// Collects and formats diagnostic information about the system,
/// plugin configuration, and audio state for troubleshooting.
///
/// @code
/// DiagnosticReporter diag;
/// diag.set_plugin_info(host.descriptor(), host.state());
/// diag.set_audio_config(44100, 512, 2, 2);
/// std::string report = diag.generate_report();
/// // Copy to clipboard or save to file
/// @endcode
class DiagnosticReporter {
public:
    // ── Plugin info ──────────────────────────────────────────────────────

    void set_plugin_info(const PluginDescriptor& desc, const state::StateStore& store) {
        plugin_name_ = desc.name;
        plugin_manufacturer_ = desc.manufacturer;
        plugin_version_ = desc.version;
        plugin_bundle_id_ = desc.bundle_id;
        is_instrument_ = (desc.category == PluginCategory::Instrument);

        // Capture parameter state
        param_snapshot_.clear();
        for (const auto& p : store.all_params()) {
            ParamSnapshot snap;
            snap.name = p.name;
            snap.id = p.id;
            snap.value = store.get_value(p.id);
            snap.normalized = store.get_normalized(p.id);
            snap.default_value = p.range.default_value;
            snap.min = p.range.min;
            snap.max = p.range.max;
            snap.unit = p.unit;
            param_snapshot_.push_back(snap);
        }
    }

    // ── Audio config ─────────────────────────────────────────────────────

    void set_audio_config(double sample_rate, int buffer_size,
                           int input_channels, int output_channels) {
        sample_rate_ = sample_rate;
        buffer_size_ = buffer_size;
        input_channels_ = input_channels;
        output_channels_ = output_channels;
    }

    void set_format_name(const std::string& name) { format_name_ = name; }
    void set_host_name(const std::string& name) { host_name_ = name; }

    // ── CPU load ─────────────────────────────────────────────────────────

    void set_cpu_load(float load, float peak) {
        cpu_load_ = load;
        cpu_peak_ = peak;
    }

    // ── Custom entries ───────────────────────────────────────────────────

    void add_custom_entry(const std::string& key, const std::string& value) {
        custom_entries_.push_back({key, value});
    }

    // ── Report generation ────────────────────────────────────────────────

    /// Generate a full diagnostic report as a human-readable string.
    std::string generate_report() const {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2);

        ss << "=== Pulp Diagnostic Report ===\n";
        ss << "Generated: " << timestamp() << "\n\n";

        // System info
        ss << "--- System ---\n";
        ss << "Platform: " << os_name() << "\n";
        ss << "Architecture: " << arch() << "\n\n";

        // Plugin info
        ss << "--- Plugin ---\n";
        ss << "Name: " << plugin_name_ << "\n";
        ss << "Manufacturer: " << plugin_manufacturer_ << "\n";
        ss << "Version: " << plugin_version_ << "\n";
        ss << "Bundle ID: " << plugin_bundle_id_ << "\n";
        ss << "Type: " << (is_instrument_ ? "Instrument" : "Effect") << "\n";
        if (!format_name_.empty()) ss << "Format: " << format_name_ << "\n";
        if (!host_name_.empty()) ss << "Host: " << host_name_ << "\n";
        ss << "\n";

        // Audio config
        ss << "--- Audio Configuration ---\n";
        ss << "Sample Rate: " << sample_rate_ << " Hz\n";
        ss << "Buffer Size: " << buffer_size_ << " samples\n";
        ss << "Input Channels: " << input_channels_ << "\n";
        ss << "Output Channels: " << output_channels_ << "\n";
        if (cpu_load_ >= 0) {
            ss << "CPU Load: " << (cpu_load_ * 100.0f) << "%\n";
            ss << "CPU Peak: " << (cpu_peak_ * 100.0f) << "%\n";
        }
        ss << "\n";

        // Parameters
        ss << "--- Parameters (" << param_snapshot_.size() << ") ---\n";
        for (const auto& p : param_snapshot_) {
            ss << "  [" << p.id << "] " << p.name << ": "
               << p.value;
            if (!p.unit.empty()) ss << " " << p.unit;
            ss << " (norm=" << p.normalized
               << ", range=" << p.min << ".." << p.max
               << ", default=" << p.default_value << ")\n";
        }
        ss << "\n";

        // Custom entries
        if (!custom_entries_.empty()) {
            ss << "--- Custom ---\n";
            for (const auto& [key, value] : custom_entries_) {
                ss << key << ": " << value << "\n";
            }
            ss << "\n";
        }

        ss << "=== End Report ===\n";
        return ss.str();
    }

    /// Generate a compact JSON report for programmatic consumption.
    std::string generate_json() const {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(4);

        ss << "{\n";
        ss << "  \"timestamp\": \"" << timestamp() << "\",\n";
        ss << "  \"platform\": \"" << os_name() << "\",\n";
        ss << "  \"arch\": \"" << arch() << "\",\n";
        ss << "  \"plugin\": {\n";
        ss << "    \"name\": \"" << plugin_name_ << "\",\n";
        ss << "    \"manufacturer\": \"" << plugin_manufacturer_ << "\",\n";
        ss << "    \"version\": \"" << plugin_version_ << "\",\n";
        ss << "    \"type\": \"" << (is_instrument_ ? "instrument" : "effect") << "\"\n";
        ss << "  },\n";
        ss << "  \"audio\": {\n";
        ss << "    \"sampleRate\": " << sample_rate_ << ",\n";
        ss << "    \"bufferSize\": " << buffer_size_ << ",\n";
        ss << "    \"inputChannels\": " << input_channels_ << ",\n";
        ss << "    \"outputChannels\": " << output_channels_ << "\n";
        ss << "  },\n";
        ss << "  \"parameters\": [\n";
        for (size_t i = 0; i < param_snapshot_.size(); ++i) {
            const auto& p = param_snapshot_[i];
            ss << "    {\"id\": " << p.id
               << ", \"name\": \"" << p.name
               << "\", \"value\": " << p.value
               << ", \"normalized\": " << p.normalized << "}";
            if (i + 1 < param_snapshot_.size()) ss << ",";
            ss << "\n";
        }
        ss << "  ]\n";
        ss << "}\n";
        return ss.str();
    }

private:
    std::string plugin_name_;
    std::string plugin_manufacturer_;
    std::string plugin_version_;
    std::string plugin_bundle_id_;
    bool is_instrument_ = false;
    std::string format_name_;
    std::string host_name_;

    double sample_rate_ = 0;
    int buffer_size_ = 0;
    int input_channels_ = 0;
    int output_channels_ = 0;
    float cpu_load_ = -1;
    float cpu_peak_ = -1;

    struct ParamSnapshot {
        std::string name;
        state::ParamID id = 0;
        float value = 0;
        float normalized = 0;
        float default_value = 0;
        float min = 0, max = 0;
        std::string unit;
    };
    std::vector<ParamSnapshot> param_snapshot_;
    std::vector<std::pair<std::string, std::string>> custom_entries_;

    static std::string timestamp() {
        auto now = std::time(nullptr);
        char buf[64];
        const auto local = runtime::localtime_local(now);
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local);
        return buf;
    }

    static std::string os_name() {
        switch (platform::current_os) {
            case platform::OS::macOS:   return "macOS";
            case platform::OS::iOS:     return "iOS";
            case platform::OS::Windows: return "Windows";
            case platform::OS::Linux:   return "Linux";
            case platform::OS::Android: return "Android";
            default:                    return "Unknown";
        }
    }

    static std::string arch() {
#if defined(__aarch64__) || defined(_M_ARM64)
        return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
        return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
        return "x86";
#else
        return "unknown";
#endif
    }
};

} // namespace pulp::format
