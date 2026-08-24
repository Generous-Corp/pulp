#include <catch2/catch_test_macros.hpp>

#include "reload_test_support.hpp"

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/sample_bank.hpp>
#include <pulp/runtime/crypto.hpp>

#include <filesystem>
#include <fstream>

namespace {

struct TempDirectory {
    std::filesystem::path path = pulp::test::unique_tmp_dir("pulp-sample-bank-");
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

std::string file_hash(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                    std::istreambuf_iterator<char>());
    return pulp::runtime::sha256_hex(bytes.data(), bytes.size());
}

pulp::audio::SampleBankManifest valid_manifest(const std::string& hash) {
    pulp::audio::SampleBankManifest manifest;
    manifest.id = "generic-audio-bank";
    manifest.name = "Generic Audio Bank";
    manifest.samples.push_back({7, "audio/tone.wav", hash, "license.txt"});
    pulp::audio::SampleZone zone;
    zone.sample_id = 7;
    zone.lowest_note = 12;
    zone.highest_note = 96;
    manifest.zones.push_back({zone, "{}"});
    return manifest;
}

} // namespace

TEST_CASE("sample-bank v1 JSON is strict and canonical", "[audio][content][sample-bank]") {
    const std::string hash(64, 'a');
    const auto written = pulp::audio::write_sample_bank_json(valid_manifest(hash));
    REQUIRE(written.valid());
    const auto parsed = pulp::audio::parse_sample_bank_json(written.json);
    REQUIRE(parsed.valid());
    REQUIRE(parsed.manifest.schema == pulp::audio::kSampleBankSchema);
    REQUIRE(parsed.manifest.samples[0].path == "audio/tone.wav");
    REQUIRE(parsed.manifest.zones[0].zone.sample_id == 7);
    REQUIRE(pulp::audio::write_sample_bank_json(parsed.manifest).json == written.json);

    auto precise = valid_manifest(hash);
    precise.zones[0].zone.tune_semitones = 0.1234567890123456;
    const auto precise_json = pulp::audio::write_sample_bank_json(precise);
    REQUIRE(precise_json.valid());
    REQUIRE(pulp::audio::parse_sample_bank_json(precise_json.json)
                .manifest.zones[0]
                .zone.tune_semitones == precise.zones[0].zone.tune_semitones);

    auto extensible = valid_manifest(hash);
    extensible.extensions_json = R"({"vendor.bank.v1":{"author":"Pulp"}})";
    extensible.samples[0].extensions_json = R"({"pulp.sample-edit.v1":{"trim":[1,3],"fade_in":2}})";
    extensible.zones[0].extensions_json =
        R"({"pulp.zone-playback.v1":{"mode":"ping_pong","reverse_entry":true}})";
    const auto extended_json = pulp::audio::write_sample_bank_json(extensible);
    REQUIRE(extended_json.valid());
    const auto extended_round_trip = pulp::audio::parse_sample_bank_json(extended_json.json);
    REQUIRE(extended_round_trip.valid());
    REQUIRE(pulp::audio::write_sample_bank_json(extended_round_trip.manifest).json ==
            extended_json.json);
    REQUIRE(extended_round_trip.manifest.samples[0].extensions_json.find(
                "\"pulp.sample-edit.v1\"") != std::string::npos);
    REQUIRE(extended_round_trip.manifest.zones[0].extensions_json.find(
                "\"pulp.zone-playback.v1\"") != std::string::npos);
    extensible.extensions_json = R"({".invalid":true})";
    REQUIRE(pulp::audio::write_sample_bank_json(extensible).status ==
            pulp::audio::SampleBankStatus::wrong_type);

    auto bad = written.json;
    bad.insert(bad.find("\"name\""), "\"unexpected\":true,");
    REQUIRE(pulp::audio::parse_sample_bank_json(bad).status ==
            pulp::audio::SampleBankStatus::unknown_field);

    bad = written.json;
    bad.replace(bad.find("audio/tone.wav"), std::string("audio/tone.wav").size(), "../tone.wav");
    REQUIRE(pulp::audio::parse_sample_bank_json(bad).status ==
            pulp::audio::SampleBankStatus::invalid_path);

    bad = written.json;
    bad.replace(bad.find("\"id\":\"generic-audio-bank\""),
                std::string("\"id\":\"generic-audio-bank\"").size(),
                "\"id\":\"first\",\"id\":\"second\"");
    REQUIRE(pulp::audio::parse_sample_bank_json(bad).status ==
            pulp::audio::SampleBankStatus::duplicate_field);

    auto duplicate = valid_manifest(hash);
    duplicate.samples.push_back(duplicate.samples[0]);
    REQUIRE(pulp::audio::write_sample_bank_json(duplicate).status ==
            pulp::audio::SampleBankStatus::duplicate_sample_id);

    duplicate = valid_manifest(hash);
    duplicate.samples.push_back({8, duplicate.samples[0].path, hash, "", "{}"});
    REQUIRE(pulp::audio::write_sample_bank_json(duplicate).status ==
            pulp::audio::SampleBankStatus::duplicate_sample_path);

    auto invalid_range = valid_manifest(hash);
    invalid_range.zones[0].zone.lowest_note = 100;
    invalid_range.zones[0].zone.highest_note = 20;
    REQUIRE(pulp::audio::write_sample_bank_json(invalid_range).status ==
            pulp::audio::SampleBankStatus::invalid_zone);

    auto missing = valid_manifest(hash);
    missing.zones[0].zone.sample_id = 99;
    REQUIRE(pulp::audio::write_sample_bank_json(missing).status ==
            pulp::audio::SampleBankStatus::missing_sample_reference);

    std::string too_large(pulp::audio::kSampleBankMaxJsonBytes + 1, ' ');
    REQUIRE(pulp::audio::parse_sample_bank_json(too_large).status ==
            pulp::audio::SampleBankStatus::resource_limit_exceeded);

    std::string too_many_samples =
        R"({"schema":"pulp.sample-bank.v1","id":"many","name":"Many","samples":[)";
    for (std::uint32_t i = 0; i <= pulp::audio::kSampleBankMaxSamples; ++i) {
        if (i != 0) too_many_samples += ",";
        too_many_samples += "{}";
    }
    too_many_samples += R"(],"zones":[]})";
    const auto cardinality = pulp::audio::parse_sample_bank_json(too_many_samples);
    REQUIRE(cardinality.status ==
            pulp::audio::SampleBankStatus::resource_limit_exceeded);
    REQUIRE(cardinality.field_path == "$.samples");

    auto programmatic = valid_manifest(hash);
    programmatic.samples.resize(pulp::audio::kSampleBankMaxSamples + 1);
    REQUIRE(pulp::audio::write_sample_bank_json(programmatic).status ==
            pulp::audio::SampleBankStatus::resource_limit_exceeded);
    programmatic = valid_manifest(hash);
    programmatic.zones.resize(pulp::audio::kSampleBankMaxZones + 1);
    REQUIRE(pulp::audio::write_sample_bank_json(programmatic).status ==
            pulp::audio::SampleBankStatus::resource_limit_exceeded);
}

TEST_CASE("sample-bank materializer verifies bytes and publishes stable views",
          "[audio][content][sample-bank]") {
    TempDirectory temp;
    std::filesystem::create_directories(temp.path / "audio");
    pulp::audio::AudioFileData audio;
    audio.sample_rate = 48000;
    audio.channels = {{0.0f, 0.25f, -0.25f, 0.5f}, {0.5f, -0.25f, 0.25f, 0.0f}};
    const auto wav = temp.path / "audio/tone.wav";
    REQUIRE(pulp::audio::write_wav_file(wav.string(), audio, pulp::audio::WavBitDepth::Float32));

    auto manifest = valid_manifest(file_hash(wav));
    pulp::audio::SampleBankMaterializePolicy policy;
    policy.sample_policy.max_channels = 2;
    policy.sample_policy.max_frames = 32;
    policy.sample_policy.max_decoded_bytes = 1024;
    policy.target_sample_rate = 96000;
    const auto loaded =
        pulp::audio::SampleBankMaterializer{}.materialize(manifest, temp.path, policy);
    REQUIRE(loaded.valid());
    REQUIRE(loaded.bank->sample_pool().resolve(7).valid);
    REQUIRE(loaded.bank->zone_map().size() == 1);
    REQUIRE(loaded.bank->sample_asset(7) == nullptr);
    const auto selection = pulp::audio::ZoneSelector::select(
        loaded.bank->zone_map(), {.note = 60, .velocity = 100, .host_sample_rate = 96000});
    REQUIRE(selection.valid);
    REQUIRE(selection.playback_rate == 0.5);

    policy.residency = pulp::audio::SampleBankMaterializePolicy::Residency::sample_assets;
    const auto asset_loaded =
        pulp::audio::SampleBankMaterializer{}.materialize(manifest, temp.path, policy);
    REQUIRE(asset_loaded.valid());
    REQUIRE(asset_loaded.bank->sample_pool().empty());
    const auto* asset = asset_loaded.bank->sample_asset(7);
    REQUIRE(asset != nullptr);
    REQUIRE(asset->valid());
    REQUIRE(asset->fully_resident());
    REQUIRE(asset->channels == 2);
    REQUIRE(asset->total_frames == 4);
    REQUIRE(asset->sample_rate == 48000);

    // Eight decoded floats require 32 bytes. Include the importer buffer in
    // addition to one retained asset copy or two published pool slots.
    policy.max_total_decoded_bytes = 63;
    REQUIRE(pulp::audio::SampleBankMaterializer{}.materialize(manifest, temp.path, policy).status ==
            pulp::audio::SampleBankStatus::resource_limit_exceeded);
    policy.residency = pulp::audio::SampleBankMaterializePolicy::Residency::published_pool;
    policy.max_total_decoded_bytes = 95;
    REQUIRE(pulp::audio::SampleBankMaterializer{}.materialize(manifest, temp.path, policy).status ==
            pulp::audio::SampleBankStatus::resource_limit_exceeded);
    policy.residency = pulp::audio::SampleBankMaterializePolicy::Residency::sample_assets;
    policy.max_total_decoded_bytes = 1;
    const auto over_budget =
        pulp::audio::SampleBankMaterializer{}.materialize(manifest, temp.path, policy);
    REQUIRE(over_budget.status == pulp::audio::SampleBankStatus::resource_limit_exceeded);
    policy.max_total_decoded_bytes = 1024;

    manifest.samples[0].sha256 = std::string(64, '0');
    const auto rejected =
        pulp::audio::SampleBankMaterializer{}.materialize(manifest, temp.path, policy);
    REQUIRE(rejected.status == pulp::audio::SampleBankStatus::hash_mismatch);

    TempDirectory outside;
    const auto outside_wav = outside.path / "outside.wav";
    REQUIRE(pulp::audio::write_wav_file(outside_wav.string(), audio));
    std::error_code symlink_error;
    std::filesystem::create_symlink(outside_wav, temp.path / "audio/escape.wav", symlink_error);
    REQUIRE_FALSE(symlink_error);
    manifest.samples[0].path = "audio/escape.wav";
    manifest.samples[0].sha256 = file_hash(outside_wav);
    const auto escaped =
        pulp::audio::SampleBankMaterializer{}.materialize(manifest, temp.path, policy);
    REQUIRE(escaped.status == pulp::audio::SampleBankStatus::path_escape);
}

TEST_CASE("sample-bank content validation classifies packs and paths precisely",
          "[audio][content][sample-bank]") {
    TempDirectory temp;
    std::filesystem::create_directories(temp.path / "banks");
    std::filesystem::create_directories(temp.path / "audio");

    pulp::audio::AudioFileData audio;
    audio.sample_rate = 48000;
    audio.channels = {{0.0f, 0.25f, -0.25f, 0.5f}};
    const auto wav = temp.path / "audio/tone.wav";
    REQUIRE(pulp::audio::write_wav_file(wav.string(), audio, pulp::audio::WavBitDepth::Float32));

    const auto written = pulp::audio::write_sample_bank_json(valid_manifest(file_hash(wav)));
    REQUIRE(written.valid());
    {
        std::ofstream out(temp.path / "banks/main.bank.json", std::ios::binary);
        out << written.json;
    }

    SECTION("a pack exporting no banks is valid even without a usable root") {
        // A pack carrying only presets or themes never opted into a bank check
        // and must not be failed by one, whatever root the caller passes. A
        // regular file is the clearest non-directory root.
        const std::vector<std::string> none;
        const auto result =
            pulp::audio::validate_sample_bank_content(temp.path / "banks/main.bank.json", none);
        REQUIRE(result.valid());
        REQUIRE(result.issues.empty());
        REQUIRE(result.manifest_paths.empty());
    }

    SECTION("a directory export parses bank manifests and skips companions") {
        // Both `.bank` and `.bank.json` name a manifest; anything else under
        // the exported directory is data or documentation, not a manifest.
        {
            std::ofstream out(temp.path / "banks/legacy.bank", std::ios::binary);
            out << written.json;
        }
        {
            std::ofstream out(temp.path / "banks/README.md", std::ios::binary);
            out << "Documentation, not a bank manifest.\n";
        }
        const auto colocated = temp.path / "banks/extra.wav";
        REQUIRE(pulp::audio::write_wav_file(colocated.string(), audio));

        const std::vector<std::string> exports{"banks"};
        const auto result = pulp::audio::validate_sample_bank_content(temp.path, exports);
        REQUIRE(result.valid());
        REQUIRE(result.manifest_paths.size() == 2);
        REQUIRE(result.manifest_paths[0] == "banks/legacy.bank");
        REQUIRE(result.manifest_paths[1] == "banks/main.bank.json");
        REQUIRE(result.sample_paths.size() == 1);
        REQUIRE(result.sample_paths[0] == "audio/tone.wav");
    }

    SECTION("a missing sample reports a filesystem error, not a path escape") {
        auto manifest = valid_manifest(std::string(64, 'b'));
        manifest.samples[0].path = "audio/absent.wav";
        const auto absent = pulp::audio::write_sample_bank_json(manifest);
        REQUIRE(absent.valid());
        {
            std::ofstream out(temp.path / "banks/absent.bank.json", std::ios::binary);
            out << absent.json;
        }
        const std::vector<std::string> exports{"banks/absent.bank.json"};
        const auto result = pulp::audio::validate_sample_bank_content(temp.path, exports);
        REQUIRE_FALSE(result.valid());
        REQUIRE(result.issues.size() == 1);
        REQUIRE(result.issues[0].status == pulp::audio::SampleBankStatus::filesystem_error);
        REQUIRE(result.issues[0].field_path == "audio/absent.wav");
    }
}

TEST_CASE("sample-bank rejects the no-slice sentinel so slices round-trip",
          "[audio][content][sample-bank]") {
    auto manifest = valid_manifest(std::string(64, 'a'));
    manifest.zones[0].zone.slice_index = pulp::audio::kNoSampleSliceIndex - 1;
    manifest.zones[0].zone.slice_region.start_frame = 0;
    manifest.zones[0].zone.slice_region.end_frame = 4;

    const auto written = pulp::audio::write_sample_bank_json(manifest);
    REQUIRE(written.valid());
    const auto accepted = pulp::audio::parse_sample_bank_json(written.json);
    REQUIRE(accepted.valid());
    REQUIRE(accepted.manifest.zones[0].zone.slice_index ==
            pulp::audio::kNoSampleSliceIndex - 1);
    REQUIRE(pulp::audio::write_sample_bank_json(accepted.manifest).json == written.json);

    // The writer omits the slice triple for kNoSampleSliceIndex, so accepting
    // the sentinel on parse would let a manifest round-trip to different JSON.
    const auto highest = std::to_string(pulp::audio::kNoSampleSliceIndex - 1);
    const auto sentinel = std::to_string(pulp::audio::kNoSampleSliceIndex);
    auto json = written.json;
    const auto at = json.find(highest);
    REQUIRE(at != std::string::npos);
    json.replace(at, highest.size(), sentinel);
    const auto rejected = pulp::audio::parse_sample_bank_json(json);
    REQUIRE_FALSE(rejected.valid());
    REQUIRE(rejected.status == pulp::audio::SampleBankStatus::invalid_zone);
}

TEST_CASE("sample-bank refuses manifests it could not write back unchanged",
          "[audio][content][sample-bank]") {
    const std::string hash(64, 'a');

    SECTION("rich loop playback metadata is rejected rather than flattened") {
        // v1 serializes only the loop frame range. Silently dropping the rest
        // would make write() lossy and break parse(write(m)) == m.
        auto manifest = valid_manifest(hash);
        manifest.zones[0].zone.has_loop = true;
        manifest.zones[0].zone.loop.start_frame = 0;
        manifest.zones[0].zone.loop.end_frame = 4;
        REQUIRE(pulp::audio::write_sample_bank_json(manifest).valid());

        manifest.zones[0].zone.loop.playback_mode = pulp::audio::LoopPlaybackMode::PingPong;
        auto written = pulp::audio::write_sample_bank_json(manifest);
        REQUIRE_FALSE(written.valid());
        REQUIRE(written.status == pulp::audio::SampleBankStatus::invalid_zone);

        manifest.zones[0].zone.loop.playback_mode = pulp::audio::LoopPlaybackMode::Forward;
        manifest.zones[0].zone.loop.crossfade_frames = 512;
        REQUIRE_FALSE(pulp::audio::write_sample_bank_json(manifest).valid());

        manifest.zones[0].zone.loop.crossfade_frames = 0;
        manifest.zones[0].zone.loop.reverse_entry = true;
        REQUIRE_FALSE(pulp::audio::write_sample_bank_json(manifest).valid());
    }

    SECTION("a slice marker index is rejected rather than flattened") {
        auto manifest = valid_manifest(hash);
        manifest.zones[0].zone.slice_index = 0;
        manifest.zones[0].zone.slice_region.start_frame = 0;
        manifest.zones[0].zone.slice_region.end_frame = 4;
        REQUIRE(pulp::audio::write_sample_bank_json(manifest).valid());

        manifest.zones[0].zone.slice_region.marker_index = 3;
        auto written = pulp::audio::write_sample_bank_json(manifest);
        REQUIRE_FALSE(written.valid());
        REQUIRE(written.status == pulp::audio::SampleBankStatus::invalid_zone);
    }

    SECTION("non-UTF-8 text is rejected so the writer cannot emit unparseable JSON") {
        // A Latin-1 filename would otherwise be written through unescaped and
        // then rejected by the parser as invalid_json on the way back in.
        const std::string latin1_name = std::string("Caf") + char(0xE9);

        auto named = valid_manifest(hash);
        named.name = latin1_name;
        auto written = pulp::audio::write_sample_bank_json(named);
        REQUIRE_FALSE(written.valid());
        REQUIRE(written.status == pulp::audio::SampleBankStatus::invalid_identifier);

        auto pathed = valid_manifest(hash);
        pathed.samples[0].path = std::string("audio/caf") + char(0xE9) + ".wav";
        REQUIRE(pulp::audio::write_sample_bank_json(pathed).status ==
                pulp::audio::SampleBankStatus::invalid_path);

        // A well-formed multi-byte name stays legal and round-trips.
        auto utf8 = valid_manifest(hash);
        utf8.name = "Caf\xC3\xA9 Bank";
        const auto ok = pulp::audio::write_sample_bank_json(utf8);
        REQUIRE(ok.valid());
        const auto parsed = pulp::audio::parse_sample_bank_json(ok.json);
        REQUIRE(parsed.valid());
        REQUIRE(parsed.manifest.name == "Caf\xC3\xA9 Bank");
    }

    SECTION("numbers that would serialize in scientific notation are rejected") {
        // std::to_chars emits `e+NN` for large magnitudes, which the JSON
        // parser does not accept.
        auto manifest = valid_manifest(hash);
        manifest.zones[0].zone.tune_semitones = 1e20;
        REQUIRE_FALSE(pulp::audio::write_sample_bank_json(manifest).valid());

        manifest.zones[0].zone.tune_semitones = 0.0;
        manifest.zones[0].zone.keytrack_cents_per_key = 1e18;
        REQUIRE_FALSE(pulp::audio::write_sample_bank_json(manifest).valid());
    }
}

namespace {

std::string bank_json(std::string_view samples, std::string_view zones,
                      std::string_view schema = "pulp.sample-bank.v1",
                      std::string_view id = "bank", std::string_view name = "Bank") {
    return std::string("{\"schema\":\"") + std::string(schema) + "\",\"id\":\"" +
           std::string(id) + "\",\"name\":\"" + std::string(name) + "\",\"samples\":[" +
           std::string(samples) + "],\"zones\":[" + std::string(zones) + "]}";
}

void expect_reject(const std::string& json, pulp::audio::SampleBankStatus status) {
    INFO("json=" << json);
    const auto parsed = pulp::audio::parse_sample_bank_json(json);
    REQUIRE_FALSE(parsed.valid());
    REQUIRE(parsed.status == status);
}

} // namespace

TEST_CASE("sample-bank parser rejects malformed manifests by category",
          "[audio][content][sample-bank]") {
    using Status = pulp::audio::SampleBankStatus;
    const std::string hash(64, 'a');
    const auto sample = std::string(R"({"id":7,"path":"audio/tone.wav","sha256":")") + hash + "\"}";
    const std::string zone = R"({"sample_id":7})";

    SECTION("shape") {
        expect_reject("[]", Status::root_not_object);
        expect_reject("{", Status::invalid_json);
        expect_reject(R"({"schema":"pulp.sample-bank.v1","id":"b","name":"B","zones":[]})",
                      Status::missing_field);
        auto unknown = bank_json(sample, zone);
        unknown.insert(unknown.size() - 1, R"(,"surprise":1)");
        expect_reject(unknown, Status::unknown_field);
        expect_reject(R"({"schema":1,"id":"b","name":"B","samples":[],"zones":[]})",
                      Status::wrong_type);
        expect_reject(R"({"schema":"pulp.sample-bank.v1","id":"b","name":"B","samples":{},"zones":[]})",
                      Status::wrong_type);
    }

    SECTION("identity") {
        expect_reject(bank_json(sample, zone, "pulp.sample-bank.v2"), Status::unsupported_schema);
        expect_reject(bank_json(sample, zone, "pulp.sample-bank.v1", "bad id!"),
                      Status::invalid_identifier);
        expect_reject(bank_json(sample, zone, "pulp.sample-bank.v1", ".."),
                      Status::invalid_identifier);
        expect_reject(bank_json(sample, zone, "pulp.sample-bank.v1", "bank", ""),
                      Status::invalid_identifier);
    }

    SECTION("sample paths stay relative and confined") {
        // The backslash is doubled so the JSON text carries a literal `\`,
        // rather than JSON unescaping it into a control character.
        for (const auto* path : {"/etc/passwd.wav", "C:/audio/tone.wav", "../escape.wav",
                                 "audio/../../escape.wav", "audio\\\\tone.wav", "./tone.wav"}) {
            const auto bad =
                std::string(R"({"id":7,"path":")") + path + R"(","sha256":")" + hash + "\"}";
            expect_reject(bank_json(bad, zone), Status::invalid_path);
        }
    }

    SECTION("hashes are lowercase 64-hex") {
        const auto short_hash =
            std::string(R"({"id":7,"path":"audio/tone.wav","sha256":"abc"})");
        expect_reject(bank_json(short_hash, zone), Status::invalid_hash);
        const auto upper = std::string(R"({"id":7,"path":"audio/tone.wav","sha256":")") +
                           std::string(64, 'A') + "\"}";
        expect_reject(bank_json(upper, zone), Status::invalid_hash);
    }

    SECTION("sample identity") {
        const auto zero = std::string(R"({"id":0,"path":"audio/tone.wav","sha256":")") + hash + "\"}";
        expect_reject(bank_json(zero, zone), Status::invalid_sample_id);
        const auto other =
            std::string(R"({"id":7,"path":"audio/other.wav","sha256":")") + hash + "\"}";
        expect_reject(bank_json(sample + "," + other, zone), Status::duplicate_sample_id);
        const auto same_path =
            std::string(R"({"id":8,"path":"audio/tone.wav","sha256":")") + hash + "\"}";
        expect_reject(bank_json(sample + "," + same_path, zone), Status::duplicate_sample_path);
        expect_reject(bank_json(sample, R"({"sample_id":99})"), Status::missing_sample_reference);
    }

    SECTION("zone ranges and typed fields") {
        expect_reject(bank_json(sample, R"({"sample_id":7,"root_note":200})"), Status::invalid_zone);
        expect_reject(bank_json(sample, R"({"sample_id":7,"lowest_note":90,"highest_note":10})"),
                      Status::invalid_zone);
        expect_reject(bank_json(sample, R"({"sample_id":7,"lowest_velocity":0})"),
                      Status::invalid_zone);
        expect_reject(bank_json(sample, R"({"sample_id":7.5})"), Status::wrong_type);
        expect_reject(bank_json(sample, R"({"sample_id":true})"), Status::wrong_type);
        // A slice or loop must arrive complete; a partial triple is a missing field.
        expect_reject(bank_json(sample, R"({"sample_id":7,"slice_index":0})"),
                      Status::missing_field);
        expect_reject(bank_json(sample, R"({"sample_id":7,"loop_start_frame":0})"),
                      Status::missing_field);
    }

    SECTION("extensions must be a namespaced object") {
        auto bad = bank_json(sample, zone);
        bad.insert(bad.size() - 1, R"(,"extensions":[])");
        expect_reject(bad, Status::wrong_type);
    }
}

TEST_CASE("sha256_file_hex streams within an explicit bound",
          "[runtime][crypto][sample-bank]") {
    TempDirectory temp;
    const auto path = temp.path / "payload.bin";

    // Empty file digests to the well-known empty SHA-256.
    { std::ofstream out(path, std::ios::binary); }
    const auto empty = pulp::runtime::sha256_file_hex(path, 0);
    REQUIRE(empty.has_value());
    REQUIRE(*empty == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // A file of exactly one read-buffer multiple must not be truncated.
    const std::string block(64 * 1024, 'x');
    { std::ofstream out(path, std::ios::binary); out << block; }
    const auto exact = pulp::runtime::sha256_file_hex(path, block.size());
    REQUIRE(exact.has_value());
    REQUIRE(*exact == pulp::runtime::sha256_hex(block));

    // One byte over the bound is refused rather than hashed short.
    REQUIRE_FALSE(pulp::runtime::sha256_file_hex(path, block.size() - 1).has_value());
    REQUIRE_FALSE(pulp::runtime::sha256_file_hex(path, 0).has_value());
    REQUIRE_FALSE(pulp::runtime::sha256_file_hex(temp.path / "absent.bin", 1024).has_value());
}

TEST_CASE("sample-bank materializer refuses unusable manifests and policies",
          "[audio][content][sample-bank]") {
    TempDirectory temp;
    std::filesystem::create_directories(temp.path / "audio");
    pulp::audio::AudioFileData audio;
    audio.sample_rate = 48000;
    audio.channels = {{0.0f, 0.25f, -0.25f, 0.5f}, {0.5f, -0.25f, 0.25f, 0.0f}};
    const auto wav = temp.path / "audio/tone.wav";
    REQUIRE(pulp::audio::write_wav_file(wav.string(), audio, pulp::audio::WavBitDepth::Float32));

    const auto manifest = valid_manifest(file_hash(wav));
    pulp::audio::SampleBankMaterializePolicy base;
    base.sample_policy.max_channels = 2;
    base.sample_policy.max_frames = 32;
    base.sample_policy.max_decoded_bytes = 1024;
    const pulp::audio::SampleBankMaterializer loader;

    SECTION("policy limits are honoured before any file is opened") {
        auto policy = base;
        policy.max_samples = 0;
        REQUIRE(loader.materialize(manifest, temp.path, policy).status ==
                pulp::audio::SampleBankStatus::resource_limit_exceeded);

        policy = base;
        policy.max_zones = 0;
        REQUIRE(loader.materialize(manifest, temp.path, policy).status ==
                pulp::audio::SampleBankStatus::resource_limit_exceeded);

        policy = base;
        policy.target_sample_rate = -1.0;
        REQUIRE(loader.materialize(manifest, temp.path, policy).status ==
                pulp::audio::SampleBankStatus::unsupported_materialization_policy);
    }

    SECTION("an unusable content root or sample is a filesystem error") {
        REQUIRE(loader.materialize(manifest, temp.path / "audio/tone.wav", base).status ==
                pulp::audio::SampleBankStatus::filesystem_error);
        REQUIRE(loader.materialize(manifest, temp.path / "missing-dir", base).status ==
                pulp::audio::SampleBankStatus::filesystem_error);

        auto absent = manifest;
        absent.samples[0].path = "audio/absent.wav";
        REQUIRE(loader.materialize(absent, temp.path, base).status ==
                pulp::audio::SampleBankStatus::filesystem_error);
    }

    SECTION("an encoded sample larger than the per-sample bound is refused") {
        auto policy = base;
        policy.max_encoded_bytes_per_sample = 8;
        REQUIRE(loader.materialize(manifest, temp.path, policy).status ==
                pulp::audio::SampleBankStatus::resource_limit_exceeded);
    }

    SECTION("the decoded-byte budget accumulates across samples") {
        // Two distinct files, same audio. The first sample must consume enough
        // of the budget that the second is refused — the loop-carried total is
        // what a single-sample test can never exercise.
        const auto second_wav = temp.path / "audio/tone2.wav";
        REQUIRE(pulp::audio::write_wav_file(second_wav.string(), audio,
                                            pulp::audio::WavBitDepth::Float32));
        auto two = manifest;
        two.samples.push_back({9, "audio/tone2.wav", file_hash(second_wav), ""});
        pulp::audio::SampleZone zone;
        zone.sample_id = 9;
        two.zones.push_back({zone, "{}"});

        auto policy = base;
        policy.residency = pulp::audio::SampleBankMaterializePolicy::Residency::published_pool;
        // Eight decoded floats = 32 bytes; published_pool budgets 3 live copies
        // per sample, so 96 admits exactly one of the two.
        policy.max_total_decoded_bytes = 150;
        REQUIRE(loader.materialize(two, temp.path, policy).status ==
                pulp::audio::SampleBankStatus::resource_limit_exceeded);

        policy.max_total_decoded_bytes = 1024;
        REQUIRE(loader.materialize(two, temp.path, policy).valid());
    }

    SECTION("resident sample assets cannot carry slices or loops") {
        auto looped = manifest;
        looped.zones[0].zone.has_loop = true;
        looped.zones[0].zone.loop.start_frame = 0;
        looped.zones[0].zone.loop.end_frame = 4;
        auto policy = base;
        policy.residency = pulp::audio::SampleBankMaterializePolicy::Residency::sample_assets;
        const auto result = loader.materialize(looped, temp.path, policy);
        REQUIRE(result.status ==
                pulp::audio::SampleBankStatus::unsupported_materialization_policy);
        REQUIRE(result.field_path == "$.zones[0]");
    }
}
