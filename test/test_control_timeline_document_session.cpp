#include "timeline_command_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/control_timeline_document_session_executor.hpp>

#include <pulp/tools/timeline/writer_profile.hpp>

#include <choc/text/choc_JSON.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace pulp::inspect;
namespace tl = pulp::timeline;
namespace tools_tl = pulp::tools::timeline;

namespace {

using Action = ControlTimelineDocumentSessionAction;
using Outcome = ControlTimelineDocumentSessionOutcome;
using SessionRequest = ControlTimelineDocumentSessionRequest;

constexpr std::string_view kOperationId = "dev.pulp.timeline/document-session@1";

tl::MediaAsset make_asset(tl::ItemId id) {
    return tl::MediaAsset{id,
                          "recorded.wav",
                          960,
                          {48'000, 1},
                          timeline_test::content_hash('e'),
                          tl::AssetStoragePolicy::PreferEmbedded,
                          {{tl::AssetLocatorKind::PackageRelative, "media/recorded.wav"}},
                          {},
                          {}};
}

/// What a host remembered about one accepted `idempotency_key`.
///
/// Mirrors the offline MCP store: the recorded response is what a hit returns,
/// and the commands plus the revision they were written against are what decide
/// whether a second use of the key is the same request or a collision.
struct IdempotencyRecord {
    std::string commands;
    std::uint64_t expected_revision = 0;
    Outcome response;
};

/// One host that owns a real `timeline::DocumentSession` behind the broker.
///
/// This is the seam a Forge Sequencer or Forge Modular host fills: the executor
/// owns no session mechanics, so everything below is the host's own document,
/// its own writer, and the session's own verdicts.
struct LiveTimelineHost {
    std::unique_ptr<tl::DocumentSession> session;
    tl::WriterToken writer;
    std::string writer_profile;
    std::string session_id;
    tl::ItemId asset_id;
    std::map<std::string, IdempotencyRecord> replay;

    int invocations = 0;
    /// The session's own refusal, kept so a test can assert the same verdict
    /// from the session side and from the broker side.
    std::optional<tl::TransactionError> last_error;

    Outcome refuse(const tl::TransactionError& error, std::string_view stage,
                   const std::vector<tools_tl::CommandAuthorityRecord>& authorities) {
        last_error = error;
        Outcome outcome;
        outcome.accepted = false;
        outcome.session_id = session_id;
        outcome.conflict = error.code;
        outcome.refusal_json = tools_tl::transaction_refusal_json(error, stage, authorities);
        outcome.explanation = std::string(tools_tl::conflict_code_name(error.code));
        return outcome;
    }

    std::vector<tl::Command> decode(const std::string& commands) const {
        if (commands == "create-asset") {
            return {tl::CreateAsset{make_asset(tl::ItemId{session->snapshot()->next_item_id()})}};
        }
        if (commands == "remove-asset")
            return {tl::RemoveAsset{asset_id}};
        if (commands == "set-tempo") {
            return {tl::SetTempoMap{session->snapshot()->tempo_map(),
                                    timeline_test::make_tempo_map(88.0)}};
        }
        return {};
    }

    Outcome open(const SessionRequest& request) {
        const auto profile = tools_tl::writer_profile_by_name(request.writer_profile);
        REQUIRE(profile);
        session = std::move(tl::DocumentSession::create(timeline_test::make_project())).value();
        // A seeding writer publishes the asset the destructive cases target, so
        // the document under test is not empty.
        auto seeder = std::move(session->register_writer()).value();
        asset_id = tl::ItemId{session->snapshot()->next_item_id()};
        REQUIRE(session->submit(seeder, timeline_test::session_transaction(
                                            seeder, session->revision(),
                                            {tl::CreateAsset{make_asset(asset_id)}})));
        writer = std::move(session->register_writer(profile->mask)).value();
        writer_profile = request.writer_profile;
        session_id = "timeline-document-session-1";
        return {.accepted = true, .session_id = session_id, .revision = session->revision().value};
    }

    Outcome apply(const SessionRequest& request) {
        if (!request.idempotency_key.empty()) {
            const auto found = replay.find(request.idempotency_key);
            if (found != replay.end()) {
                const bool same_request =
                    found->second.commands == request.commands_json &&
                    (!request.expected_revision ||
                     *request.expected_revision == found->second.expected_revision);
                if (!same_request) {
                    tl::TransactionError error;
                    error.code = tl::ConflictCode::TransactionIdCollision;
                    error.expected_revision = tl::DocumentRevision{found->second.expected_revision};
                    error.current_revision = session->revision();
                    return refuse(error, "apply", {});
                }
                // The recorded response is what a hit returns; the commands are
                // deliberately not re-evaluated against a document that moved on.
                auto response = found->second.response;
                response.replayed = true;
                return response;
            }
        }

        const auto expected = request.expected_revision
                                  ? tl::DocumentRevision{*request.expected_revision}
                                  : session->revision();
        auto transaction =
            timeline_test::session_transaction(writer, expected, decode(request.commands_json));
        const auto authorities = tools_tl::capture_command_authorities(transaction);
        auto committed = session->submit(writer, std::move(transaction));
        if (!committed)
            return refuse(committed.error(), "apply", authorities);

        Outcome outcome{.accepted = true,
                        .session_id = session_id,
                        .revision = committed.value().revision.value,
                        .applied = true};
        if (!request.idempotency_key.empty()) {
            replay.emplace(request.idempotency_key,
                           IdempotencyRecord{request.commands_json, expected.value, outcome});
        }
        return outcome;
    }

    Outcome history(bool undo) {
        auto committed = undo ? session->undo(writer) : session->redo(writer);
        if (!committed)
            return refuse(committed.error(), undo ? "undo" : "redo", {});
        return {.accepted = true,
                .session_id = session_id,
                .revision = committed.value().revision.value,
                .applied = true};
    }

    Outcome invoke(const SessionRequest& request) {
        ++invocations;
        switch (request.action) {
        case Action::Open:
            return open(request);
        case Action::Apply:
            return apply(request);
        case Action::Diff:
            return {.accepted = true,
                    .session_id = session_id,
                    .revision = session->revision().value,
                    .diff_json = "{\"assets\":" +
                                 std::to_string(session->snapshot()->assets().size()) + "}"};
        case Action::Undo:
            return history(true);
        case Action::Redo:
            return history(false);
        }
        return {};
    }
};

ControlAdmissionPlan plan() {
    ControlAdmissionPlan value;
    value.registration_id = ControlRegistrationId{"registration-1"};
    value.instance_id = "instance-1";
    value.publication_id = "publication-1";
    value.receipt_id = ControlReceiptId{"receipt-1"};
    return value;
}

ControlTimelineDocumentSessionSource source(LiveTimelineHost& host) {
    return {.registration_id = ControlRegistrationId{"registration-1"},
            .instance_id = "instance-1",
            .publication_id = "publication-1",
            .invoke = [&host](const SessionRequest& request) { return host.invoke(request); }};
}

ControlOperationExecutor executor(LiveTimelineHost& host) {
    return make_control_timeline_document_session_executor(
        [&host](const ControlAdmissionPlan&) { return std::optional{source(host)}; });
}

ControlRequestEnvelope request(std::string params) {
    return {.registration_id = "registration-1",
            .operation_id = std::string(kOperationId),
            .operation_version = 1,
            .params_json = std::move(params)};
}

ControlExecutionContext context() {
    return {.checkpoint = [] { return ControlExecutionCheckpoint::Continue; }};
}

std::string open_params(std::string_view profile) {
    return std::string(R"({"action":"open","project":"canonical.pulpproject","writer_profile":")") +
           std::string(profile) + "\"}";
}

std::string apply_params(std::string_view commands, std::string_view profile,
                         std::string_view idempotency_key = {},
                         std::optional<std::uint64_t> expected_revision = std::nullopt) {
    std::string params = R"({"action":"apply","session_id":"timeline-document-session-1")";
    params += ",\"commands\":\"" + std::string(commands) + "\"";
    params += ",\"writer_profile\":\"" + std::string(profile) + "\"";
    if (!idempotency_key.empty())
        params += ",\"idempotency_key\":\"" + std::string(idempotency_key) + "\"";
    if (expected_revision)
        params += ",\"expected_revision\":" + std::to_string(*expected_revision);
    return params + "}";
}

/// Opens a live session through the broker and returns its published revision.
std::uint64_t open_session(const ControlOperationExecutor& execute, std::string_view profile) {
    const auto outcome = execute(plan(), request(open_params(profile)), context());
    REQUIRE(outcome.terminal_state == ControlReceiptState::Completed);
    const auto detail = choc::json::parse(outcome.result.detail_json);
    CHECK(detail["session_id"].getString() == "timeline-document-session-1");
    CHECK(detail["writer_profile"].getString() == profile);
    return static_cast<std::uint64_t>(detail["revision"].getWithDefault<std::int64_t>(-1));
}

} // namespace

TEST_CASE("the seven layers compose end to end over a live document session",
          "[inspect][control][timeline][document-session]") {
    LiveTimelineHost host;
    const auto execute = executor(host);

    const auto opened = open_session(execute, "editor");
    REQUIRE(host.session);
    CHECK(opened == host.session->revision().value);
    const auto assets_before = host.session->snapshot()->assets().size();

    const auto applied =
        execute(plan(), request(apply_params("create-asset", "editor")), context());
    REQUIRE(applied.terminal_state == ControlReceiptState::Completed);
    const auto applied_detail = choc::json::parse(applied.result.detail_json);
    CHECK(applied_detail["applied"].getWithDefault(false));
    CHECK_FALSE(applied_detail["replayed"].getWithDefault(true));
    CHECK(applied_detail["receipt_id"].getString() == "receipt-1");
    CHECK(applied_detail["action"].getString() == "apply");
    // The live document actually moved: the executor published what the session
    // committed, not a broker-side story about it.
    CHECK(static_cast<std::uint64_t>(applied_detail["revision"].getWithDefault<std::int64_t>(0)) ==
          host.session->revision().value);
    CHECK(host.session->revision().value == opened + 1);
    CHECK(host.session->snapshot()->assets().size() == assets_before + 1);

    const auto diffed = execute(
        plan(), request(R"({"action":"diff","session_id":"timeline-document-session-1"})"),
        context());
    REQUIRE(diffed.terminal_state == ControlReceiptState::Completed);
    const auto diff_detail = choc::json::parse(diffed.result.detail_json);
    CHECK(diff_detail.hasObjectMember("diff"));
    CHECK(diff_detail["diff"].getString() ==
          "{\"assets\":" + std::to_string(assets_before + 1) + "}");

    const auto undone = execute(
        plan(), request(R"({"action":"undo","session_id":"timeline-document-session-1"})"),
        context());
    REQUIRE(undone.terminal_state == ControlReceiptState::Completed);
    CHECK(host.session->snapshot()->assets().size() == assets_before);
}

TEST_CASE("a stale expected_revision is refused live under the offline surface's own code",
          "[inspect][control][timeline][document-session][concurrency]") {
    LiveTimelineHost host;
    const auto execute = executor(host);
    const auto opened = open_session(execute, "editor");

    const auto stale = execute(
        plan(), request(apply_params("create-asset", "editor", {}, opened + 7)), context());

    // Broker side.
    CHECK(stale.terminal_state == ControlReceiptState::Failed);
    CHECK(stale.result.result_code == ControlResultCode::StateConflict);
    CHECK(stale.result.retry == ControlRetryClassification::AfterRefresh);

    // Session side: the same verdict, named in the offline surface's vocabulary.
    REQUIRE(host.last_error);
    CHECK(host.last_error->code == tl::ConflictCode::StaleRevision);
    CHECK(tools_tl::conflict_code_name(host.last_error->code) == "stale_revision");
    CHECK(stale.result.result_code ==
          control_timeline_conflict_result_code(host.last_error->code));
    CHECK(stale.result.retry == control_timeline_conflict_retry(host.last_error->code));
    // The typed refusal envelope crosses the broker verbatim.
    CHECK(stale.result.detail_json ==
          tools_tl::transaction_refusal_json(*host.last_error, "apply", {}));
    const auto refusal = choc::json::parse(stale.result.detail_json);
    CHECK(refusal["error"]["conflict_code"].getString() == "stale_revision");
    CHECK(static_cast<std::uint64_t>(
              refusal["error"]["current_revision"].getWithDefault<std::int64_t>(0)) == opened);

    // Nothing landed, and the same commands against the current revision do.
    CHECK(host.session->revision().value == opened);
    const auto fresh = execute(plan(), request(apply_params("create-asset", "editor")), context());
    CHECK(fresh.terminal_state == ControlReceiptState::Completed);
    CHECK(host.session->revision().value == opened + 1);
}

TEST_CASE("a replayed idempotency key returns the original result and a changed one collides",
          "[inspect][control][timeline][document-session][idempotency]") {
    LiveTimelineHost host;
    const auto execute = executor(host);
    const auto opened = open_session(execute, "editor");

    const auto first =
        execute(plan(), request(apply_params("create-asset", "editor", "once")), context());
    REQUIRE(first.terminal_state == ControlReceiptState::Completed);
    const auto first_detail = choc::json::parse(first.result.detail_json);
    const auto committed_revision = host.session->revision().value;
    const auto committed_assets = host.session->snapshot()->assets().size();
    CHECK(committed_revision == opened + 1);

    const auto replayed =
        execute(plan(), request(apply_params("create-asset", "editor", "once")), context());
    REQUIRE(replayed.terminal_state == ControlReceiptState::Completed);
    const auto replayed_detail = choc::json::parse(replayed.result.detail_json);
    CHECK(replayed_detail["replayed"].getWithDefault(false));
    CHECK(replayed_detail["revision"].getWithDefault<std::int64_t>(-1) ==
          first_detail["revision"].getWithDefault<std::int64_t>(-2));
    CHECK(replayed_detail["applied"].getWithDefault(false) ==
          first_detail["applied"].getWithDefault(false));
    // A replay returns the original result without writing a second time.
    CHECK(host.session->revision().value == committed_revision);
    CHECK(host.session->snapshot()->assets().size() == committed_assets);

    const auto collision =
        execute(plan(), request(apply_params("set-tempo", "editor", "once")), context());
    CHECK(collision.terminal_state == ControlReceiptState::Failed);
    CHECK(collision.result.result_code == ControlResultCode::StateConflict);
    CHECK(collision.result.retry == ControlRetryClassification::Never);
    REQUIRE(host.last_error);
    CHECK(host.last_error->code == tl::ConflictCode::TransactionIdCollision);
    CHECK(tools_tl::conflict_code_name(host.last_error->code) == "transaction_id_collision");
    CHECK(collision.result.result_code ==
          control_timeline_conflict_result_code(host.last_error->code));
    CHECK(choc::json::parse(collision.result.detail_json)["error"]["conflict_code"].getString() ==
          "transaction_id_collision");
    CHECK(host.session->revision().value == committed_revision);

    // The same commands under a fresh key are accepted, so the refusal is the
    // key collision and not the tempo change.
    const auto fresh =
        execute(plan(), request(apply_params("set-tempo", "editor", "twice")), context());
    CHECK(fresh.terminal_state == ControlReceiptState::Completed);
    CHECK(host.session->revision().value == committed_revision + 1);
}

TEST_CASE("a CommandAuthority refusal reaches the broker as the session's own verdict",
          "[inspect][control][timeline][document-session][authority]") {
    LiveTimelineHost host;
    const auto execute = executor(host);
    open_session(execute, "proposal");
    const auto revision_before = host.session->revision().value;
    const auto assets_before = host.session->snapshot()->assets().size();
    REQUIRE(assets_before == 1);

    const auto denied =
        execute(plan(), request(apply_params("remove-asset", "proposal")), context());

    // Broker side.
    CHECK(denied.terminal_state == ControlReceiptState::Failed);
    CHECK(denied.result.result_code == ControlResultCode::PolicyDenied);
    CHECK(denied.result.retry == ControlRetryClassification::AfterGrant);

    // Session side, in the same vocabulary, naming the same denied axis.
    REQUIRE(host.last_error);
    CHECK(host.last_error->code == tl::ConflictCode::CapabilityDenied);
    CHECK(tools_tl::conflict_code_name(host.last_error->code) == "capability_denied");
    CHECK(denied.result.result_code ==
          control_timeline_conflict_result_code(host.last_error->code));
    CHECK(denied.result.retry == control_timeline_conflict_retry(host.last_error->code));
    const auto refusal = choc::json::parse(denied.result.detail_json);
    CHECK(refusal["error"]["conflict_code"].getString() == "capability_denied");
    CHECK(refusal["error"]["required_capability"]["class"].getString() == "asset");
    CHECK(refusal["error"]["required_capability"]["intent"].getString() == "remove");

    // Nothing was removed.
    CHECK(host.session->revision().value == revision_before);
    CHECK(host.session->snapshot()->assets().size() == assets_before);

    // Control: the identical removal under the authority that holds it lands,
    // so the refusal is the capability mask and not an unrelated rejection.
    LiveTimelineHost editor_host;
    const auto editor_execute = executor(editor_host);
    open_session(editor_execute, "editor");
    const auto allowed =
        editor_execute(plan(), request(apply_params("remove-asset", "editor")), context());
    CHECK(allowed.terminal_state == ControlReceiptState::Completed);
    CHECK(editor_host.session->snapshot()->assets().empty());
}

TEST_CASE("the broker never selects the trusted authority on a caller's behalf",
          "[inspect][control][timeline][document-session][authority]") {
    // The offline authority model does carry `trusted`; the broker refuses to
    // select it rather than defining a second, parallel set of profiles.
    REQUIRE(tools_tl::writer_profile_by_name("trusted"));
    CHECK_FALSE(control_timeline_admissible_writer_profile("trusted"));
    CHECK_FALSE(control_timeline_admissible_writer_profile("root"));
    // Every name the broker does admit resolves in the existing model, and an
    // unspecified profile resolves down to the least authority.
    REQUIRE(control_timeline_admissible_writer_profile("").value() == "proposal");
    for (const auto* name : {"proposal", "editor"}) {
        const auto admitted = control_timeline_admissible_writer_profile(name);
        REQUIRE(admitted);
        CHECK(*admitted == name);
        REQUIRE(tools_tl::writer_profile_by_name(*admitted));
    }
    // The projection is the existing mask, not a broker-side copy of it.
    CHECK(tools_tl::writer_profile_by_name("proposal")->mask.allowed ==
          tl::non_destructive_capabilities().allowed);
    CHECK(tools_tl::writer_profile_by_name("editor")->mask.allowed ==
          tl::unrestricted_capabilities().allowed);

    LiveTimelineHost host;
    const auto execute = executor(host);
    const auto escalated = execute(plan(), request(open_params("trusted")), context());
    CHECK(escalated.terminal_state == ControlReceiptState::Failed);
    CHECK(escalated.result.result_code == ControlResultCode::PolicyDenied);
    // The session is never reached, so the escalation cannot have side effects.
    CHECK(host.invocations == 0);
    CHECK(host.session == nullptr);

    // Control: an admissible profile does reach the session.
    open_session(execute, "editor");
    CHECK(host.invocations == 1);
    REQUIRE(host.session);
}

TEST_CASE("shape and exact-instance bindings refuse before the session is reached",
          "[inspect][control][timeline][document-session][admission]") {
    LiveTimelineHost host;
    const auto execute = executor(host);

    const auto cases = std::vector<std::string>{
        R"({"action":"open","writer_profile":"editor"})",
        R"({"action":"open","project":"p","session_id":"s","writer_profile":"editor"})",
        R"({"action":"diff","writer_profile":"editor"})",
        R"({"action":"diff","session_id":"s","commands":"create-asset","writer_profile":"editor"})",
        R"({"action":"apply","session_id":"s","writer_profile":"editor"})",
        R"({"action":"rewrite","session_id":"s","writer_profile":"editor"})",
        R"({"session_id":"s","writer_profile":"editor"})",
        R"({"action":"apply","session_id":"s","commands":"c","expected_revision":-1})",
        "not json at all",
    };
    for (const auto& params : cases) {
        const auto refused = execute(plan(), request(params), context());
        CHECK(refused.result.result_code == ControlResultCode::InvalidRequest);
    }
    CHECK(host.invocations == 0);

    // A source that is not the exact admitted instance is unusable.
    auto drifted = make_control_timeline_document_session_executor(
        [&host](const ControlAdmissionPlan&) {
            auto value = source(host);
            value.instance_id = "instance-2";
            return std::optional{value};
        });
    const auto unavailable = drifted(plan(), request(open_params("editor")), context());
    CHECK(unavailable.result.result_code == ControlResultCode::HostUnavailable);
    CHECK(unavailable.result.retry == ControlRetryClassification::AfterRefresh);

    auto absent = make_control_timeline_document_session_executor(
        [](const ControlAdmissionPlan&) {
            return std::optional<ControlTimelineDocumentSessionSource>{};
        });
    CHECK(absent(plan(), request(open_params("editor")), context()).result.result_code ==
          ControlResultCode::HostUnavailable);
    CHECK(host.invocations == 0);

    // Control: the same request against the exact admitted source is accepted.
    CHECK(execute(plan(), request(open_params("editor")), context()).terminal_state ==
          ControlReceiptState::Completed);
    CHECK(host.invocations == 1);
}

TEST_CASE("a cancelled or revoked checkpoint stops the operation terminally",
          "[inspect][control][timeline][document-session][cancellation]") {
    LiveTimelineHost host;
    const auto execute = executor(host);

    ControlExecutionContext cancelled{
        .checkpoint = [] { return ControlExecutionCheckpoint::Cancelled; }};
    const auto stopped = execute(plan(), request(open_params("editor")), cancelled);
    CHECK(stopped.terminal_state == ControlReceiptState::Cancelled);
    CHECK(stopped.result.result_code == ControlResultCode::Cancelled);
    CHECK(stopped.result.cancellation_reason == "client-cancelled");
    CHECK(host.invocations == 0);

    ControlExecutionContext revoked{
        .checkpoint = [] { return ControlExecutionCheckpoint::AuthorityRevoked; }};
    const auto lost = execute(plan(), request(open_params("editor")), revoked);
    CHECK(lost.terminal_state == ControlReceiptState::Cancelled);
    CHECK(lost.result.cancellation_reason == "authority-revoked");
    CHECK(host.invocations == 0);

    // Control: a continuing checkpoint runs the same request through.
    CHECK(execute(plan(), request(open_params("editor")), context()).terminal_state ==
          ControlReceiptState::Completed);
    CHECK(host.invocations == 1);
}
