#include <pulp/format/descriptor_validation.hpp>

namespace pulp::format {

namespace {

bool looks_like_reverse_dns(const std::string& s) {
    // Minimal heuristic: at least two '.' separated non-empty segments.
    // Full RFC 1035 validation isn't the goal — we just want to catch
    // "MyPlugin" vs "com.example.myplugin".
    int dots = 0;
    bool seen_non_empty = false;
    bool segment_has_chars = false;
    for (char c : s) {
        if (c == '.') {
            if (!segment_has_chars) return false;
            ++dots;
            segment_has_chars = false;
        } else if (c > ' ') {
            segment_has_chars = true;
            seen_non_empty = true;
        }
    }
    return seen_non_empty && segment_has_chars && dots >= 2;
}

} // namespace

std::vector<DescriptorIssue> validate_descriptor(const PluginDescriptor& d) {
    std::vector<DescriptorIssue> issues;

    if (d.name.empty()) {
        issues.push_back({DescriptorIssueSeverity::Error,
                          "name", "name is empty"});
    }
    if (d.manufacturer.empty()) {
        issues.push_back({DescriptorIssueSeverity::Error,
                          "manufacturer", "manufacturer is empty"});
    }
    if (d.bundle_id.empty()) {
        issues.push_back({DescriptorIssueSeverity::Error,
                          "bundle_id", "bundle_id is empty"});
    } else if (!looks_like_reverse_dns(d.bundle_id)) {
        issues.push_back({DescriptorIssueSeverity::Warning,
                          "bundle_id",
                          "bundle_id '" + d.bundle_id +
                          "' does not look like reverse-DNS "
                          "(e.g. com.example.myplugin)"});
    }

    // Audio-producing categories must declare a non-empty main output.
    // MidiEffect is MIDI-only by contract (receives MIDI, produces MIDI)
    // and may legitimately skip audio output entirely.
    if (d.category != PluginCategory::MidiEffect) {
        if (d.output_buses.empty() ||
            d.output_buses[0].default_channels == 0) {
            issues.push_back({DescriptorIssueSeverity::Error,
                              "output_buses",
                              "plugin must declare at least one audio "
                              "output bus with > 0 channels"});
        }
    }

    if (d.category == PluginCategory::Instrument &&
        !d.input_buses.empty() &&
        d.input_buses[0].default_channels > 0 &&
        !d.input_buses[0].optional) {
        issues.push_back({DescriptorIssueSeverity::Warning,
                          "input_buses",
                          "instruments usually declare 0-channel or "
                          "optional main input — non-optional audio "
                          "input will confuse some hosts"});
    }

    if (d.category == PluginCategory::MidiEffect &&
        !d.accepts_midi) {
        issues.push_back({DescriptorIssueSeverity::Warning,
                          "accepts_midi",
                          "MidiEffect category without accepts_midi=true "
                          "— the plugin will receive no MIDI input"});
    }

    if ((d.supports_mpe || d.supports_ump) && !d.accepts_midi) {
        issues.push_back({DescriptorIssueSeverity::Warning,
                          "accepts_midi",
                          "supports_mpe / supports_ump are set but "
                          "accepts_midi is false — MIDI will be filtered "
                          "out before reaching the MPE / UMP sidecars"});
    }

    return issues;
}

bool descriptor_is_valid(const std::vector<DescriptorIssue>& issues) {
    for (const auto& i : issues) {
        if (i.severity == DescriptorIssueSeverity::Error) return false;
    }
    return true;
}

}  // namespace pulp::format
