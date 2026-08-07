#include <pulp/inspect/control_artifacts.hpp>

#include "control_private_store.hpp"

#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <mutex>
#include <system_error>
#include <utility>

namespace pulp::inspect {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kMetadataSchema = "pulp.control.artifact.v1";
constexpr std::size_t kMaximumLineageFieldBytes = 512;
constexpr std::size_t kMaximumContentTypeBytes = 256;

bool lowercase_hex(std::string_view value, std::size_t size) {
    return value.size() == size && std::ranges::all_of(value, [](unsigned char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

bool valid_artifact_id(std::string_view value) {
    constexpr std::string_view prefix = "artifact-";
    return value.starts_with(prefix) && lowercase_hex(value.substr(prefix.size()), 32);
}

bool valid_lineage(const ControlArtifactLineage& lineage) {
    const std::array<std::string_view, 12> fields{
        lineage.broker_id,
        lineage.receipt_id,
        lineage.producer_client_id,
        lineage.producer_registration_id,
        lineage.session_id,
        lineage.instance_id,
        lineage.publication_id,
        lineage.producer_capability_id,
        lineage.producer_operation_id,
        lineage.original_grant_id,
        lineage.consent_decision_id,
        lineage.manifest_digest,
    };
    return lineage.producer_operation_version != 0 && lowercase_hex(lineage.manifest_digest, 64) &&
           lowercase_hex(lineage.producer_artifact_digest, 64) &&
           std::ranges::all_of(fields, [](std::string_view field) {
               return !field.empty() && field.size() <= kMaximumLineageFieldBytes;
           });
}

std::string_view sensitivity_id(ControlArtifactSensitivity sensitivity) {
    switch (sensitivity) {
    case ControlArtifactSensitivity::Public:
        return "public";
    case ControlArtifactSensitivity::Internal:
        return "internal";
    case ControlArtifactSensitivity::Sensitive:
        return "sensitive";
    case ControlArtifactSensitivity::Restricted:
        return "restricted";
    }
    return {};
}

std::optional<ControlArtifactSensitivity> sensitivity_from_id(std::string_view id) {
    if (id == "public")
        return ControlArtifactSensitivity::Public;
    if (id == "internal")
        return ControlArtifactSensitivity::Internal;
    if (id == "sensitive")
        return ControlArtifactSensitivity::Sensitive;
    if (id == "restricted")
        return ControlArtifactSensitivity::Restricted;
    return std::nullopt;
}

std::string_view deletion_state_id(ControlArtifactDeletionState state) {
    switch (state) {
    case ControlArtifactDeletionState::Active:
        return "active";
    case ControlArtifactDeletionState::Deleted:
        return "deleted";
    }
    return {};
}

std::optional<ControlArtifactDeletionState> deletion_state_from_id(std::string_view id) {
    if (id == "active")
        return ControlArtifactDeletionState::Active;
    if (id == "deleted")
        return ControlArtifactDeletionState::Deleted;
    return std::nullopt;
}

std::string_view redaction_state_id(ControlArtifactRedactionState state) {
    switch (state) {
    case ControlArtifactRedactionState::Original:
        return "original";
    case ControlArtifactRedactionState::Redacted:
        return "redacted";
    }
    return {};
}

std::optional<ControlArtifactRedactionState> redaction_state_from_id(std::string_view id) {
    if (id == "original")
        return ControlArtifactRedactionState::Original;
    if (id == "redacted")
        return ControlArtifactRedactionState::Redacted;
    return std::nullopt;
}

bool valid_properties(const ControlArtifactProperties& properties) {
    return !properties.content_type.empty() &&
           properties.content_type.size() <= kMaximumContentTypeBytes &&
           properties.created_at_unix_ms != 0 &&
           properties.expires_at_unix_ms > properties.created_at_unix_ms &&
           !sensitivity_id(properties.sensitivity).empty() &&
           !redaction_state_id(properties.redaction_state).empty();
}

std::string hex_encode(std::string_view value) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string encoded(value.size() * 2, '0');
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto byte = static_cast<unsigned char>(value[index]);
        encoded[index * 2] = digits[byte >> 4];
        encoded[index * 2 + 1] = digits[byte & 0xf];
    }
    return encoded;
}

std::optional<std::string> hex_decode(std::string_view value) {
    if ((value.size() & 1u) != 0)
        return std::nullopt;
    const auto digit = [](unsigned char c) -> std::optional<unsigned> {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        return std::nullopt;
    };
    std::string decoded(value.size() / 2, '\0');
    for (std::size_t index = 0; index < decoded.size(); ++index) {
        const auto high = digit(static_cast<unsigned char>(value[index * 2]));
        const auto low = digit(static_cast<unsigned char>(value[index * 2 + 1]));
        if (!high || !low)
            return std::nullopt;
        decoded[index] = static_cast<char>((*high << 4u) | *low);
    }
    return decoded;
}

std::string encode_metadata(const ControlArtifactMetadata& metadata) {
    std::string encoded;
    const auto append = [&](std::string_view key, std::string_view value) {
        encoded.append(key);
        encoded.push_back('=');
        encoded.append(hex_encode(value));
        encoded.push_back('\n');
    };
    encoded.append(kMetadataSchema);
    encoded.push_back('\n');
    append("artifact_id", metadata.artifact_id);
    append("broker_id", metadata.lineage.broker_id);
    append("receipt_id", metadata.lineage.receipt_id);
    append("producer_client_id", metadata.lineage.producer_client_id);
    append("producer_registration_id", metadata.lineage.producer_registration_id);
    append("session_id", metadata.lineage.session_id);
    append("instance_id", metadata.lineage.instance_id);
    append("publication_id", metadata.lineage.publication_id);
    append("producer_capability_id", metadata.lineage.producer_capability_id);
    append("producer_operation_id", metadata.lineage.producer_operation_id);
    encoded.append("producer_operation_version=");
    encoded.append(std::to_string(metadata.lineage.producer_operation_version));
    encoded.push_back('\n');
    append("original_grant_id", metadata.lineage.original_grant_id);
    append("consent_decision_id", metadata.lineage.consent_decision_id);
    append("manifest_digest", metadata.lineage.manifest_digest);
    append("producer_artifact_digest", metadata.lineage.producer_artifact_digest);
    append("sha256", metadata.sha256);
    encoded.append("byte_size=");
    encoded.append(std::to_string(metadata.byte_size));
    encoded.push_back('\n');
    append("content_type", metadata.content_type);
    encoded.append("created_at_unix_ms=");
    encoded.append(std::to_string(metadata.created_at_unix_ms));
    encoded.push_back('\n');
    encoded.append("expires_at_unix_ms=");
    encoded.append(std::to_string(metadata.expires_at_unix_ms));
    encoded.push_back('\n');
    append("sensitivity", sensitivity_id(metadata.sensitivity));
    append("deletion_state", deletion_state_id(metadata.deletion_state));
    append("redaction_state", redaction_state_id(metadata.redaction_state));
    return encoded;
}

std::optional<ControlArtifactMetadata> decode_metadata(std::string_view text) {
    const auto first_newline = text.find('\n');
    if (first_newline == std::string_view::npos ||
        text.substr(0, first_newline) != kMetadataSchema) {
        return std::nullopt;
    }
    text.remove_prefix(first_newline + 1);
    std::array<std::string, 23> values;
    constexpr std::array<std::string_view, 23> keys{
        "artifact_id",
        "broker_id",
        "receipt_id",
        "producer_client_id",
        "producer_registration_id",
        "session_id",
        "instance_id",
        "publication_id",
        "producer_capability_id",
        "producer_operation_id",
        "producer_operation_version",
        "original_grant_id",
        "consent_decision_id",
        "manifest_digest",
        "producer_artifact_digest",
        "sha256",
        "byte_size",
        "content_type",
        "created_at_unix_ms",
        "expires_at_unix_ms",
        "sensitivity",
        "deletion_state",
        "redaction_state",
    };
    for (std::size_t index = 0; index < keys.size(); ++index) {
        const auto newline = text.find('\n');
        if (newline == std::string_view::npos)
            return std::nullopt;
        const auto line = text.substr(0, newline);
        text.remove_prefix(newline + 1);
        const auto separator = line.find('=');
        if (separator == std::string_view::npos || line.substr(0, separator) != keys[index]) {
            return std::nullopt;
        }
        const auto encoded_value = line.substr(separator + 1);
        if (keys[index] == "producer_operation_version" || keys[index] == "byte_size" ||
            keys[index] == "created_at_unix_ms" || keys[index] == "expires_at_unix_ms") {
            values[index] = encoded_value;
        } else {
            auto decoded = hex_decode(encoded_value);
            if (!decoded)
                return std::nullopt;
            values[index] = std::move(*decoded);
        }
    }
    if (!text.empty())
        return std::nullopt;

    const auto parse_u64 = [&](std::size_t index) -> std::optional<std::uint64_t> {
        std::uint64_t value = 0;
        const auto* begin = values[index].data();
        const auto* end = begin + values[index].size();
        const auto parsed = std::from_chars(begin, end, value);
        if (parsed.ec != std::errc{} || parsed.ptr != end)
            return std::nullopt;
        return value;
    };
    const auto operation_version = parse_u64(10);
    const auto byte_size = parse_u64(16);
    const auto created_at = parse_u64(18);
    const auto expires_at = parse_u64(19);
    const auto sensitivity = sensitivity_from_id(values[20]);
    const auto deletion_state = deletion_state_from_id(values[21]);
    const auto redaction_state = redaction_state_from_id(values[22]);
    if (!operation_version || *operation_version > std::numeric_limits<std::uint32_t>::max() ||
        !byte_size || !created_at || !expires_at || !sensitivity || !deletion_state ||
        !redaction_state) {
        return std::nullopt;
    }

    ControlArtifactMetadata metadata{
        .artifact_id = std::move(values[0]),
        .lineage =
            {
                .broker_id = std::move(values[1]),
                .receipt_id = std::move(values[2]),
                .producer_client_id = std::move(values[3]),
                .producer_registration_id = std::move(values[4]),
                .session_id = std::move(values[5]),
                .instance_id = std::move(values[6]),
                .publication_id = std::move(values[7]),
                .producer_capability_id = std::move(values[8]),
                .producer_operation_id = std::move(values[9]),
                .producer_operation_version = static_cast<std::uint32_t>(*operation_version),
                .original_grant_id = std::move(values[11]),
                .consent_decision_id = std::move(values[12]),
                .manifest_digest = std::move(values[13]),
                .producer_artifact_digest = std::move(values[14]),
            },
        .sha256 = std::move(values[15]),
        .byte_size = *byte_size,
        .content_type = std::move(values[17]),
        .created_at_unix_ms = *created_at,
        .expires_at_unix_ms = *expires_at,
        .sensitivity = *sensitivity,
        .deletion_state = *deletion_state,
        .redaction_state = *redaction_state,
    };
    if (!valid_artifact_id(metadata.artifact_id) || !valid_lineage(metadata.lineage) ||
        !lowercase_hex(metadata.sha256, 64) || metadata.content_type.empty() ||
        metadata.content_type.size() > kMaximumContentTypeBytes ||
        metadata.created_at_unix_ms == 0 ||
        metadata.expires_at_unix_ms <= metadata.created_at_unix_ms) {
        return std::nullopt;
    }
    return metadata;
}

std::optional<std::string> random_hex(std::size_t bytes) {
    const auto random = runtime::secure_random_bytes(bytes);
    return random ? std::optional{runtime::hex_encode(*random)} : std::nullopt;
}

bool authorization_matches(const ControlArtifactMetadata& metadata,
                           const ControlArtifactMetadata& authorized) {
    const auto& actual = metadata.lineage;
    const auto& expected = authorized.lineage;
    return metadata.artifact_id == authorized.artifact_id && metadata.sha256 == authorized.sha256 &&
           metadata.byte_size == authorized.byte_size &&
           metadata.content_type == authorized.content_type &&
           metadata.created_at_unix_ms == authorized.created_at_unix_ms &&
           metadata.expires_at_unix_ms == authorized.expires_at_unix_ms &&
           metadata.sensitivity == authorized.sensitivity &&
           metadata.deletion_state == authorized.deletion_state &&
           metadata.redaction_state == authorized.redaction_state &&
           actual.broker_id == expected.broker_id && actual.receipt_id == expected.receipt_id &&
           actual.producer_client_id == expected.producer_client_id &&
           actual.producer_registration_id == expected.producer_registration_id &&
           actual.session_id == expected.session_id && actual.instance_id == expected.instance_id &&
           actual.publication_id == expected.publication_id &&
           actual.producer_capability_id == expected.producer_capability_id &&
           actual.producer_operation_id == expected.producer_operation_id &&
           actual.producer_operation_version == expected.producer_operation_version &&
           actual.original_grant_id == expected.original_grant_id &&
           actual.consent_decision_id == expected.consent_decision_id &&
           actual.manifest_digest == expected.manifest_digest &&
           actual.producer_artifact_digest == expected.producer_artifact_digest;
}

} // namespace

std::string_view control_artifact_status_id(ControlArtifactStatus status) {
    switch (status) {
    case ControlArtifactStatus::Stored:
        return "stored";
    case ControlArtifactStatus::Read:
        return "read";
    case ControlArtifactStatus::InvalidRequest:
        return "invalid-request";
    case ControlArtifactStatus::Unauthorized:
        return "unauthorized";
    case ControlArtifactStatus::NotFound:
        return "not-found";
    case ControlArtifactStatus::Corrupt:
        return "corrupt";
    case ControlArtifactStatus::ResourceExhausted:
        return "resource-exhausted";
    case ControlArtifactStatus::IoError:
        return "io-error";
    }
    return "invalid-request";
}

class ControlArtifactStore::Impl {
  public:
    explicit Impl(ControlArtifactStoreConfig config_in, WallClock clock_in)
        : config(std::move(config_in)), blobs(config.root / "blobs"),
          artifacts(config.root / "artifacts"), clock(std::move(clock_in)) {
        if (config.root.empty() || config.maximum_blob_bytes == 0 ||
            config.maximum_chunk_bytes == 0 ||
            config.maximum_chunk_bytes > config.maximum_blob_bytes ||
            config.maximum_lifetime.count() <= 0) {
            return;
        }
        std::error_code error;
        const auto parent = config.root.parent_path();
        if (!parent.empty() && !fs::exists(parent, error))
            return;
        ready = detail::ensure_owner_private_directory(config.root) &&
                detail::ensure_owner_private_directory(blobs) &&
                detail::ensure_owner_private_directory(artifacts);
    }

    fs::path blob_path(std::string_view sha256) const {
        return blobs / (std::string(sha256) + ".blob");
    }

    fs::path metadata_path(std::string_view artifact_id) const {
        return artifacts / (std::string(artifact_id) + ".meta");
    }

    std::uint64_t now_unix_ms() const {
        const auto count =
            std::chrono::duration_cast<std::chrono::milliseconds>(clock().time_since_epoch())
                .count();
        return count > 0 ? static_cast<std::uint64_t>(count) : 0;
    }

    void expire(const fs::path& path) const {
        // Removing the metadata revokes discovery and authorization. Shared
        // content-addressed blobs remain orphaned for the aggregate retention
        // and quota collector; an orphan blob is never readable alone.
        (void)detail::remove_owner_private_file_durable(path);
    }

    ControlArtifactStoreConfig config;
    fs::path blobs;
    fs::path artifacts;
    WallClock clock;
    bool ready = false;
    mutable std::mutex mutex;
};

ControlArtifactStore::ControlArtifactStore(ControlArtifactStoreConfig config, WallClock clock)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(clock))) {}

ControlArtifactStore::~ControlArtifactStore() = default;

bool ControlArtifactStore::is_ready() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->ready;
}

ControlArtifactStoreResult ControlArtifactStore::store(std::span<const std::uint8_t> bytes,
                                                       ControlArtifactLineage lineage,
                                                       ControlArtifactProperties properties) {
    const auto now = impl_->now_unix_ms();
    std::lock_guard lock(impl_->mutex);
    if (!impl_->ready)
        return {.status = ControlArtifactStatus::IoError};
    if (bytes.empty() || bytes.size() > impl_->config.maximum_blob_bytes ||
        !valid_lineage(lineage) || !valid_properties(properties) ||
        properties.expires_at_unix_ms <= now ||
        properties.expires_at_unix_ms - now >
            static_cast<std::uint64_t>(impl_->config.maximum_lifetime.count())) {
        return {.status = bytes.size() > impl_->config.maximum_blob_bytes
                              ? ControlArtifactStatus::ResourceExhausted
                              : ControlArtifactStatus::InvalidRequest};
    }

    const auto hash = runtime::sha256_hex(bytes.data(), bytes.size());
    const auto blob_path = impl_->blob_path(hash);
    auto blob_publish = detail::publish_owner_private_file(blob_path, bytes);
    if (blob_publish == detail::OwnerPrivateFilePublishResult::Failed)
        return {.status = ControlArtifactStatus::IoError};
    if (blob_publish == detail::OwnerPrivateFilePublishResult::Exists) {
        const auto existing =
            detail::read_owner_private_file(blob_path, impl_->config.maximum_blob_bytes);
        if (!existing || existing->size() != bytes.size() ||
            runtime::sha256_hex(existing->data(), existing->size()) != hash) {
            return {.status = ControlArtifactStatus::Corrupt};
        }
    }

    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        const auto random = random_hex(16);
        if (!random)
            return {.status = ControlArtifactStatus::ResourceExhausted};
        ControlArtifactMetadata metadata{
            .artifact_id = "artifact-" + *random,
            .lineage = lineage,
            .sha256 = hash,
            .byte_size = static_cast<std::uint64_t>(bytes.size()),
            .content_type = properties.content_type,
            .created_at_unix_ms = properties.created_at_unix_ms,
            .expires_at_unix_ms = properties.expires_at_unix_ms,
            .sensitivity = properties.sensitivity,
            .deletion_state = ControlArtifactDeletionState::Active,
            .redaction_state = properties.redaction_state,
        };
        const auto encoded = encode_metadata(metadata);
        const auto encoded_bytes =
            std::span(reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size());
        const auto published = detail::publish_owner_private_file(
            impl_->metadata_path(metadata.artifact_id), encoded_bytes);
        if (published == detail::OwnerPrivateFilePublishResult::Published) {
            return {
                .status = ControlArtifactStatus::Stored,
                .metadata = std::move(metadata),
            };
        }
        if (published == detail::OwnerPrivateFilePublishResult::Failed)
            return {.status = ControlArtifactStatus::IoError};
    }
    return {.status = ControlArtifactStatus::ResourceExhausted};
}

std::optional<ControlArtifactMetadata>
ControlArtifactStore::metadata(std::string_view artifact_id) const {
    const auto now = impl_->now_unix_ms();
    std::lock_guard lock(impl_->mutex);
    if (!impl_->ready || !valid_artifact_id(artifact_id))
        return std::nullopt;

    const auto path = impl_->metadata_path(artifact_id);
    const auto bytes = detail::read_owner_private_file(path, 64u * 1024u);
    if (!bytes)
        return std::nullopt;
    const std::string_view text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    auto decoded = decode_metadata(text);
    if (!decoded || decoded->artifact_id != artifact_id ||
        decoded->deletion_state != ControlArtifactDeletionState::Active)
        return std::nullopt;
    if (decoded->expires_at_unix_ms <= now) {
        impl_->expire(path);
        return std::nullopt;
    }
    return decoded;
}

ControlArtifactReadResult
ControlArtifactStore::read_authorized(std::string_view artifact_id, std::uint64_t offset,
                                      std::size_t maximum_bytes,
                                      const ControlArtifactMetadata& authorized_metadata) const {
    const auto now = impl_->now_unix_ms();
    std::lock_guard lock(impl_->mutex);
    if (!impl_->ready)
        return {.status = ControlArtifactStatus::IoError};
    if (!valid_artifact_id(artifact_id) || maximum_bytes == 0 ||
        maximum_bytes > impl_->config.maximum_chunk_bytes) {
        return {.status = ControlArtifactStatus::InvalidRequest};
    }

    const auto metadata_path = impl_->metadata_path(artifact_id);
    std::error_code error;
    const auto metadata_status = fs::symlink_status(metadata_path, error);
    if (error == std::errc::no_such_file_or_directory || (!error && !fs::exists(metadata_status))) {
        return {.status = ControlArtifactStatus::NotFound};
    }
    if (error)
        return {.status = ControlArtifactStatus::Corrupt};
    const auto metadata_bytes = detail::read_owner_private_file(metadata_path, 64u * 1024u);
    if (!metadata_bytes)
        return {.status = ControlArtifactStatus::Corrupt};
    const std::string_view metadata_text(reinterpret_cast<const char*>(metadata_bytes->data()),
                                         metadata_bytes->size());
    const auto metadata = decode_metadata(metadata_text);
    if (!metadata || metadata->artifact_id != artifact_id ||
        metadata->byte_size > impl_->config.maximum_blob_bytes) {
        return {.status = ControlArtifactStatus::Corrupt};
    }
    if (metadata->deletion_state != ControlArtifactDeletionState::Active)
        return {.status = ControlArtifactStatus::NotFound};
    if (metadata->expires_at_unix_ms <= now) {
        impl_->expire(metadata_path);
        return {.status = ControlArtifactStatus::NotFound};
    }
    if (!authorization_matches(*metadata, authorized_metadata)) {
        // Do not disclose receipt or producer lineage to an unauthorized
        // caller merely because it guessed an opaque artifact ID.
        return {.status = ControlArtifactStatus::Unauthorized};
    }
    if (offset > metadata->byte_size)
        return {.status = ControlArtifactStatus::InvalidRequest, .metadata = *metadata};

    const auto blob_path = impl_->blob_path(metadata->sha256);
    const auto blob = detail::read_owner_private_file(blob_path, impl_->config.maximum_blob_bytes);
    if (!blob || blob->size() != metadata->byte_size ||
        runtime::sha256_hex(blob->data(), blob->size()) != metadata->sha256) {
        return {.status = ControlArtifactStatus::Corrupt, .metadata = *metadata};
    }

    const auto available = blob->size() - static_cast<std::size_t>(offset);
    const auto count = std::min(available, maximum_bytes);
    ControlArtifactReadResult result{
        .status = ControlArtifactStatus::Read,
        .metadata = *metadata,
        .bytes = {},
        .eof = count == available,
    };
    result.bytes.insert(result.bytes.end(), blob->begin() + static_cast<std::ptrdiff_t>(offset),
                        blob->begin() + static_cast<std::ptrdiff_t>(offset + count));
    return result;
}

} // namespace pulp::inspect
