#include "timeline_session_store.hpp"

#include <pulp/timeline/document_session.hpp>
#include <pulp/timeline/schema_json.hpp>
#include <pulp/timeline/serialize.hpp>

#include "document_session_internal.hpp"
#include "mcp_json.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <exception>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulp_mcp {
namespace {

std::size_t saturated_add(std::size_t lhs, std::size_t rhs) noexcept {
    if (rhs > std::numeric_limits<std::size_t>::max() - lhs)
        return std::numeric_limits<std::size_t>::max();
    return lhs + rhs;
}

std::size_t saturated_multiply(std::size_t value, std::size_t factor) noexcept {
    if (factor != 0 && value > std::numeric_limits<std::size_t>::max() / factor)
        return std::numeric_limits<std::size_t>::max();
    return value * factor;
}

std::string dirty_flags_json(pulp::timeline::DirtyFlags flags) {
    using pulp::timeline::DirtyFlags;
    constexpr std::pair<DirtyFlags, std::string_view> names[] = {
        {DirtyFlags::Structure, "structure"},   {DirtyFlags::Timing, "timing"},
        {DirtyFlags::Content, "content"},       {DirtyFlags::Notes, "notes"},
        {DirtyFlags::Added, "added"},           {DirtyFlags::Removed, "removed"},
        {DirtyFlags::Automation, "automation"}, {DirtyFlags::Take, "take"},
        {DirtyFlags::Freeze, "freeze"},         {DirtyFlags::Marker, "marker"},
        {DirtyFlags::Context, "context"},       {DirtyFlags::Mixer, "mixer"},
    };
    const auto bits = static_cast<std::uint16_t>(flags);
    std::string json = "[";
    bool first = true;
    for (const auto& [flag, name] : names) {
        if ((bits & static_cast<std::uint16_t>(flag)) == 0)
            continue;
        if (!first)
            json += ',';
        first = false;
        json += pulp::timeline::quote_json_string(name);
    }
    json += ']';
    return json;
}

std::string item_id_json(pulp::timeline::ItemId id) {
    if (!id.valid())
        return "null";
    return pulp::timeline::quote_json_string(std::to_string(id.value));
}

std::string generate_process_nonce() {
    // The store nonce is deliberately not derived from a PID: PIDs are reused,
    // which could let a handle retained by a client alias a new server's first
    // session. std::random_device is the standard-library bridge to the host's
    // nondeterministic source on supported production platforms.
    std::random_device random;
    std::array<std::uint32_t, 4> words{};
    for (auto& word : words)
        word = random();
    std::ostringstream encoded;
    encoded << std::hex << std::setfill('0');
    for (const auto word : words)
        encoded << std::setw(8) << word;
    return encoded.str();
}

} // namespace

std::string timeline_dirty_set_json(const pulp::timeline::DirtySet& dirty) {
    std::string json = "{\"contexts\":[";
    bool first = true;
    for (const auto& context : dirty.contexts()) {
        if (!first)
            json += ',';
        first = false;
        std::string_view kind = "unknown";
        switch (context.kind) {
        case pulp::timeline::CompileContextKind::ChordScale:
            kind = "chord_scale";
            break;
        case pulp::timeline::CompileContextKind::Groove:
            kind = "groove";
            break;
        }
        json += "{\"kind\":" + pulp::timeline::quote_json_string(kind) +
                ",\"kind_id\":" + std::to_string(static_cast<std::uint8_t>(context.kind)) +
                ",\"owner_sequence_id\":" + item_id_json(context.owner_sequence) + "}";
    }
    json += "],\"items\":[";
    first = true;
    for (const auto& item : dirty.items()) {
        if (!first)
            json += ',';
        first = false;
        json += "{\"flag_bits\":" + std::to_string(static_cast<std::uint16_t>(item.flags)) +
                ",\"flags\":" + dirty_flags_json(item.flags) +
                ",\"item_id\":" + item_id_json(item.item) +
                ",\"owner_sequence_id\":" + item_id_json(item.owner_sequence) +
                ",\"owner_track_id\":" + item_id_json(item.owner_track) + "}";
    }
    json += "]}";
    return json;
}

namespace {

struct TimelineSession {
    TimelineSession(std::unique_ptr<pulp::timeline::DocumentSession> document_value,
                    pulp::timeline::WriterToken writer_value, std::size_t admission_charge_value)
        : document(std::move(document_value)), writer(std::move(writer_value)),
          admission_charge(admission_charge_value) {}

    std::unique_ptr<pulp::timeline::DocumentSession> document;
    pulp::timeline::WriterToken writer;
    pulp::timeline::DirtySet latest_dirty;
    pulp::timeline::DocumentRevision latest_before;
    pulp::timeline::DocumentRevision latest_after;
    std::size_t admission_charge = 0;
};

} // namespace

struct TimelineSessionStore::Impl {
    explicit Impl(TimelineSessionStoreLimits limits_value) : limits(limits_value) {
        history_reservation_ = limits.max_history_bytes_per_session;
        if (history_reservation_ == 0 && limits.max_sessions != 0)
            history_reservation_ = limits.max_admission_charge_bytes / limits.max_sessions / 2;
    }

    std::optional<std::string> open(std::string_view canonical_project, std::string& error) {
        auto registry = pulp::timeline::make_builtin_timeline_registry();
        if (!registry) {
            error = "could not construct the built-in schema registry";
            return std::nullopt;
        }
        auto project = pulp::timeline::deserialize_project(canonical_project, registry.value());
        if (!project) {
            error = "could not deserialize the opened project";
            return std::nullopt;
        }
        auto serialized = pulp::timeline::serialize_project(project.value(), registry.value(),
                                                            {limits.max_output_bytes});
        if (!serialized) {
            error = "opened project exceeds the timeline session output limit";
            return std::nullopt;
        }
        const auto charge = session_charge(serialized.value().json.size());
        if (limits.max_sessions == 0 || charge > limits.max_admission_charge_bytes) {
            error = "opened project exceeds the timeline session store budget";
            return std::nullopt;
        }
        pulp::timeline::SessionLimits session_limits;
        session_limits.max_cached_results = 0;
        session_limits.journal.max_retained_bytes =
            history_reservation_ / 3 * 2 + history_reservation_ % 3 * 2 / 3;
        session_limits.undo.max_retained_bytes =
            history_reservation_ - session_limits.journal.max_retained_bytes;
        auto document =
            pulp::timeline::DocumentSession::create(std::move(project).value(), session_limits);
        if (!document) {
            error = "could not create a document session";
            return std::nullopt;
        }
        auto writer = document.value()->register_writer();
        if (!writer) {
            error = "could not register a document writer";
            return std::nullopt;
        }

        std::lock_guard lock(mutex_);
        const auto id = "timeline-" + process_nonce_ + "-" + std::to_string(next_id_);
        const auto output = "{\"ok\":true,\"project\":" + serialized.value().json +
                            ",\"session_id\":" + pulp::timeline::quote_json_string(id) + "}";
        if (json_tool_payload_size(output) > limits.max_output_bytes) {
            error = "opened project exceeds the timeline session output limit";
            return std::nullopt;
        }
        while (sessions_.size() >= limits.max_sessions ||
               admission_charge_ > limits.max_admission_charge_bytes - charge)
            evict(order_.front());
        ++next_id_;
        order_.push_back(id);
        sessions_.emplace(
            id, TimelineSession{std::move(document).value(), std::move(writer).value(), charge});
        admission_charge_ += charge;
        return id;
    }

    pulp::tools::timeline::OperationResult apply(std::string_view id, std::string_view commands) {
        std::lock_guard lock(mutex_);
        auto* session = find(id);
        if (session == nullptr)
            return session_failure("unknown or expired timeline session");
        auto registry = pulp::timeline::make_builtin_timeline_registry();
        if (!registry)
            return session_failure("could not construct the built-in schema registry");
        auto decoded = pulp::timeline::deserialize_commands(commands, registry.value());
        if (!decoded)
            return session_failure("timeline persistence error " +
                                       std::to_string(static_cast<unsigned>(decoded.error().code)),
                                   2, decoded.error().path);
        pulp::timeline::Transaction transaction;
        transaction.id = session->writer.allocate_transaction_id();
        transaction.expected_revision = session->document->revision();
        transaction.commands.reserve(decoded.value().size());
        for (auto& command : decoded.value())
            transaction.commands.push_back(
                {session->writer.allocate_command_id(), std::move(command)});
        auto preview =
            pulp::timeline::reduce_transaction(*session->document->snapshot(), transaction);
        if (!preview)
            return transaction_failure(preview.error());
        auto prepared = prepare_candidate(id, *session, preview.value().project,
                                          preview.value().dirty, registry.value());
        if (!prepared)
            return std::move(prepared).error();
        auto committed = session->document->submit(session->writer, std::move(transaction));
        return finish_commit(id, *session, std::move(committed), std::move(prepared).value());
    }

    pulp::tools::timeline::OperationResult diff(std::string_view id) {
        std::lock_guard lock(mutex_);
        auto* session = find(id);
        if (session == nullptr)
            return session_failure("unknown or expired timeline session");
        auto json = status_json(id, *session);
        if (json_tool_payload_size(json) > limits.max_output_bytes)
            return session_failure("timeline session output byte budget exceeded");
        return {0, std::move(json)};
    }

    pulp::tools::timeline::OperationResult undo(std::string_view id) {
        std::lock_guard lock(mutex_);
        auto* session = find(id);
        if (session == nullptr)
            return session_failure("unknown or expired timeline session");
        auto registry = pulp::timeline::make_builtin_timeline_registry();
        if (!registry)
            return session_failure("could not construct the built-in schema registry");
        auto preview =
            pulp::timeline::detail::DocumentSessionPreviewAccess::undo(*session->document);
        if (!preview)
            return transaction_failure(preview.error());
        auto prepared = prepare_candidate(id, *session, preview.value().project,
                                          preview.value().dirty, registry.value());
        if (!prepared)
            return std::move(prepared).error();
        return finish_commit(id, *session, session->document->undo(session->writer),
                             std::move(prepared).value());
    }

    pulp::tools::timeline::OperationResult redo(std::string_view id) {
        std::lock_guard lock(mutex_);
        auto* session = find(id);
        if (session == nullptr)
            return session_failure("unknown or expired timeline session");
        auto registry = pulp::timeline::make_builtin_timeline_registry();
        if (!registry)
            return session_failure("could not construct the built-in schema registry");
        auto preview =
            pulp::timeline::detail::DocumentSessionPreviewAccess::redo(*session->document);
        if (!preview)
            return transaction_failure(preview.error());
        auto prepared = prepare_candidate(id, *session, preview.value().project,
                                          preview.value().dirty, registry.value());
        if (!prepared)
            return std::move(prepared).error();
        return finish_commit(id, *session, session->document->redo(session->writer),
                             std::move(prepared).value());
    }

    std::size_t admission_charge() const {
        std::lock_guard lock(mutex_);
        return admission_charge_;
    }

    void set_max_output_bytes(std::size_t maximum) {
        std::lock_guard lock(mutex_);
        limits.max_output_bytes = maximum;
    }

  private:
    struct PreparedCandidate {
        std::string project_json;
        std::vector<std::string> evictions;
        std::size_t admission_charge = 0;
        std::size_t wire_size_bound = 0;
    };

    using PrepareResult =
        pulp::runtime::Result<PreparedCandidate, pulp::tools::timeline::OperationResult>;

    TimelineSession* find(std::string_view id) {
        const auto found = sessions_.find(std::string(id));
        return found == sessions_.end() ? nullptr : &found->second;
    }

    static pulp::tools::timeline::OperationResult
    session_failure(std::string_view message, int exit_code = 1, std::string_view path = {}) {
        std::string json = "{\"error\":{\"message\":" + pulp::timeline::quote_json_string(message);
        if (!path.empty())
            json += ",\"path\":" + pulp::timeline::quote_json_string(path);
        json += ",\"stage\":\"session\"},\"ok\":false}";
        return {exit_code, std::move(json)};
    }

    static pulp::tools::timeline::OperationResult
    transaction_failure(const pulp::timeline::TransactionError& error) {
        std::string_view code = "transaction_conflict";
        std::string_view message = "timeline transaction conflict";
        if (error.code == pulp::timeline::ConflictCode::NothingToUndo) {
            code = "nothing_to_undo";
            message = "nothing to undo";
        } else if (error.code == pulp::timeline::ConflictCode::NothingToRedo) {
            code = "nothing_to_redo";
            message = "nothing to redo";
        }
        return {1, "{\"error\":{\"conflict_code\":" + pulp::timeline::quote_json_string(code) +
                       ",\"message\":" + pulp::timeline::quote_json_string(message) +
                       ",\"numeric_code\":" + std::to_string(static_cast<unsigned>(error.code)) +
                       ",\"stage\":\"session\"},\"ok\":false}"};
    }

    static std::string status_json(std::string_view id, pulp::timeline::DocumentRevision before,
                                   pulp::timeline::DocumentRevision after,
                                   const pulp::timeline::DirtySet& dirty, bool can_undo,
                                   bool can_redo) {
        return "{\"after_revision\":\"" + std::to_string(after.value) +
               "\",\"before_revision\":\"" + std::to_string(before.value) +
               "\",\"can_redo\":" + std::string(can_redo ? "true" : "false") +
               ",\"can_undo\":" + std::string(can_undo ? "true" : "false") +
               ",\"dirty\":" + timeline_dirty_set_json(dirty) + ",\"ok\":true,\"revision\":\"" +
               std::to_string(after.value) +
               "\",\"session_id\":" + pulp::timeline::quote_json_string(id) + "}";
    }

    static std::string status_json(std::string_view id, const TimelineSession& session) {
        return status_json(id, session.latest_before, session.latest_after, session.latest_dirty,
                           session.document->can_undo(), session.document->can_redo());
    }

    std::size_t session_charge(std::size_t project_json_bytes) const noexcept {
        return saturated_add(saturated_multiply(project_json_bytes, 2), history_reservation_);
    }

    PrepareResult prepare_candidate(std::string_view id, const TimelineSession& session,
                                    const pulp::timeline::Project& project,
                                    const pulp::timeline::DirtySet& dirty,
                                    const pulp::timeline::SchemaRegistry& registry) const {
        auto serialized =
            pulp::timeline::serialize_project(project, registry, {limits.max_output_bytes});
        if (!serialized) {
            return pulp::runtime::Err(
                session_failure("timeline persistence error " +
                                    std::to_string(static_cast<unsigned>(serialized.error().code)),
                                1, serialized.error().path));
        }
        const auto charge = session_charge(serialized.value().json.size());
        if (charge > limits.max_admission_charge_bytes) {
            return pulp::runtime::Err(
                session_failure("timeline session admission charge exceeded"));
        }
        std::size_t projected =
            saturated_add(admission_charge_ - session.admission_charge, charge);
        std::vector<std::string> evictions;
        for (const auto& candidate : order_) {
            if (projected <= limits.max_admission_charge_bytes)
                break;
            if (candidate == id)
                continue;
            const auto found = sessions_.find(candidate);
            if (found == sessions_.end())
                continue;
            projected -= found->second.admission_charge;
            evictions.push_back(candidate);
        }
        if (projected > limits.max_admission_charge_bytes)
            return pulp::runtime::Err(
                session_failure("timeline session admission charge exceeded"));
        auto project_json = std::move(serialized).value().json;
        const auto before = session.document->revision();
        if (before.value == std::numeric_limits<std::uint64_t>::max())
            return pulp::runtime::Err(session_failure("timeline session revision space exhausted"));
        const pulp::timeline::DocumentRevision after{before.value + 1};
        auto output = status_json(id, before, after, dirty, false, false);
        output.insert(output.size() - 1, ",\"project\":" + project_json);
        const auto wire_size_bound = json_tool_payload_size(output);
        if (wire_size_bound > limits.max_output_bytes)
            return pulp::runtime::Err(
                session_failure("timeline session output byte budget exceeded"));
        return pulp::runtime::Ok(PreparedCandidate{std::move(project_json), std::move(evictions),
                                                   charge, wire_size_bound});
    }

    pulp::tools::timeline::OperationResult finish_commit(
        std::string_view id, TimelineSession& session,
        pulp::runtime::Result<pulp::timeline::CommitResult, pulp::timeline::TransactionError>
            committed,
        PreparedCandidate prepared) {
        if (!committed)
            return transaction_failure(committed.error());
        for (const auto& victim : prepared.evictions)
            evict(victim);
        admission_charge_ -= session.admission_charge;
        admission_charge_ += prepared.admission_charge;
        session.admission_charge = prepared.admission_charge;
        session.latest_dirty = committed.value().dirty;
        session.latest_after = committed.value().revision;
        session.latest_before = {committed.value().revision.value - 1};
        auto json = status_json(id, session);
        json.insert(json.size() - 1, ",\"project\":" + prepared.project_json);
        // prepare_candidate builds the same response with both booleans set to
        // false, the longest spelling. Every other field comes from the exact
        // preview committed above, so there is no fallible work after publish.
        if (json_tool_payload_size(json) > prepared.wire_size_bound)
            std::terminate();
        return {0, std::move(json)};
    }

    void evict(const std::string& id) {
        const auto found = sessions_.find(id);
        if (found != sessions_.end()) {
            admission_charge_ -= found->second.admission_charge;
            sessions_.erase(found);
        }
        const auto ordered = std::find(order_.begin(), order_.end(), id);
        if (ordered != order_.end())
            order_.erase(ordered);
    }

    mutable std::mutex mutex_;
    TimelineSessionStoreLimits limits;
    const std::string process_nonce_ = generate_process_nonce();
    std::uint64_t next_id_ = 1;
    std::deque<std::string> order_;
    std::unordered_map<std::string, TimelineSession> sessions_;
    std::size_t admission_charge_ = 0;
    std::size_t history_reservation_ = 0;
};

TimelineSessionStore& timeline_sessions() {
    static TimelineSessionStore store;
    return store;
}

TimelineSessionStore::TimelineSessionStore(TimelineSessionStoreLimits limits)
    : impl_(std::make_unique<Impl>(limits)) {}

TimelineSessionStore::~TimelineSessionStore() = default;

std::optional<std::string> TimelineSessionStore::open(std::string_view canonical_project,
                                                      std::string& error) {
    return impl_->open(canonical_project, error);
}

pulp::tools::timeline::OperationResult TimelineSessionStore::apply(std::string_view session_id,
                                                                   std::string_view commands) {
    return impl_->apply(session_id, commands);
}

pulp::tools::timeline::OperationResult TimelineSessionStore::diff(std::string_view session_id) {
    return impl_->diff(session_id);
}

pulp::tools::timeline::OperationResult TimelineSessionStore::undo(std::string_view session_id) {
    return impl_->undo(session_id);
}

pulp::tools::timeline::OperationResult TimelineSessionStore::redo(std::string_view session_id) {
    return impl_->redo(session_id);
}

std::size_t TimelineSessionStore::admission_charge_for_testing() const {
    return impl_->admission_charge();
}

void TimelineSessionStore::set_max_output_bytes_for_testing(std::size_t maximum) {
    impl_->set_max_output_bytes(maximum);
}

std::optional<std::string> open_timeline_session(std::string_view canonical_project,
                                                 std::string& error) {
    return timeline_sessions().open(canonical_project, error);
}

pulp::tools::timeline::OperationResult apply_timeline_session(std::string_view session_id,
                                                              std::string_view commands) {
    return timeline_sessions().apply(session_id, commands);
}

pulp::tools::timeline::OperationResult diff_timeline_session(std::string_view session_id) {
    return timeline_sessions().diff(session_id);
}

pulp::tools::timeline::OperationResult undo_timeline_session(std::string_view session_id) {
    return timeline_sessions().undo(session_id);
}

pulp::tools::timeline::OperationResult redo_timeline_session(std::string_view session_id) {
    return timeline_sessions().redo(session_id);
}

} // namespace pulp_mcp
