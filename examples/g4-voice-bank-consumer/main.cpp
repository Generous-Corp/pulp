#include <pulp/host/voice_template.hpp>

#include <iostream>
#include <stdexcept>
#include <string>

using namespace pulp::host;

namespace {

void check(bool condition, const char* rule) {
    if (!condition)
        throw std::runtime_error(rule);
}

VoiceTemplateSchema schema() {
    VoiceTemplateSchema value;
    value.template_namespace = "com.pulp";
    value.template_name = "installed-voice";
    value.template_revision = 2;
    value.bank_schema_version = 1;
    value.bindings = {
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
    return value;
}

VoiceBankState state_for(const VoiceBankManifest& manifest, float value) {
    VoiceBankState state;
    state.manifest_sha256 = manifest.sha256;
    state.parameters.push_back({3, {"filter", "cutoff"}, "value", value});
    return state;
}

} // namespace

int main() {
    try {
        const auto prepared = prepare_voice_bank(schema());
        check(prepared.valid, "prepare");

        const auto saved = state_for(prepared.manifest, 0.25f);
        const auto saved_receipt = encode_voice_bank_state(prepared.manifest, saved);
        check(saved_receipt.valid, "save");

        VoiceBankState loaded;
        check(decode_voice_bank_state(saved_receipt.encoding, loaded), "load");
        const auto loaded_receipt = encode_voice_bank_state(prepared.manifest, loaded);
        check(loaded_receipt.valid && loaded_receipt.encoding == saved_receipt.encoding,
              "canonical-load-bytes");

        auto live = state_for(prepared.manifest, 0.5f);
        const auto edited = state_for(prepared.manifest, 0.75f);
        check(restore_voice_bank_state(prepared.manifest, saved, live), "undo");
        check(live.parameters.front().value == 0.25f, "undo-value");
        check(restore_voice_bank_state(prepared.manifest, edited, live), "redo");
        check(live.parameters.front().value == 0.75f, "redo-value");

        auto refused = schema();
        refused.bindings.front().shared_across_slots = true;
        check(!prepare_voice_bank(refused).valid, "shared-binding-refusal");
        auto forged = prepared.manifest;
        forged.bindings.front().spec.shared_across_slots = true;
        check(!encode_voice_bank_state(forged, edited).valid, "forged-shared-refusal");

        std::cout << "g4-voice-bank-public-consumer PASS checks=10\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "g4-voice-bank-public-consumer FAIL rule=" << error.what() << '\n';
        return 1;
    }
}
