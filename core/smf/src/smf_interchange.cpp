#include <pulp/smf/interchange.hpp>

#include "smf_export_internal.hpp"

#include <pulp/timeline/smf.hpp>

#include <utility>

namespace pulp::smf {
namespace {

bool loses(const interchange::ExportPlan& plan, interchange::Concept concept_value) noexcept {
    return plan.losses().find(concept_value) != nullptr;
}

timeline::detail::SmfExportLossPolicy loss_policy(const interchange::ExportPlan& plan) {
    using interchange::Concept;
    return {
        .drop_media_clips = loses(plan, Concept::ClipMedia),
        .drop_registered_content = loses(plan, Concept::ContentRegistered),
        .drop_opaque_content = loses(plan, Concept::ContentOpaque),
        .drop_nested_sequences = loses(plan, Concept::SequenceNested),
        .drop_absolute_clips = loses(plan, Concept::ClipAbsolute),
        .strip_note_modifiers = loses(plan, Concept::ClipNoteModifier),
        .drop_midi_expression_lanes = loses(plan, Concept::ClipMidiExpressionLane),
        .quantize_note_velocity = loses(plan, Concept::ClipNoteVelocityQuantized),
        .drop_non_root_sequences = loses(plan, Concept::SequenceMultiple),
        .step_tempo_ramps = loses(plan, Concept::TempoRamp),
    };
}

} // namespace

interchange::FormatBoundExportWriter writer() {
    return {interchange::Format::Smf,
            [](const interchange::ExportPlan& plan)
                -> runtime::Result<interchange::ExportArtifacts, interchange::ExportError> {
        auto encoded = timeline::detail::export_smf_with_policy(
            plan.project(), timeline::SmfExportOptions{}, loss_policy(plan));
        if (!encoded) {
            return runtime::Err(interchange::ExportError{
                interchange::ExportErrorCode::WriterFailed,
                "Standard MIDI File writer failed: " + encoded.error().message, {}});
        }

        interchange::ExportArtifacts artifacts;
        artifacts.artifacts.push_back({"project.mid", std::move(encoded.value().bytes)});
        return runtime::Ok(std::move(artifacts));
    }};
}

} // namespace pulp::smf
