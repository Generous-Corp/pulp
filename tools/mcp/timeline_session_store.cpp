#include "timeline_session_store.hpp"

#include <pulp/timeline/document_session.hpp>
#include <pulp/timeline/schema_json.hpp>
#include <pulp/timeline/serialize.hpp>

#include "document_session_internal.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <iomanip>
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
                    pulp::timeline::WriterToken writer_value, std::size_t retained_bytes_value)
        : document(std::move(document_value)), writer(std::move(writer_value)),
          retained_bytes(retained_bytes_value) {}

    std::unique_ptr<pulp::timeline::DocumentSession> document;
    pulp::timeline::WriterToken writer;
    pulp::timeline::DirtySet latest_dirty;
    std::size_t retained_bytes = 0;
};

} // namespace

struct TimelineSessionStore::Impl {
    explicit Impl(TimelineSessionStoreLimits limits_value) : limits(limits_value) {}

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
        auto serialized = pulp::timeline::serialize_project(
            project.value(), registry.value(), {limits.max_output_bytes});
        if (!serialized) {
            error = "opened project exceeds the timeline session output limit";
            return std::nullopt;
        }
        const auto retained = serialized.value().json.size();
        if (limits.max_sessions == 0 || retained > limits.max_retained_bytes) {
            error = "opened project exceeds the timeline session store budget";
            return std::nullopt;
        }
        pulp::timeline::SessionLimits session_limits;
        session_limits.max_cached_results = 0;
        auto document = pulp::timeline::DocumentSession::create(std::move(project).value(),
                                                                 session_limits);
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
        while (sessions_.size() >= limits.max_sessions ||
               retained_bytes_ > limits.max_retained_bytes - retained)
            evict(order_.front());
        const auto id = "timeline-" + process_nonce_ + "-" + std::to_string(next_id_++);
        order_.push_back(id);
        sessions_.emplace(id, TimelineSession{std::move(document).value(),
                                               std::move(writer).value(), retained});
        retained_bytes_ += retained;
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
        auto preview = pulp::timeline::reduce_transaction(*session->document->snapshot(), transaction);
        if (!preview)
            return transaction_failure(preview.error());
        auto prepared = prepare_candidate(id, *session, preview.value().project, registry.value());
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
        return {0, status_json(id, *session)};
    }

    pulp::tools::timeline::OperationResult undo(std::string_view id) {
        std::lock_guard lock(mutex_);
        auto* session = find(id);
        if (session == nullptr)
            return session_failure("unknown or expired timeline session");
        auto registry = pulp::timeline::make_builtin_timeline_registry();
        if (!registry)
            return session_failure("could not construct the built-in schema registry");
        auto preview = pulp::timeline::detail::DocumentSessionPreviewAccess::undo(*session->document);
        if (!preview)
            return transaction_failure(preview.error());
        auto prepared = prepare_candidate(id, *session, preview.value().project, registry.value());
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
        auto preview = pulp::timeline::detail::DocumentSessionPreviewAccess::redo(*session->document);
        if (!preview)
            return transaction_failure(preview.error());
        auto prepared = prepare_candidate(id, *session, preview.value().project, registry.value());
        if (!prepared)
            return std::move(prepared).error();
        return finish_commit(id, *session, session->document->redo(session->writer),
                             std::move(prepared).value());
    }

    std::size_t retained_bytes() const {
        std::lock_guard lock(mutex_);
        return retained_bytes_;
    }

    void set_max_output_bytes(std::size_t maximum) {
        std::lock_guard lock(mutex_);
        limits.max_output_bytes = maximum;
    }

  private:
    struct PreparedCandidate {
        std::string project_json;
        std::vector<std::string> evictions;
        std::size_t retained_bytes = 0;
    };

    using PrepareResult = pulp::runtime::Result<
        PreparedCandidate, pulp::tools::timeline::OperationResult>;

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

    static std::string status_json(std::string_view id, const TimelineSession& session) {
        return "{\"can_redo\":" + std::string(session.document->can_redo() ? "true" : "false") +
               ",\"can_undo\":" + std::string(session.document->can_undo() ? "true" : "false") +
               ",\"dirty\":" + timeline_dirty_set_json(session.latest_dirty) +
               ",\"ok\":true,\"revision\":\"" + std::to_string(session.document->revision().value) +
               "\",\"session_id\":" + pulp::timeline::quote_json_string(id) + "}";
    }

    PrepareResult prepare_candidate(std::string_view id, const TimelineSession& session,
                                    const pulp::timeline::Project& project,
                                    const pulp::timeline::SchemaRegistry& registry) const {
        auto serialized = pulp::timeline::serialize_project(
            project, registry, {limits.max_output_bytes});
        if (!serialized) {
            return pulp::runtime::Err(session_failure(
                "timeline persistence error " +
                    std::to_string(static_cast<unsigned>(serialized.error().code)),
                1, serialized.error().path));
        }
        const auto retained = serialized.value().json.size();
        if (retained > limits.max_retained_bytes) {
            return pulp::runtime::Err(
                session_failure("timeline session store byte budget exceeded"));
        }
        std::size_t projected = retained_bytes_ - session.retained_bytes + retained;
        std::vector<std::string> evictions;
        for (const auto& candidate : order_) {
            if (projected <= limits.max_retained_bytes)
                break;
            if (candidate == id)
                continue;
            const auto found = sessions_.find(candidate);
            if (found == sessions_.end())
                continue;
            projected -= found->second.retained_bytes;
            evictions.push_back(candidate);
        }
        if (projected > limits.max_retained_bytes)
            return pulp::runtime::Err(
                session_failure("timeline session store byte budget exceeded"));
        return pulp::runtime::Ok(PreparedCandidate{
            std::move(serialized).value().json, std::move(evictions), retained});
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
        retained_bytes_ -= session.retained_bytes;
        retained_bytes_ += prepared.retained_bytes;
        session.retained_bytes = prepared.retained_bytes;
        session.latest_dirty = committed.value().dirty;
        auto json = status_json(id, session);
        json.insert(json.size() - 1, ",\"project\":" + prepared.project_json);
        return {0, std::move(json)};
    }

    void evict(const std::string& id) {
        const auto found = sessions_.find(id);
        if (found != sessions_.end()) {
            retained_bytes_ -= found->second.retained_bytes;
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
    std::size_t retained_bytes_ = 0;
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

std::size_t TimelineSessionStore::retained_bytes_for_testing() const {
    return impl_->retained_bytes();
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
