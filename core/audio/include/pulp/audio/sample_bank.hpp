#pragma once

#include <pulp/audio/sample_asset.hpp>
#include <pulp/audio/sample_asset_io.hpp>
#include <pulp/audio/sample_pool.hpp>
#include <pulp/audio/sample_zone_map.hpp>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::audio {

inline constexpr std::string_view kSampleBankSchema = "pulp.sample-bank.v1";
inline constexpr std::size_t kSampleBankMaxJsonBytes = 16u << 20;
inline constexpr std::uint32_t kSampleBankMaxSamples = 4096;
inline constexpr std::uint32_t kSampleBankMaxZones = 65536;

struct SampleBankSample {
    std::uint32_t id = kInvalidSampleId;
    std::string path;
    std::string sha256;
    std::string license_ref;
    // Canonical JSON object with namespaced keys (for example
    // "pulp.sample-edit.v1"). Unknown extension payloads are preserved but
    // never interpreted by the v1 materializer.
    std::string extensions_json = "{}";
};

struct SampleBankZone {
    SampleZone zone;
    std::string extensions_json = "{}";
};

struct SampleBankManifest {
    std::string schema = std::string(kSampleBankSchema);
    std::string id;
    std::string name;
    std::vector<SampleBankSample> samples;
    std::vector<SampleBankZone> zones;
    std::string extensions_json = "{}";
};

enum class SampleBankStatus : std::uint8_t {
    ok,
    invalid_json,
    root_not_object,
    unknown_field,
    duplicate_field,
    missing_field,
    wrong_type,
    unsupported_schema,
    invalid_identifier,
    invalid_sample_id,
    invalid_path,
    invalid_hash,
    duplicate_sample_id,
    duplicate_sample_path,
    invalid_zone,
    missing_sample_reference,
    filesystem_error,
    path_escape,
    hash_mismatch,
    import_failed,
    preparation_failed,
    resource_limit_exceeded,
    unsupported_materialization_policy,
};

const char* sample_bank_status_name(SampleBankStatus status) noexcept;

struct SampleBankParseResult {
    SampleBankStatus status = SampleBankStatus::invalid_json;
    std::string field_path;
    SampleBankManifest manifest;

    bool valid() const noexcept {
        return status == SampleBankStatus::ok;
    }
};

struct SampleBankWriteResult {
    SampleBankStatus status = SampleBankStatus::invalid_zone;
    std::string field_path;
    std::string json;

    bool valid() const noexcept {
        return status == SampleBankStatus::ok;
    }
};

SampleBankParseResult parse_sample_bank_json(std::string_view json);
SampleBankStatus validate_sample_bank_manifest(const SampleBankManifest& manifest,
                                               std::string& field_path);
SampleBankWriteResult write_sample_bank_json(const SampleBankManifest& manifest);

struct SampleBankContentIssue {
    SampleBankStatus status = SampleBankStatus::filesystem_error;
    std::string manifest_path;
    std::string field_path;
};

struct SampleBankContentValidationResult {
    std::vector<std::string> manifest_paths;
    std::vector<std::string> sample_paths;
    std::vector<SampleBankContentIssue> issues;

    bool valid() const noexcept { return issues.empty(); }
};

/// Validate exported bank manifests and every referenced sample under one
/// content root. Paths are confined, symlinks are rejected, manifests and
/// sample reads are bounded, and hashes are verified. Control/background
/// thread only.
SampleBankContentValidationResult validate_sample_bank_content(
    const std::filesystem::path& content_root,
    std::span<const std::string> exported_bank_paths,
    std::uint64_t max_encoded_bytes_per_sample = 1ull << 30);

struct SampleBankMaterializePolicy {
    enum class Residency : std::uint8_t {
        sample_assets,
        published_pool,
    };

    SampleAssetPolicy sample_policy;
    // Optional host/render rate used to validate preparation context. Samples
    // always preserve their native rates; playback consumers apply the existing
    // source-rate / host-rate ratio.
    double target_sample_rate = 0.0;
    Residency residency = Residency::published_pool;
    // v1 materializes resident data. Values smaller than a decoded sample are
    // rejected instead of silently discarding its tail.
    std::uint64_t preload_frames_per_sample = std::numeric_limits<std::uint64_t>::max();
    std::uint32_t max_samples = kSampleBankMaxSamples;
    std::uint32_t max_zones = kSampleBankMaxZones;
    std::uint64_t max_encoded_bytes_per_sample = 1ull << 30;
    std::uint64_t max_total_decoded_bytes = 8ull << 30;
};

class PreparedSampleBank {
  public:
    PreparedSampleBank();
    ~PreparedSampleBank();
    PreparedSampleBank(const PreparedSampleBank&) = delete;
    PreparedSampleBank& operator=(const PreparedSampleBank&) = delete;
    PreparedSampleBank(PreparedSampleBank&&) noexcept;
    PreparedSampleBank& operator=(PreparedSampleBank&&) noexcept;

    const SampleBankManifest& manifest() const noexcept;
    const SampleZoneMap& zone_map() const noexcept;
    const SamplePool& sample_pool() const noexcept;
    const SampleAssetView* sample_asset(std::uint32_t sample_id) const noexcept;

  private:
    friend class SampleBankMaterializer;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct SampleBankMaterializeResult {
    SampleBankStatus status = SampleBankStatus::preparation_failed;
    std::string field_path;
    SampleAssetStatus asset_status = SampleAssetStatus::ok;
    std::unique_ptr<PreparedSampleBank> bank;

    bool valid() const noexcept {
        return status == SampleBankStatus::ok && bank != nullptr;
    }
};

/// Control/background-thread loader. The returned owner keeps every borrowed
/// SampleAssetView, PublishedSampleView, SamplePool entry, and SampleZoneMap
/// snapshot alive. Destroy or replace it only after realtime readers quiesce.
class SampleBankMaterializer {
  public:
    SampleBankMaterializeResult materialize(const SampleBankManifest& manifest,
                                            const std::filesystem::path& content_root,
                                            const SampleBankMaterializePolicy& policy = {}) const;
};

} // namespace pulp::audio
