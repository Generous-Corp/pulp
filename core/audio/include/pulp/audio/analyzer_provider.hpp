#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <pulp/audio/buffer.hpp>

namespace pulp::audio {

enum class AnalyzerCapability : std::uint8_t {
    OnsetDetection,
    BeatDetection,
    SliceAnalysis,
    LoopPointAnalysis,
    KeyDetection,
    TempoDetection,
    TransientClassification,
    TimeStretch,
    PitchShift,
};

enum class AnalyzerBackend : std::uint8_t {
    BuiltIn,
    Package,
    ExternalProcess,
    ImportedMetadata,
};

enum class AnalyzerAvailability : std::uint8_t {
    Available,
    MissingPackage,
    LicenseNotAccepted,
    UnsupportedPlatform,
    Disabled,
    Unavailable,
};

enum class AnalyzerLicensePolicy : std::uint8_t {
    Permissive,
    NoticeRequired,
    Copyleft,
    Commercial,
    Unknown,
};

enum class AnalyzerExecutionContext : std::uint8_t {
    OfflineOnly,
    BackgroundThread,
    RealtimeSafe,
};

enum class AnalyzerDescriptorStatus : std::uint8_t {
    ok,
    empty_id,
    empty_display_name,
    missing_capability,
    package_id_required,
    duplicate_id,
};

[[nodiscard]] const char* analyzer_capability_name(AnalyzerCapability capability) noexcept;
[[nodiscard]] const char* analyzer_backend_name(AnalyzerBackend backend) noexcept;
[[nodiscard]] const char* analyzer_availability_name(AnalyzerAvailability availability) noexcept;
[[nodiscard]] const char* analyzer_license_policy_name(AnalyzerLicensePolicy policy) noexcept;
[[nodiscard]] const char* analyzer_execution_context_name(AnalyzerExecutionContext context) noexcept;
[[nodiscard]] const char* analyzer_descriptor_status_name(AnalyzerDescriptorStatus status) noexcept;

struct AnalyzerDescriptor {
    std::string id;
    std::string display_name;
    std::string package_id;
    std::string version;
    std::string license_id;
    std::string url;
    AnalyzerBackend backend = AnalyzerBackend::BuiltIn;
    AnalyzerAvailability availability = AnalyzerAvailability::Available;
    AnalyzerLicensePolicy license_policy = AnalyzerLicensePolicy::Permissive;
    std::vector<AnalyzerCapability> capabilities;
    AnalyzerExecutionContext execution_context = AnalyzerExecutionContext::BackgroundThread;
    bool supports_streaming_input = false;
    bool supports_offline_buffers = true;
};

struct AnalyzerSelectionPolicy {
    bool include_unavailable = false;
    bool include_copyleft = false;
    bool include_commercial = false;
    bool include_unknown_license = false;
    bool require_real_time_safe = false;
    bool require_offline_buffers = false;
    bool allow_deferred_execution = true;
};

struct PackageAnalyzerDescriptorInput {
    std::string provider_id;
    std::string package_id;
    std::string display_name;
    std::string version;
    std::string license_id;
    std::string url;
    std::vector<AnalyzerCapability> capabilities;
    bool installed = false;
    bool platform_supported = true;
    bool license_accepted = true;
    AnalyzerExecutionContext execution_context = AnalyzerExecutionContext::BackgroundThread;
    bool supports_streaming_input = false;
    bool supports_offline_buffers = true;
};

struct AnalyzerProvenance {
    std::string provider_id;
    std::string package_id;
    std::string version;
    std::string analysis_id;
    AnalyzerBackend backend = AnalyzerBackend::BuiltIn;
};

struct AnalyzerMarkerProvenance {
    std::uint32_t marker_index = 0;
    AnalyzerCapability capability = AnalyzerCapability::OnsetDetection;
    AnalyzerProvenance provenance;
};

struct KeyTempoAnalysisConfig {
    double source_sample_rate = 0.0;
    std::uint32_t channels = 0;
    std::uint32_t max_tempo_candidates = 8;
    bool estimate_key = true;
    bool estimate_tempo = true;
};

enum class MusicalKeyMode : std::uint8_t {
    Unknown,
    Major,
    Minor,
};

struct KeyTempoAnalysisResult {
    bool ok = false;
    double tempo_bpm = 0.0;
    double tempo_confidence = 0.0;
    // Pitch-class root, C=0 through B=11. -1 means unavailable.
    int key_root = -1;
    MusicalKeyMode key_mode = MusicalKeyMode::Unknown;
    double key_confidence = 0.0;
    AnalyzerProvenance provenance;
};

enum class TransientClass : std::uint8_t {
    Unknown,
    Kick,
    Snare,
    Clap,
    Hat,
    Cymbal,
    Tom,
    Bass,
    Vocal,
    Percussion,
    Tonal,
    Noise,
};

struct TransientClassification {
    std::uint64_t frame = 0;
    double confidence = 0.0;
    TransientClass transient_class = TransientClass::Unknown;
    AnalyzerProvenance provenance;
};

struct TimePitchProcessSpec {
    double source_sample_rate = 0.0;
    double time_ratio = 1.0;
    double pitch_shift_semitones = 0.0;
    std::uint64_t max_input_frames = 0;
    std::uint64_t max_output_frames = 0;
};

struct TimePitchProcessResult {
    bool ok = false;
    std::uint64_t input_frames_consumed = 0;
    std::uint64_t output_frames_produced = 0;
    AnalyzerProvenance provenance;
};

// Optional provider interfaces. Implementations may allocate, use package code,
// or perform longer scans unless their descriptor explicitly says otherwise.
class KeyTempoAnalyzer {
public:
    virtual ~KeyTempoAnalyzer() = default;
    [[nodiscard]] virtual const AnalyzerDescriptor& descriptor() const noexcept = 0;
    [[nodiscard]] virtual KeyTempoAnalysisResult analyze(
        BufferView<const float> source,
        const KeyTempoAnalysisConfig& config) = 0;
};

class TransientClassifier {
public:
    virtual ~TransientClassifier() = default;
    [[nodiscard]] virtual const AnalyzerDescriptor& descriptor() const noexcept = 0;
    [[nodiscard]] virtual std::vector<TransientClassification> classify(
        BufferView<const float> source,
        std::span<const std::uint64_t> candidate_frames) = 0;
};

class TimePitchProcessor {
public:
    virtual ~TimePitchProcessor() = default;
    [[nodiscard]] virtual const AnalyzerDescriptor& descriptor() const noexcept = 0;
    [[nodiscard]] virtual TimePitchProcessResult process(
        BufferView<const float> input,
        BufferView<float> output,
        const TimePitchProcessSpec& spec) = 0;
};

[[nodiscard]] bool analyzer_descriptor_has_capability(
    const AnalyzerDescriptor& descriptor,
    AnalyzerCapability capability) noexcept;
[[nodiscard]] AnalyzerDescriptorStatus validate_analyzer_descriptor(
    const AnalyzerDescriptor& descriptor) noexcept;
[[nodiscard]] bool analyzer_descriptor_selectable(
    const AnalyzerDescriptor& descriptor,
    AnalyzerCapability capability,
    const AnalyzerSelectionPolicy& policy = {}) noexcept;

[[nodiscard]] AnalyzerLicensePolicy classify_analyzer_license_policy(
    std::string_view license_id) noexcept;
[[nodiscard]] AnalyzerAvailability infer_package_analyzer_availability(
    bool installed,
    bool platform_supported,
    bool license_accepted) noexcept;
[[nodiscard]] AnalyzerDescriptor make_package_analyzer_descriptor(
    const PackageAnalyzerDescriptorInput& input);

[[nodiscard]] AnalyzerProvenance analyzer_provenance_from_descriptor(
    const AnalyzerDescriptor& descriptor,
    std::string analysis_id = {});
[[nodiscard]] bool validate_analyzer_marker_provenance(
    std::size_t marker_count,
    std::span<const AnalyzerMarkerProvenance> provenance) noexcept;
[[nodiscard]] const AnalyzerProvenance* find_analyzer_marker_provenance(
    std::span<const AnalyzerMarkerProvenance> provenance,
    std::uint32_t marker_index) noexcept;

[[nodiscard]] std::vector<AnalyzerDescriptor> built_in_analyzer_descriptors();

class AnalyzerProviderRegistry {
public:
    [[nodiscard]] AnalyzerDescriptorStatus add(AnalyzerDescriptor descriptor);
    // The returned pointer is owned by the registry and may be invalidated by
    // later add() calls. Cache descriptor ids instead of long-lived pointers.
    [[nodiscard]] const AnalyzerDescriptor* find(std::string_view id) const noexcept;
    [[nodiscard]] std::vector<AnalyzerDescriptor> descriptors() const;
    [[nodiscard]] std::vector<AnalyzerDescriptor> descriptors_for(
        AnalyzerCapability capability,
        const AnalyzerSelectionPolicy& policy = {}) const;
    [[nodiscard]] std::size_t size() const noexcept { return descriptors_.size(); }
    [[nodiscard]] bool empty() const noexcept { return descriptors_.empty(); }

private:
    std::vector<AnalyzerDescriptor> descriptors_;
};

}  // namespace pulp::audio
