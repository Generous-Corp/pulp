#include <catch2/catch_test_macros.hpp>
#include <pulp/host/voice_template.hpp>

#include <cmath>
#include <limits>

using namespace pulp::host;

namespace {
VoiceTemplateSchema schema() {
    VoiceTemplateSchema s;
    s.template_namespace = "com.pulp";
    s.template_name = "voice";
    s.template_revision = 2;
    s.bank_schema_version = 1;
    s.bindings = {
        {{"filter", "cutoff"},
         "value",
         VoiceBindingTarget::Parameter,
         VoiceBindingType::Control,
         VoiceBindingDirection::Input,
         VoiceBindingShape::Scalar,
         VoiceBindingRate::Block,
         false,
         0},
        {{"audio", "out"},
         "main",
         VoiceBindingTarget::Port,
         VoiceBindingType::Audio,
         VoiceBindingDirection::Output,
         VoiceBindingShape::Stereo,
         VoiceBindingRate::Sample,
         false,
         0},
    };
    return s;
}

VoiceBankState state_for(const VoiceBankManifest& manifest, float value) {
    VoiceBankState state;
    state.manifest_sha256 = manifest.sha256;
    state.parameters.push_back({3, {"filter", "cutoff"}, "value", value});
    return state;
}
} // namespace

TEST_CASE("G4 voice template expands eight isolated stable bindings") {
    const auto prepared = prepare_voice_bank(schema());
    REQUIRE(prepared.valid);
    REQUIRE(prepared.manifest.bindings.size() == 16);
    CHECK_FALSE(prepared.manifest.sha256.empty());
    CHECK(prepared.manifest.bindings.front().slot_ordinal == 0);
    CHECK(prepared.manifest.bindings.back().slot_ordinal == 7);
    CHECK(prepared.manifest.bindings.front().identity.valid);

    auto reordered = schema();
    std::swap(reordered.bindings[0], reordered.bindings[1]);
    const auto second = prepare_voice_bank(reordered);
    REQUIRE(second.valid);
    CHECK(second.manifest.sha256 == prepared.manifest.sha256);
}

TEST_CASE("G4 voice template persistence restores atomically and supports edit undo") {
    const auto prepared = prepare_voice_bank(schema());
    REQUIRE(prepared.valid);
    auto before = state_for(prepared.manifest, 0.25f);
    auto receipt = encode_voice_bank_state(prepared.manifest, before);
    REQUIRE(receipt.valid);
    CHECK(receipt.encoding.front() == 0x01);
    CHECK(receipt.sha256.size() == 64);
    VoiceBankState decoded;
    REQUIRE(decode_voice_bank_state(receipt.encoding, decoded));
    CHECK(decoded.manifest_sha256 == before.manifest_sha256);
    REQUIRE(decoded.parameters.size() == 1);
    CHECK(decoded.parameters.front().path == before.parameters.front().path);
    CHECK(decoded.parameters.front().target == before.parameters.front().target);

    VoiceBankState live = state_for(prepared.manifest, 0.5f);
    REQUIRE(restore_voice_bank_state(prepared.manifest, before, live));
    CHECK(live.parameters.front().value == 0.25f);
    auto undo = live;
    live.parameters.front().value = 0.75f;
    REQUIRE(restore_voice_bank_state(prepared.manifest, undo, live));
    CHECK(live.parameters.front().value == 0.25f);
}

TEST_CASE(
    "G4 planted negatives refuse aliases, cross-slot mutation, mismatch, and failed migration") {
    auto duplicate = schema();
    duplicate.bindings.push_back(duplicate.bindings.front());
    auto rejected = prepare_voice_bank(duplicate);
    CHECK_FALSE(rejected.valid);

    auto ordinal_duplicate = schema();
    ordinal_duplicate.bindings.front().ordinal = 9;
    ordinal_duplicate.bindings.push_back(ordinal_duplicate.bindings.front());
    CHECK_FALSE(prepare_voice_bank(ordinal_duplicate).valid);

    auto alias = schema();
    alias.bindings[0].path[0] = "filte\xCC\x81r";
    alias.bindings.push_back(alias.bindings[0]);
    alias.bindings.back().path[0] = "filtér";
    CHECK_FALSE(prepare_voice_bank(alias).valid);

    auto shared = schema();
    shared.bindings[0].shared_across_slots = true;
    CHECK_FALSE(prepare_voice_bank(shared).valid);

    auto mismatch = schema();
    mismatch.bindings[1].type = VoiceBindingType::Event;
    mismatch.bindings[1].rate = VoiceBindingRate::Sample;
    CHECK_FALSE(prepare_voice_bank(mismatch).valid);
    mismatch = schema();
    mismatch.bindings[1].shape = VoiceBindingShape::Scalar;
    CHECK_FALSE(prepare_voice_bank(mismatch).valid);

    const auto prepared = prepare_voice_bank(schema());
    REQUIRE(prepared.valid);
    auto forged_manifest = prepared.manifest;
    forged_manifest.bindings.front().identity.sha256[0] =
        forged_manifest.bindings.front().identity.sha256[0] == '0' ? '1' : '0';
    auto forged_state = state_for(forged_manifest, 0.5f);
    CHECK_FALSE(encode_voice_bank_state(forged_manifest, forged_state).valid);
    auto live = state_for(prepared.manifest, 0.5f);
    const auto unchanged = live;
    auto failed = state_for(prepared.manifest, 0.8f);
    failed.manifest_sha256 = "stale";
    CHECK_FALSE(restore_voice_bank_state(prepared.manifest, failed, live));
    CHECK(live.parameters.front().value == unchanged.parameters.front().value);
    failed = state_for(prepared.manifest, 0.8f);
    failed.parameters.front().value = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(restore_voice_bank_state(prepared.manifest, failed, live));
    CHECK(live.parameters.front().value == unchanged.parameters.front().value);
    failed = state_for(prepared.manifest, 0.8f);
    failed.parameters.front().path[0] = "filte\xCC\x81r";
    failed.parameters.push_back(failed.parameters.front());
    failed.parameters.back().path[0] = "filtér";
    CHECK_FALSE(restore_voice_bank_state(prepared.manifest, failed, live));
    CHECK(live.parameters.front().value == unchanged.parameters.front().value);
}
