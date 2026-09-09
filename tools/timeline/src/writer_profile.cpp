#include <pulp/tools/timeline/writer_profile.hpp>

#include <pulp/timeline/schema_json.hpp>

#include <limits>

namespace pulp::tools::timeline {

namespace {

using pulp::timeline::quote_json_string;

constexpr std::size_t kUnbounded = std::numeric_limits<std::size_t>::max();

std::string quota_json(std::size_t value) {
    // An unbounded ceiling is emitted as null rather than as SIZE_MAX, which
    // exceeds the range JSON numbers carry exactly.
    return value == kUnbounded ? std::string{"null"} : std::to_string(value);
}

std::string command_id_json(pulp::timeline::CommandId id) {
    return "{\"sequence\":" + std::to_string(id.sequence) + ",\"writer\":" +
           std::to_string(id.writer.value) + "}";
}

std::string authority_json(pulp::timeline::CommandAuthority authority) {
    return "{\"class\":" + quote_json_string(command_class_name(authority.command_class)) +
           ",\"intent\":" + quote_json_string(command_intent_name(authority.intent)) + "}";
}

std::string_view conflict_code_message(pulp::timeline::ConflictCode code) noexcept {
    using Code = pulp::timeline::ConflictCode;
    switch (code) {
    case Code::CapabilityDenied:
        return "the writer profile does not permit this command class and intent";
    case Code::WriterQuotaExhausted:
        return "the transaction exceeds the writer profile's retained-byte quota";
    case Code::StaleRevision:
        return "the document advanced past the revision this transaction expected";
    default:
        break;
    }
    return "the timeline transaction was refused";
}

} // namespace

WriterProfile proposal_writer_profile() noexcept {
    WriterProfile profile;
    profile.kind = WriterProfileKind::Proposal;
    profile.mask = pulp::timeline::non_destructive_capabilities();
    profile.mask.max_transaction_retained_bytes = kProposalMaxTransactionRetainedBytes;
    profile.mask.max_session_retained_bytes = kProposalMaxSessionRetainedBytes;
    return profile;
}

WriterProfile editor_writer_profile() noexcept {
    WriterProfile profile;
    profile.kind = WriterProfileKind::Editor;
    profile.mask = pulp::timeline::unrestricted_capabilities();
    profile.mask.max_transaction_retained_bytes = kEditorMaxTransactionRetainedBytes;
    profile.mask.max_session_retained_bytes = kEditorMaxSessionRetainedBytes;
    return profile;
}

WriterProfile trusted_writer_profile() noexcept {
    WriterProfile profile;
    profile.kind = WriterProfileKind::Trusted;
    profile.mask = pulp::timeline::unrestricted_capabilities();
    return profile;
}

std::optional<WriterProfile> writer_profile_by_name(std::string_view name) noexcept {
    if (name == "proposal")
        return proposal_writer_profile();
    if (name == "editor")
        return editor_writer_profile();
    if (name == "trusted")
        return trusted_writer_profile();
    return std::nullopt;
}

std::string_view writer_profile_name(WriterProfileKind kind) noexcept {
    switch (kind) {
    case WriterProfileKind::Proposal:
        return "proposal";
    case WriterProfileKind::Editor:
        return "editor";
    case WriterProfileKind::Trusted:
        return "trusted";
    }
    return "proposal";
}

std::string_view selectable_writer_profile_names() noexcept {
    return "proposal|editor|trusted";
}

std::string_view command_class_name(pulp::timeline::CommandClass value) noexcept {
    using Class = pulp::timeline::CommandClass;
    switch (value) {
    case Class::Clip:
        return "clip";
    case Class::Note:
        return "note";
    case Class::Automation:
        return "automation";
    case Class::Track:
        return "track";
    case Class::Take:
        return "take";
    case Class::Scene:
        return "scene";
    case Class::Sequence:
        return "sequence";
    case Class::Device:
        return "device";
    case Class::Annotation:
        return "annotation";
    case Class::Timing:
        return "timing";
    case Class::Asset:
        return "asset";
    }
    return "unknown";
}

std::string_view command_intent_name(pulp::timeline::CommandIntent value) noexcept {
    using Intent = pulp::timeline::CommandIntent;
    switch (value) {
    case Intent::Create:
        return "create";
    case Intent::Modify:
        return "modify";
    case Intent::Remove:
        return "remove";
    }
    return "unknown";
}

std::string_view conflict_code_name(pulp::timeline::ConflictCode code) noexcept {
    using Code = pulp::timeline::ConflictCode;
    switch (code) {
    case Code::InvalidIdentifier:
        return "invalid_identifier";
    case Code::EmptyTransaction:
        return "empty_transaction";
    case Code::StaleRevision:
        return "stale_revision";
    case Code::TransactionIdCollision:
        return "transaction_id_collision";
    case Code::CommandIdCollision:
        return "command_id_collision";
    case Code::AlreadyAppliedResultExpired:
        return "already_applied_result_expired";
    case Code::TargetMissing:
        return "target_missing";
    case Code::WrongTargetKind:
        return "wrong_target_kind";
    case Code::InactiveTarget:
        return "inactive_target";
    case Code::GestureState:
        return "gesture_state";
    case Code::ParentMismatch:
        return "parent_mismatch";
    case Code::ExpectedValueMismatch:
        return "expected_value_mismatch";
    case Code::IdentityNotAvailable:
        return "identity_not_available";
    case Code::JournalFull:
        return "journal_full";
    case Code::UndoFull:
        return "undo_full";
    case Code::NothingToUndo:
        return "nothing_to_undo";
    case Code::NothingToRedo:
        return "nothing_to_redo";
    case Code::WriterLimit:
        return "writer_limit";
    case Code::SequenceExhausted:
        return "sequence_exhausted";
    case Code::ModelInvariant:
        return "model_invariant";
    case Code::JournalDurability:
        return "journal_durability";
    case Code::CheckpointMismatch:
        return "checkpoint_mismatch";
    case Code::ReplayDivergence:
        return "replay_divergence";
    case Code::Unspecified:
        return "unspecified";
    case Code::CapabilityDenied:
        return "capability_denied";
    case Code::WriterQuotaExhausted:
        return "writer_quota_exhausted";
    }
    return "unspecified";
}

std::string writer_capability_json(const pulp::timeline::WriterCapabilityMask& mask) {
    std::string json = "{\"classes\":[";
    bool first_class = true;
    for (std::size_t class_index = 0; class_index < pulp::timeline::kCommandClassCount;
         ++class_index) {
        const auto command_class = static_cast<pulp::timeline::CommandClass>(class_index);
        std::string intents;
        for (std::size_t intent_index = 0; intent_index < pulp::timeline::kCommandIntentCount;
             ++intent_index) {
            const auto intent = static_cast<pulp::timeline::CommandIntent>(intent_index);
            if (!pulp::timeline::allows(mask, {command_class, intent}))
                continue;
            if (!intents.empty())
                intents += ',';
            intents += quote_json_string(command_intent_name(intent));
        }
        if (!first_class)
            json += ',';
        first_class = false;
        json += "{\"class\":" + quote_json_string(command_class_name(command_class)) +
                ",\"intents\":[" + intents + "]}";
    }
    json += "],\"max_session_retained_bytes\":" + quota_json(mask.max_session_retained_bytes) +
            ",\"max_transaction_retained_bytes\":" +
            quota_json(mask.max_transaction_retained_bytes) + "}";
    return json;
}

std::string writer_profile_json(const WriterProfile& profile) {
    return "{\"capabilities\":" + writer_capability_json(profile.mask) + ",\"profile\":" +
           quote_json_string(writer_profile_name(profile.kind)) + "}";
}

std::vector<CommandAuthorityRecord>
capture_command_authorities(const pulp::timeline::Transaction& transaction) {
    std::vector<CommandAuthorityRecord> records;
    records.reserve(transaction.commands.size());
    for (const auto& envelope : transaction.commands)
        records.push_back({envelope.id, pulp::timeline::command_authority(envelope.command)});
    return records;
}

std::string transaction_refusal_json(const pulp::timeline::TransactionError& error,
                                     std::string_view stage,
                                     const std::vector<CommandAuthorityRecord>& authorities) {
    std::string json = "{\"error\":{";
    if (error.command.valid())
        json += "\"command\":" + command_id_json(error.command) + ",";
    json += "\"conflict_code\":" + quote_json_string(conflict_code_name(error.code));
    if (error.code == pulp::timeline::ConflictCode::StaleRevision) {
        json += ",\"current_revision\":" + std::to_string(error.current_revision.value) +
                ",\"expected_revision\":" + std::to_string(error.expected_revision.value);
    }
    json += ",\"message\":" + quote_json_string(conflict_code_message(error.code)) +
            ",\"numeric_code\":" + std::to_string(static_cast<unsigned>(error.code));
    if (error.code == pulp::timeline::ConflictCode::CapabilityDenied) {
        for (const auto& record : authorities) {
            if (record.command != error.command)
                continue;
            json += ",\"required_capability\":" + authority_json(record.authority);
            break;
        }
    }
    json += ",\"stage\":" + quote_json_string(stage) + "},\"ok\":false}";
    return json;
}

} // namespace pulp::tools::timeline
