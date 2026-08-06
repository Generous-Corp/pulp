#include <pulp/interchange/export_plan.hpp>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace pulp::interchange {
namespace {

constexpr std::string_view kLossManifestArtifact = "pulp-loss-manifest.json";

void write_json_string(std::ostringstream& out, std::string_view text) {
    out << '"';
    for (const unsigned char byte : text) {
        switch (byte) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (byte < 0x20u) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<unsigned int>(byte) << std::dec;
            } else {
                out << static_cast<char>(byte);
            }
        }
    }
    out << '"';
}

} // namespace

std::string loss_manifest_json(const ExportPlan& plan) {
    std::ostringstream out;
    out << "{\"schema_version\":1,\"format\":";
    write_json_string(out, format_id(plan.format()));
    out << ",\"lossless\":" << (plan.is_lossless() ? "true" : "false")
        << ",\"losses\":[";
    bool first = true;
    for (const LossEntry& entry : plan.losses().entries()) {
        if (!first)
            out << ',';
        first = false;
        out << "{\"concept\":";
        write_json_string(out, concept_id(entry.concept_value));
        out << ",\"level\":";
        write_json_string(out, export_level_id(entry.level));
        out << ",\"class\":";
        write_json_string(out, loss_class_id(entry.loss_class));
        if (entry.degraded_to) {
            out << ",\"degraded_to\":";
            write_json_string(out, concept_id(*entry.degraded_to));
        }
        out << ",\"count\":";
        write_json_string(out, std::to_string(entry.count));
        out << ",\"owners\":[";
        bool first_owner = true;
        for (const timeline::ItemId owner : entry.owners) {
            if (!first_owner)
                out << ',';
            first_owner = false;
            write_json_string(out, std::to_string(owner.value));
        }
        out << "],\"detail\":";
        write_json_string(out, entry.detail);
        out << '}';
    }
    out << "]}";
    return out.str();
}

namespace {

std::vector<std::uint8_t> loss_manifest_bytes(const ExportPlan& plan) {
    const auto text = loss_manifest_json(plan);
    return {text.begin(), text.end()};
}

} // namespace

std::vector<Concept> ExportPlan::required_consent() const {
    std::vector<Concept> concepts;
    concepts.reserve(losses_.entries().size());
    for (const LossEntry& entry : losses_.entries())
        concepts.push_back(entry.concept_value);
    return concepts;
}

ExportPlan plan_export(const timeline::Project& project, Format format,
                       const CensusLimits& limits) {
    ExportPlan plan(project, format);
    plan.census_ = census(plan.project_, limits);

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
run_export(const ExportPlan& plan, const ExportOptions& options,
           const FormatBoundExportWriter& writer) {
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

    if (writer.format() != plan.format()) {
        std::string message = "writer for ";
        message += format_display_name(writer.format());
        message += " cannot execute an export plan for ";
        message += format_display_name(plan.format());
        return runtime::Err(ExportError{ExportErrorCode::WriterFormatMismatch,
                                        std::move(message), {}});
    }

    auto result = writer.function_(plan);
    if (!result)
        return result;

    for (const ExportArtifact& artifact : result.value().artifacts) {
        if (artifact.name == kLossManifestArtifact) {
            return runtime::Err(ExportError{
                ExportErrorCode::WriterFailed,
                "writer returned the reserved artifact name pulp-loss-manifest.json", {}});
        }
    }
    result.value().artifacts.push_back(
        {std::string(kLossManifestArtifact), loss_manifest_bytes(plan)});
    return result;
}

runtime::Result<ExportArtifacts, ExportError>
run_export(const ExportPlan& plan, const ExportOptions& options, const ExportWriter& writer) {
    // Preserve the released callable writer seam. It predates format binding,
    // so the caller's plan supplies the format while the same consent and
    // canonical-manifest path remains mandatory.
    return run_export(plan, options, FormatBoundExportWriter{plan.format(), writer});
}

} // namespace pulp::interchange
