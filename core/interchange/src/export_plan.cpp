#include <pulp/interchange/export_plan.hpp>

#include <algorithm>
#include <string>

namespace pulp::interchange {

std::vector<Concept> ExportPlan::required_consent() const {
    std::vector<Concept> concepts;
    concepts.reserve(losses_.entries().size());
    for (const LossEntry& entry : losses_.entries())
        concepts.push_back(entry.concept_value);
    return concepts;
}

ExportPlan plan_export(const timeline::Project& project, Format format,
                       const CensusLimits& limits) {
    ExportPlan plan;
    plan.format_ = format;
    plan.census_ = census(project, limits);

    for (Concept concept_value : plan.census_.present()) {
        const ExportRow& row = export_capability(format, concept_value);
        if (row.level == ExportLevel::Full || row.level == ExportLevel::RoundtripOnly) {
            plan.representable_.push_back(concept_value);
            continue;
        }

        LossEntry entry;
        entry.concept_value = concept_value;
        entry.level = row.level;
        entry.loss_class = row.loss_class;
        if (row.level == ExportLevel::Degrade)
            entry.degraded_to = row.degrade_to;
        entry.count = plan.census_.count(concept_value);
        const std::span<const timeline::ItemId> owners = plan.census_.owners(concept_value);
        entry.owners.assign(owners.begin(), owners.end());
        entry.detail = row.loss;
        plan.losses_.add(std::move(entry));
    }

    return plan;
}

runtime::Result<ExportArtifacts, ExportError>
run_export(const ExportPlan& plan, const ExportOptions& options, const ExportWriter& writer) {
    // Consent is checked before anything else so a caller learns what an export
    // would cost even when no writer exists to perform it.
    std::vector<Concept> unaccepted;
    for (const LossEntry& entry : plan.losses().entries()) {
        const bool accepted = std::find(options.accepted_losses.begin(),
                                        options.accepted_losses.end(),
                                        entry.concept_value) != options.accepted_losses.end();
        if (!accepted)
            unaccepted.push_back(entry.concept_value);
    }
    if (!unaccepted.empty()) {
        std::string message = "export to ";
        message += format_display_name(plan.format());
        message += " loses concepts that were not accepted:";
        for (Concept concept_value : unaccepted) {
            message += " ";
            message += concept_id(concept_value);
        }
        return runtime::Err(ExportError{ExportErrorCode::UnacceptedLoss, std::move(message),
                                        std::move(unaccepted)});
    }

    if (!writer) {
        std::string message = "no writer is registered for ";
        message += format_display_name(plan.format());
        return runtime::Err(
            ExportError{ExportErrorCode::NoWriterRegistered, std::move(message), {}});
    }

    return writer(plan);
}

} // namespace pulp::interchange
