#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/control_artifacts.hpp>
#include <pulp/runtime/crypto.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace pulp::inspect;

namespace {

namespace fs = std::filesystem;

struct TemporaryDirectory {
    fs::path path;

    TemporaryDirectory() {
        const auto random = pulp::runtime::secure_random_bytes(8);
        REQUIRE(random.has_value());
        path = fs::temp_directory_path() /
               ("pulp-control-artifacts-" + pulp::runtime::hex_encode(*random));
        REQUIRE(fs::create_directory(path));
        fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace, ignored);
        fs::remove_all(path, ignored);
    }
};

ControlArtifactLineage lineage(std::string suffix = "a") {
    return {
        .broker_id = "broker-" + suffix,
        .receipt_id = "receipt-" + suffix,
        .producer_client_id = "client-" + suffix,
        .producer_registration_id = "registration-" + suffix,
        .session_id = "session-" + suffix,
        .instance_id = "instance-" + suffix,
        .publication_id = "publication-" + suffix,
        .producer_capability_id = "dev.pulp.state/read@1",
        .producer_operation_id = "state/read",
        .producer_operation_version = 1,
        .original_grant_id = "grant-" + suffix,
        .consent_decision_id = "consent-" + suffix,
        .manifest_digest = std::string(64, 'a'),
        .producer_artifact_digest = std::string(64, 'b'),
    };
}

ControlArtifactProperties properties(std::string content_type = "text/plain") {
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    return {
        .content_type = std::move(content_type),
        .created_at_unix_ms = 1'000,
        .expires_at_unix_ms = static_cast<std::uint64_t>(now_ms) + 60u * 60u * 1000u,
        .sensitivity = ControlArtifactSensitivity::Sensitive,
        .redaction_state = ControlArtifactRedactionState::Original,
    };
}

std::vector<std::uint8_t> bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

std::size_t count_extension(const fs::path& directory, std::string_view extension) {
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (entry.path().extension() == extension)
            ++count;
    }
    return count;
}

#ifndef _WIN32
void check_owner_private(const fs::path& path, bool directory) {
    struct stat status{};
    REQUIRE(::lstat(path.c_str(), &status) == 0);
    CHECK(status.st_uid == ::geteuid());
    CHECK((status.st_mode & 077) == 0);
    if (directory)
        CHECK(S_ISDIR(status.st_mode));
    else
        CHECK(S_ISREG(status.st_mode));
}
#endif

} // namespace

TEST_CASE("artifact store clock callbacks may reenter read-only store state",
          "[inspect][control][artifacts][concurrency]") {
    TemporaryDirectory temporary;
    ControlArtifactStore* store_address = nullptr;
    std::size_t ready_observations = 0;
    ControlArtifactStore store{
        {.root = temporary.path / "store"},
        [&] {
            if (store_address && store_address->is_ready())
                ++ready_observations;
            return std::chrono::system_clock::now();
        },
    };
    store_address = &store;

    const auto stored = store.store(bytes("reentrant-clock"), lineage(), properties());
    REQUIRE(stored.status == ControlArtifactStatus::Stored);
    REQUIRE(stored.metadata);
    CHECK(ready_observations == 1);
    REQUIRE(store.metadata(stored.metadata->artifact_id));
    CHECK(ready_observations == 2);
}

TEST_CASE("control artifact store publishes blob before opaque ACL metadata",
          "[inspect][control][artifacts]") {
    TemporaryDirectory temporary;
    ControlArtifactStore store{
        {.root = temporary.path / "store", .maximum_blob_bytes = 1024, .maximum_chunk_bytes = 4}};
    const auto content = bytes("artifact-bytes");
    const auto stored = store.store(content, lineage(), properties("application/octet-stream"));
    REQUIRE(stored.status == ControlArtifactStatus::Stored);
    REQUIRE(stored.metadata.has_value());
    CHECK(stored.metadata->artifact_id.starts_with("artifact-"));
    CHECK(stored.metadata->artifact_id.find("receipt") == std::string::npos);
    CHECK(stored.metadata->sha256 == pulp::runtime::sha256_hex(content.data(), content.size()));
    CHECK(stored.metadata->byte_size == content.size());
    CHECK(stored.metadata->lineage.receipt_id == "receipt-a");
    CHECK(stored.metadata->lineage.producer_client_id == "client-a");
    CHECK(stored.metadata->lineage.producer_registration_id == "registration-a");
    CHECK(stored.metadata->lineage.session_id == "session-a");
    CHECK(stored.metadata->lineage.instance_id == "instance-a");
    CHECK(stored.metadata->lineage.publication_id == "publication-a");
    CHECK(stored.metadata->lineage.producer_operation_id == "state/read");
    CHECK(stored.metadata->lineage.original_grant_id == "grant-a");

    const auto metadata_path =
        temporary.path / "store" / "artifacts" / (stored.metadata->artifact_id + ".meta");
    const auto blob_path = temporary.path / "store" / "blobs" / (stored.metadata->sha256 + ".blob");
    CHECK(fs::is_regular_file(metadata_path));
    CHECK(fs::is_regular_file(blob_path));
    CHECK((fs::status(metadata_path).permissions() &
           (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
#ifndef _WIN32
    check_owner_private(temporary.path / "store", true);
    check_owner_private(temporary.path / "store" / "artifacts", true);
    check_owner_private(temporary.path / "store" / "blobs", true);
    check_owner_private(metadata_path, false);
    check_owner_private(blob_path, false);
#endif
}

TEST_CASE("capture render state and trace evidence contracts fail closed",
          "[inspect][control][artifacts][evidence][redaction]") {
    CHECK(control_evidence_contract_matches(ControlEvidenceKind::Screenshot, "image/png",
                                            ControlArtifactSensitivity::Sensitive,
                                            ControlArtifactRedactionState::Redacted));
    CHECK(control_evidence_contract_matches(ControlEvidenceKind::OfflineRender, "audio/wav",
                                            ControlArtifactSensitivity::Internal,
                                            ControlArtifactRedactionState::Original));
    CHECK(control_evidence_contract_matches(
        ControlEvidenceKind::StateSnapshot, "application/vnd.pulp.state-snapshot+json",
        ControlArtifactSensitivity::Restricted, ControlArtifactRedactionState::Redacted));
    CHECK(control_evidence_contract_matches(
        ControlEvidenceKind::PerfettoTrace, "application/vnd.pulp.perfetto-trace",
        ControlArtifactSensitivity::Sensitive, ControlArtifactRedactionState::Redacted));
    CHECK_FALSE(control_evidence_contract_matches(ControlEvidenceKind::Screenshot, "image/jpeg",
                                                  ControlArtifactSensitivity::Sensitive,
                                                  ControlArtifactRedactionState::Redacted));
    CHECK_FALSE(control_evidence_contract_matches(ControlEvidenceKind::Screenshot, "image/png",
                                                  ControlArtifactSensitivity::Internal,
                                                  ControlArtifactRedactionState::Redacted));
    CHECK_FALSE(control_evidence_contract_matches(
        ControlEvidenceKind::StateSnapshot, "application/vnd.pulp.state-snapshot+json",
        ControlArtifactSensitivity::Public, ControlArtifactRedactionState::Original));
}

TEST_CASE("control artifact blobs dedupe without sharing ACL lineage",
          "[inspect][control][artifacts][security]") {
    TemporaryDirectory temporary;
    ControlArtifactStore store{{.root = temporary.path / "store"}};
    const auto content = bytes("shared immutable bytes");
    const auto first = store.store(content, lineage("a"), properties());
    const auto second = store.store(content, lineage("b"), properties());
    REQUIRE(first.metadata.has_value());
    REQUIRE(second.metadata.has_value());
    CHECK(first.metadata->artifact_id != second.metadata->artifact_id);
    CHECK(first.metadata->sha256 == second.metadata->sha256);
    CHECK(count_extension(temporary.path / "store" / "blobs", ".blob") == 1);
    CHECK(count_extension(temporary.path / "store" / "artifacts", ".meta") == 2);
}

TEST_CASE("control artifact expiry is enforced inside the store",
          "[inspect][control][artifacts][expiry][security]") {
    TemporaryDirectory temporary;
    auto now_ms = std::make_shared<std::int64_t>(1'500);
    ControlArtifactStore store({.root = temporary.path / "store"}, [now_ms] {
        return std::chrono::system_clock::time_point{std::chrono::milliseconds(*now_ms)};
    });
    auto expiring = properties();
    expiring.expires_at_unix_ms = 2'000;
    const auto stored = store.store(bytes("expires"), lineage(), expiring);
    REQUIRE(stored.metadata);
    REQUIRE(store.metadata(stored.metadata->artifact_id));
    const auto metadata_path =
        temporary.path / "store" / "artifacts" / (stored.metadata->artifact_id + ".meta");
    const auto blob_path = temporary.path / "store" / "blobs" / (stored.metadata->sha256 + ".blob");
    REQUIRE(fs::exists(metadata_path));
    REQUIRE(fs::exists(blob_path));
    *now_ms = 2'000;
    CHECK_FALSE(store.metadata(stored.metadata->artifact_id));
    CHECK_FALSE(fs::exists(metadata_path));
    // Shared immutable blobs are retained for the aggregate collector,
    // but without metadata there is no artifact publication or authorization.
    CHECK(fs::exists(blob_path));
    CHECK(store.store(bytes("already expired"), lineage("old"), expiring).status ==
          ControlArtifactStatus::InvalidRequest);
}

TEST_CASE("control artifact lifetime is bounded by store policy",
          "[inspect][control][artifacts][expiry][security]") {
    TemporaryDirectory temporary;
    auto now_ms = std::make_shared<std::int64_t>(1'500);
    ControlArtifactStore store(
        {.root = temporary.path / "store", .maximum_lifetime = std::chrono::milliseconds{500}},
        [now_ms] {
            return std::chrono::system_clock::time_point{std::chrono::milliseconds(*now_ms)};
        });
    auto bounded = properties();
    bounded.created_at_unix_ms = 1'500;
    bounded.expires_at_unix_ms = 2'000;
    CHECK(store.store(bytes("bounded"), lineage(), bounded).status ==
          ControlArtifactStatus::Stored);
    bounded.expires_at_unix_ms = 2'001;
    CHECK(store.store(bytes("too long"), lineage("long"), bounded).status ==
          ControlArtifactStatus::InvalidRequest);
}

TEST_CASE("control artifact quotas bound aggregate and per-client publications",
          "[inspect][control][artifacts][quota][security]") {
    TemporaryDirectory temporary;
    ControlArtifactStore store{{.root = temporary.path / "store",
                                .maximum_blob_bytes = 16,
                                .maximum_chunk_bytes = 8,
                                .maximum_total_bytes = 8,
                                .maximum_artifacts = 3,
                                .maximum_artifacts_per_client = 2}};
    REQUIRE(store.store(bytes("1234"), lineage("a"), properties()).status ==
            ControlArtifactStatus::Stored);
    auto same_client = lineage("b");
    same_client.producer_client_id = "client-a";
    REQUIRE(store.store(bytes("5678"), same_client, properties()).status ==
            ControlArtifactStatus::Stored);
    auto third = lineage("c");
    third.producer_client_id = "client-a";
    CHECK(store.store(bytes("x"), third, properties()).status ==
          ControlArtifactStatus::ResourceExhausted);
    CHECK(store.store(bytes("x"), lineage("d"), properties()).status ==
          ControlArtifactStatus::ResourceExhausted);
}

TEST_CASE(
    "control artifact collector removes expiry orphans and partial writes with redacted audit",
    "[inspect][control][artifacts][retention][crash][redaction]") {
    TemporaryDirectory temporary;
    auto now_ms = std::make_shared<std::int64_t>(1'500);
    const auto root = temporary.path / "store";
    ControlArtifactStore store({.root = root}, [now_ms] {
        return std::chrono::system_clock::time_point{std::chrono::milliseconds(*now_ms)};
    });
    auto expiring = properties("application/vnd.pulp.state-snapshot+json");
    expiring.created_at_unix_ms = 1'000;
    expiring.expires_at_unix_ms = 2'000;
    expiring.redaction_state = ControlArtifactRedactionState::Redacted;
    const auto stored =
        store.store(bytes(R"({"token":"[redacted]"})"), lineage("private-secret-marker"), expiring);
    REQUIRE(stored.metadata);

    const auto orphan_hash = std::string(64, 'c');
    const auto orphan = root / "blobs" / (orphan_hash + ".blob");
    {
        std::ofstream output(orphan, std::ios::binary);
        output << "orphan";
    }
    fs::permissions(orphan, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace);
    const auto partial = root / "artifacts" / ".private-publish-crashed";
    {
        std::ofstream output(partial, std::ios::binary);
        output << "partial-secret-marker";
    }
    fs::permissions(partial, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace);

    *now_ms = 2'000;
    const auto collected = store.collect();
    CHECK(collected.deleted_artifacts == 1);
    CHECK(collected.deleted_orphan_blobs == 2); // expired content + crash orphan
    CHECK(collected.deleted_partial_files == 1);
    CHECK_FALSE(fs::exists(partial));
    const auto audit = store.deletion_audit();
    REQUIRE(audit.size() == 1);
    CHECK(audit.front().artifact_id == stored.metadata->artifact_id);
    CHECK(audit.front().reason == ControlArtifactDeletionReason::Expired);
    std::ifstream audit_file(root / "audit" / "deletions.log", std::ios::binary);
    REQUIRE(audit_file.good());
    const std::string persisted((std::istreambuf_iterator<char>(audit_file)),
                                std::istreambuf_iterator<char>());
    CHECK(persisted.find("private-secret-marker") == std::string::npos);
    CHECK(persisted.find("partial-secret-marker") == std::string::npos);
}

TEST_CASE("orphan control artifact blob from crash is invisible and reusable",
          "[inspect][control][artifacts][crash]") {
    TemporaryDirectory temporary;
    const auto root = temporary.path / "store";
    ControlArtifactStore store{{.root = root}};
    const auto content = bytes("blob survived before metadata publish");
    const auto hash = pulp::runtime::sha256_hex(content.data(), content.size());
    const auto orphan = root / "blobs" / (hash + ".blob");
    {
        std::ofstream output(orphan, std::ios::binary);
        output.write(reinterpret_cast<const char*>(content.data()),
                     static_cast<std::streamsize>(content.size()));
    }
    fs::permissions(orphan, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace);

    CHECK(count_extension(root / "artifacts", ".meta") == 0);
    CHECK_FALSE(store.metadata("artifact-00000000000000000000000000000000"));

    const auto stored = store.store(content, lineage(), properties());
    REQUIRE(stored.status == ControlArtifactStatus::Stored);
    REQUIRE(stored.metadata.has_value());
    CHECK(stored.metadata->sha256 == hash);
    CHECK(count_extension(root / "blobs", ".blob") == 1);
    CHECK(count_extension(root / "artifacts", ".meta") == 1);
}

TEST_CASE("control artifact corruption symlinks and insecure files fail closed",
          "[inspect][control][artifacts][security]") {
    TemporaryDirectory temporary;
    const auto root = temporary.path / "store";
    ControlArtifactStore store{{.root = root}};
    const auto content = bytes("integrity checked");

    auto insecure = store.store(content, lineage("insecure"), properties());
    REQUIRE(insecure.metadata.has_value());
    const auto insecure_metadata = root / "artifacts" / (insecure.metadata->artifact_id + ".meta");
    fs::permissions(insecure_metadata, fs::perms::group_read, fs::perm_options::add);
    CHECK_FALSE(store.metadata(insecure.metadata->artifact_id));
    fs::permissions(insecure_metadata, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace);
}

TEST_CASE("control artifact identifiers cannot traverse or substitute metadata",
          "[inspect][control][artifacts][security][traversal]") {
    TemporaryDirectory temporary;
    const auto root = temporary.path / "store";
    ControlArtifactStore store{{.root = root}};
    const auto content = bytes("metadata target");
    const auto stored = store.store(content, lineage("traversal"), properties());
    REQUIRE(stored.metadata.has_value());

    CHECK_FALSE(store.metadata("../outside"));
    CHECK_FALSE(store.metadata("artifact-00000000000000000000000000000000/../outside"));

    const auto metadata_path = root / "artifacts" / (stored.metadata->artifact_id + ".meta");
    const auto displaced = root / "displaced-metadata";
    fs::rename(metadata_path, displaced);
    std::error_code symlink_error;
    fs::create_symlink(displaced, metadata_path, symlink_error);
    if (!symlink_error) {
        CHECK_FALSE(store.metadata(stored.metadata->artifact_id));
    }
}

TEST_CASE("control artifact store validates blob and chunk bounds",
          "[inspect][control][artifacts][bounds]") {
    TemporaryDirectory temporary;
    ControlArtifactStore store{
        {.root = temporary.path / "store", .maximum_blob_bytes = 8, .maximum_chunk_bytes = 4}};
    CHECK(store.store({}, lineage(), properties()).status == ControlArtifactStatus::InvalidRequest);
    CHECK(store.store(bytes("123456789"), lineage(), properties()).status ==
          ControlArtifactStatus::ResourceExhausted);
    const auto stored = store.store(bytes("12345678"), lineage(), properties());
    REQUIRE(stored.metadata.has_value());
    CHECK(store.metadata(stored.metadata->artifact_id).has_value());
}
