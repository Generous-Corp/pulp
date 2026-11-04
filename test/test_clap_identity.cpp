// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <pulp/host/isolated_scanner.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/scan_cache.hpp>
#include <pulp/runtime/crypto.hpp>

using namespace pulp::host;
namespace fs = std::filesystem;
namespace {
struct Fixture {
    fs::path directory =
        fs::temp_directory_path() /
        ("pulp-clap-identity-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() {
        fs::create_directory(directory);
    }
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(directory, ec);
    }
    std::string copy(const std::string& name) {
        const auto file = directory / (name + ".clap");
        fs::copy_file(PULP_CLAP_IDENTITY_FIXTURE, file);
        return file.string();
    }
};
constexpr const char* second_id = "org.pulp.identity.second";
void refuses(const std::string& path, const std::string& id, PluginIdentityError error) {
    auto result = PluginSlot::load_exact({path, {"clap", id}});
    REQUIRE(std::holds_alternative<PluginIdentityError>(result.value));
    CHECK(std::get<PluginIdentityError>(result.value) == error);
}
} // namespace

TEST_CASE("CLAP exact loading chooses the second same-name descriptor and owns evidence",
          "[host][clap][identity]") {
    Fixture fixture;
    auto path = fixture.copy("positive");
    std::optional<PluginResolvedIdentity> receipt;
    {
        auto result = PluginSlot::load_exact({path, {"clap", second_id}});
        REQUIRE(std::holds_alternative<std::unique_ptr<PluginSlot>>(result.value));
        auto slot = std::move(std::get<std::unique_ptr<PluginSlot>>(result.value));
        REQUIRE(slot);
        CHECK(slot->info().name == "Same name");
        CHECK(slot->info().unique_id == second_id);
        auto legacy_info = slot->info();
        legacy_info.unique_id = "missing";
        auto legacy = PluginSlot::load(legacy_info);
        REQUIRE(legacy);
        CHECK_FALSE(legacy->info().identity_evidence);
        receipt = slot->resolved_identity();
        REQUIRE(receipt);
        CHECK(receipt->loaded.verification() == PluginIdentityVerification::loaded);
        CHECK(receipt->loaded.origin() == PluginIdentityOrigin::clap_descriptor);
        CHECK(receipt->loaded.type() == receipt->requested);
        CHECK(receipt->loaded.artifact().sha256 ==
              *pulp::runtime::sha256_file_hex(path, 1ULL << 30));
        CHECK(receipt->loaded.artifact().byte_length == fs::file_size(path));
        CHECK(receipt->loaded.artifact().descriptor_version == "1.0");
    }
    auto canonical = canonical_plugin_identity(receipt->loaded);
    REQUIRE(std::holds_alternative<CanonicalPluginIdentity>(canonical));
    CHECK(std::get<CanonicalPluginIdentity>(canonical).bytes ==
          "{\"format\":\"clap\",\"native_type\":{\"descriptor_id\":\"org.pulp.identity.second\"}}");
    auto moved = PluginSlot::load_exact({fixture.copy("moved"), {"clap", second_id}});
    REQUIRE(std::holds_alternative<std::unique_ptr<PluginSlot>>(moved.value));
    auto moved_receipt = std::get<std::unique_ptr<PluginSlot>>(moved.value)->resolved_identity();
    CHECK(
        std::get<CanonicalPluginIdentity>(canonical_plugin_identity(moved_receipt->loaded)).bytes ==
        std::get<CanonicalPluginIdentity>(canonical).bytes);
    CHECK(moved_receipt->loaded.artifact() == receipt->loaded.artifact());
    auto imported =
        parse_plugin_identity_evidence(serialize_plugin_identity_evidence(receipt->loaded));
    REQUIRE(imported);
    CHECK(imported->verification() == PluginIdentityVerification::declared);
    CHECK(imported->artifact() == receipt->loaded.artifact());
    CHECK(std::get<CanonicalPluginIdentity>(canonical_plugin_identity(*imported))
              .evidence.verification() == PluginIdentityVerification::declared);
}

TEST_CASE("CLAP strict selection refuses invalid, ambiguous, incomplete and mismatched factories",
          "[host][clap][identity]") {
    Fixture fixture;
    auto valid = fixture.copy("valid");
    refuses(valid, "Same name", PluginIdentityError::descriptor_selection_missing);
    refuses(valid, "absent", PluginIdentityError::descriptor_selection_missing);
    refuses(valid, "", PluginIdentityError::native_identity_missing);
    refuses(valid, std::string("a\0b", 3), PluginIdentityError::invalid_native_identity);
    refuses(fixture.copy("duplicate"), "org.pulp.identity.first",
            PluginIdentityError::descriptor_selection_ambiguous);
    refuses(fixture.copy("null"), second_id, PluginIdentityError::native_identity_missing);
    refuses(fixture.copy("empty"), second_id, PluginIdentityError::native_identity_missing);
    refuses(fixture.copy("invalid"), second_id, PluginIdentityError::invalid_native_identity);
    refuses(fixture.copy("hole"), "org.pulp.identity.first",
            PluginIdentityError::descriptor_enumeration_incomplete);
    refuses(fixture.copy("capped"), "org.pulp.identity.first",
            PluginIdentityError::descriptor_enumeration_incomplete);
    refuses(fixture.copy("mismatch"), second_id, PluginIdentityError::loaded_identity_mismatch);
    refuses(fixture.copy("createfail"), second_id, PluginIdentityError::plugin_load_failed);
    refuses(fixture.copy("initfail"), second_id, PluginIdentityError::plugin_load_failed);
    PluginInfo legacy;
    legacy.format = PluginFormat::CLAP;
    legacy.path = valid;
    legacy.unique_id = "absent";
    auto slot = PluginSlot::load(legacy);
    REQUIRE(slot);
    CHECK(slot->info().unique_id == "org.pulp.identity.first");
    CHECK_FALSE(slot->resolved_identity());
}

TEST_CASE("CLAP identity validation preserves exact UTF-8 and declared trust",
          "[host][clap][identity]") {
    for (const std::string id : {"a b", "ORG.Pulp/a", "caf\xc3\xa9", "cafe\xcc\x81"}) {
        const auto assertion = PluginIdentityEvidence::declared({"clap", id});
        auto result = canonical_plugin_identity(assertion);
        REQUIRE(std::holds_alternative<CanonicalPluginIdentity>(result));
        CHECK(std::get<CanonicalPluginIdentity>(result).evidence.type().descriptor_id == id);
        CHECK(std::get<CanonicalPluginIdentity>(result).evidence.verification() ==
              PluginIdentityVerification::declared);
    }
    for (const std::string id :
         {"a\n", "\xc2\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82", "\xc0\xaf"})
        CHECK(validate_plugin_type_identity({"clap", id}) ==
              PluginIdentityError::invalid_native_identity);
    CHECK(validate_plugin_type_identity({"vst3", "anything"}) ==
          PluginIdentityError::unsupported_format);
}

TEST_CASE("CLAP scanner captures provenance across worker and cache without promotion",
          "[host][clap][identity]") {
    Fixture fixture;
    const auto path = fixture.copy("scan");
    auto descriptors = PluginScanner::scan_bundle(path, PluginFormat::CLAP);
    REQUIRE(descriptors.size() == 2);
    REQUIRE(descriptors[1].identity_evidence);
    CHECK(descriptors[0].name == descriptors[1].name);
    CHECK(descriptors[0].identity_evidence->type() != descriptors[1].identity_evidence->type());
    IsolatedPluginScanner worker(PULP_CLAP_IDENTITY_WORKER);
    auto scanned = worker.scan(path, 10000);
    REQUIRE(scanned.status == ScanStatus::Ok);
    REQUIRE(scanned.descriptors.size() == 2);
    REQUIRE(scanned.descriptors[1].identity_evidence);
    CHECK(scanned.descriptors[1].identity_evidence->artifact() ==
          descriptors[1].identity_evidence->artifact());
    HostScanCache cache;
    cache.put(path, descriptors[1]);
    HostScanCache restored;
    REQUIRE(restored.from_json(cache.to_json()));
    auto cached = restored.get(path);
    REQUIRE(cached);
    REQUIRE(cached->identity_evidence);
    CHECK(cached->identity_evidence->verification() == PluginIdentityVerification::declared);
    CHECK(cached->identity_evidence->artifact() == descriptors[1].identity_evidence->artifact());
    // Same path/mtime/size changed bytes cannot turn a cache capture into loaded evidence.
    const auto stamp = fs::last_write_time(path);
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        file.seekg(-1, std::ios::end);
        const auto byte = file.get();
        file.seekp(-1, std::ios::end);
        file.put(static_cast<char>(byte ^ 0x7f));
    }
    fs::last_write_time(path, stamp);
    cached = restored.get(path);
    REQUIRE(cached);
    CHECK(cached->identity_evidence->verification() == PluginIdentityVerification::declared);
    CHECK(cached->identity_evidence->artifact().sha256 !=
          *pulp::runtime::sha256_file_hex(path, 1ULL << 30));
    PluginInfo old;
    old.format = PluginFormat::CLAP;
    old.path = path;
    old.unique_id = second_id;
    cache.put(path, old);
    REQUIRE(restored.from_json(cache.to_json()));
    CHECK_FALSE(restored.get(path)->identity_evidence);
}
