#include <pulp/audio/sample_bank.hpp>

#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/temporary_file.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <system_error>
#include <vector>

namespace pulp::audio {
namespace {

std::optional<std::vector<std::uint8_t>> read_bounded(const std::filesystem::path& path,
                                                      std::uint64_t maximum) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > maximum ||
        size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (!input)
        return std::nullopt;
    return bytes;
}

bool path_contains_symlink(const std::filesystem::path& root,
                           const std::filesystem::path& relative) {
    std::error_code error;
    auto current = root;
    for (const auto& component : relative) {
        current /= component;
        const auto status = std::filesystem::symlink_status(current, error);
        if (error) {
            // A missing component is not a symlink; the caller's existence
            // check reports it as a filesystem error. Any other failure
            // (permissions, IO) stays suspicious.
            return error != std::errc::no_such_file_or_directory;
        }
        if (std::filesystem::is_symlink(status)) return true;
    }
    return false;
}

} // namespace

struct PreparedSampleBank::Impl {
    struct Entry {
        std::uint32_t id = kInvalidSampleId;
        std::unique_ptr<SampleAsset> asset;
        SampleAssetView asset_view{};
        std::unique_ptr<PublishedSampleStore> store;
    };
    SampleBankManifest manifest;
    std::vector<Entry> entries;
    SamplePool pool;
    SampleZoneMap zones;
};

PreparedSampleBank::PreparedSampleBank() : impl_(std::make_unique<Impl>()) {}
PreparedSampleBank::~PreparedSampleBank() = default;
PreparedSampleBank::PreparedSampleBank(PreparedSampleBank&&) noexcept = default;
PreparedSampleBank& PreparedSampleBank::operator=(PreparedSampleBank&&) noexcept = default;
const SampleBankManifest& PreparedSampleBank::manifest() const noexcept {
    return impl_->manifest;
}
const SampleZoneMap& PreparedSampleBank::zone_map() const noexcept {
    return impl_->zones;
}
const SamplePool& PreparedSampleBank::sample_pool() const noexcept {
    return impl_->pool;
}
const SampleAssetView* PreparedSampleBank::sample_asset(std::uint32_t sample_id) const noexcept {
    const auto found =
        std::find_if(impl_->entries.begin(), impl_->entries.end(),
                     [sample_id](const auto& entry) { return entry.id == sample_id; });
    return found == impl_->entries.end() || !found->asset_view.valid() ? nullptr
                                                                       : &found->asset_view;
}

SampleBankMaterializeResult
SampleBankMaterializer::materialize(const SampleBankManifest& manifest,
                                    const std::filesystem::path& content_root,
                                    const SampleBankMaterializePolicy& policy) const {
    SampleBankMaterializeResult result;
    result.status = validate_sample_bank_manifest(manifest, result.field_path);
    if (result.status != SampleBankStatus::ok)
        return result;
    if (manifest.samples.size() > policy.max_samples || manifest.zones.size() > policy.max_zones) {
        result.status = SampleBankStatus::resource_limit_exceeded;
        result.field_path = "$";
        return result;
    }
    if (!std::isfinite(policy.target_sample_rate) || policy.target_sample_rate < 0.0) {
        result.status = SampleBankStatus::unsupported_materialization_policy;
        result.field_path = "$";
        return result;
    }
    std::error_code error;
    const auto canonical_root = std::filesystem::weakly_canonical(content_root, error);
    if (error || !std::filesystem::is_directory(canonical_root)) {
        result.status = SampleBankStatus::filesystem_error;
        result.field_path = "$";
        return result;
    }
    auto bank = std::make_unique<PreparedSampleBank>();
    bank->impl_->manifest = manifest;
    bank->impl_->entries.reserve(manifest.samples.size());
    std::vector<SamplePoolEntry> pool_entries;
    pool_entries.reserve(manifest.samples.size());
    std::uint64_t total_decoded_bytes = 0;
    SampleAssetImporter importer;
    for (std::size_t i = 0; i < manifest.samples.size(); ++i) {
        const auto& sample = manifest.samples[i];
        const auto field = "$.samples[" + std::to_string(i) + "]";
        const auto joined = canonical_root / std::filesystem::path(sample.path);
        if (path_contains_symlink(canonical_root, std::filesystem::path(sample.path))) {
            result.status = SampleBankStatus::path_escape;
            result.field_path = field + ".path";
            return result;
        }
        const auto canonical_file = std::filesystem::weakly_canonical(joined, error);
        if (error || !std::filesystem::is_regular_file(canonical_file)) {
            result.status = SampleBankStatus::filesystem_error;
            result.field_path = field + ".path";
            return result;
        }
        const auto relative = std::filesystem::relative(canonical_file, canonical_root, error);
        if (error || relative.empty() || *relative.begin() == "..") {
            result.status = SampleBankStatus::path_escape;
            result.field_path = field + ".path";
            return result;
        }
        auto bytes = read_bounded(canonical_file, policy.max_encoded_bytes_per_sample);
        if (!bytes) {
            result.status = SampleBankStatus::resource_limit_exceeded;
            result.field_path = field + ".path";
            return result;
        }
        if (runtime::sha256_hex(bytes->data(), bytes->size()) != sample.sha256) {
            result.status = SampleBankStatus::hash_mismatch;
            result.field_path = field + ".sha256";
            return result;
        }
        pulp::runtime::TemporaryFile verified_file(canonical_file.extension().string());
        {
            std::ofstream output(verified_file.path(), std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(bytes->data()),
                         static_cast<std::streamsize>(bytes->size()));
            output.close();
            if (!output) {
                result.status = SampleBankStatus::filesystem_error;
                result.field_path = field + ".path";
                return result;
            }
        }
        auto import_policy = policy.sample_policy;
        const auto remaining_decoded_bytes = policy.max_total_decoded_bytes - total_decoded_bytes;
        const std::uint64_t retained_copy_count =
            policy.residency == SampleBankMaterializePolicy::Residency::published_pool ? 2 : 1;
        // The importer-owned buffer remains live while the destination copies are made.
        // Budget that transient allocation as well as every retained copy.
        const auto peak_live_copy_count = retained_copy_count + 1;
        const auto remaining_source_bytes = remaining_decoded_bytes / peak_live_copy_count;
        if (remaining_source_bytes == 0) {
            result.status = SampleBankStatus::resource_limit_exceeded;
            result.field_path = field + ".path";
            return result;
        }
        if (import_policy.max_decoded_bytes == 0 ||
            import_policy.max_decoded_bytes > remaining_source_bytes)
            import_policy.max_decoded_bytes = remaining_source_bytes;
        const auto descriptor =
            importer.describe_audio_file(verified_file.path_string(), import_policy);
        if (!descriptor.ok()) {
            result.status = descriptor.status == SampleAssetStatus::byte_budget_exceeded
                                ? SampleBankStatus::resource_limit_exceeded
                                : SampleBankStatus::import_failed;
            result.asset_status = descriptor.status;
            result.field_path = field + ".path";
            return result;
        }
        auto imported = importer.import_audio_file(verified_file.path_string(), import_policy);
        if (!imported.ok()) {
            result.status = SampleBankStatus::import_failed;
            result.asset_status = imported.descriptor.status;
            result.field_path = field + ".path";
            return result;
        }
        if (imported.descriptor.decoded_bytes > remaining_source_bytes) {
            result.status = SampleBankStatus::resource_limit_exceeded;
            result.field_path = field + ".path";
            return result;
        }
        total_decoded_bytes += imported.descriptor.decoded_bytes * retained_copy_count;
        if (imported.audio.num_frames() > policy.preload_frames_per_sample) {
            result.status = SampleBankStatus::unsupported_materialization_policy;
            result.field_path = field + ".path";
            return result;
        }
        std::vector<float*> pointers;
        std::vector<const float*> const_pointers;
        pointers.reserve(imported.audio.channels.size());
        const_pointers.reserve(imported.audio.channels.size());
        for (auto& channel : imported.audio.channels) {
            pointers.push_back(channel.data());
            const_pointers.push_back(channel.data());
        }
        BufferView<float> view(pointers.data(), pointers.size(), imported.audio.num_frames());
        BufferView<const float> const_view(const_pointers.data(), const_pointers.size(),
                                           imported.audio.num_frames());
        PreparedSampleBank::Impl::Entry entry;
        entry.id = sample.id;
        SampleAssetConfig config;
        config.asset = {sample.id, 1};
        config.source = {sample.id, 1};
        config.channels = imported.audio.num_channels();
        config.total_frames = imported.audio.num_frames();
        config.sample_rate = imported.audio.sample_rate;
        config.preload_frames = imported.audio.num_frames();
        if (policy.residency == SampleBankMaterializePolicy::Residency::sample_assets) {
            entry.asset = std::make_unique<SampleAsset>();
            if (!entry.asset->prepare(config, view)) {
                result.status = SampleBankStatus::preparation_failed;
                result.field_path = field;
                return result;
            }
            entry.asset_view = entry.asset->view();
        } else {
            entry.store = std::make_unique<PublishedSampleStore>();
            if (!entry.store->prepare({2, config.channels, config.total_frames}) ||
                !entry.store->publish(const_view, config.total_frames, config.sample_rate)) {
                result.status = SampleBankStatus::preparation_failed;
                result.field_path = field;
                return result;
            }
            pool_entries.push_back(
                {sample.id, entry.store.get(), entry.store->read_published_view()});
        }
        bank->impl_->entries.push_back(std::move(entry));
    }
    if (!pool_entries.empty() && !bank->impl_->pool.configure(pool_entries)) {
        result.status = SampleBankStatus::preparation_failed;
        result.field_path = "$.samples";
        return result;
    }
    std::vector<SampleZone> prepared_zones;
    prepared_zones.reserve(manifest.zones.size());
    for (const auto& zone : manifest.zones)
        prepared_zones.push_back(zone.zone);
    for (std::size_t i = 0; i < prepared_zones.size(); ++i) {
        if (policy.residency == SampleBankMaterializePolicy::Residency::sample_assets) {
            if (prepared_zones[i].slice_index != kNoSampleSliceIndex ||
                prepared_zones[i].has_loop) {
                result.status = SampleBankStatus::unsupported_materialization_policy;
                result.field_path = "$.zones[" + std::to_string(i) + "]";
                return result;
            }
            continue;
        }
        auto resolved = bank->impl_->pool.resolve(prepared_zones[i].sample_id);
        if (!resolved.valid) {
            result.status = SampleBankStatus::missing_sample_reference;
            result.field_path = "$.zones[" + std::to_string(i) + "].sample_id";
            return result;
        }
        prepared_zones[i].sample = resolved.view;
        if (prepared_zones[i].has_loop) {
            prepared_zones[i].loop.source_sample_rate = resolved.view.sample_rate;
        }
    }
    if (!bank->impl_->zones.configure(prepared_zones)) {
        result.status = SampleBankStatus::invalid_zone;
        result.field_path = "$.zones";
        return result;
    }
    result.status = SampleBankStatus::ok;
    result.bank = std::move(bank);
    return result;
}

} // namespace pulp::audio
