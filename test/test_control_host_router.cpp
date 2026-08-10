#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/control_host_router.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <optional>
#include <thread>

using namespace std::chrono_literals;
using namespace pulp::inspect;

namespace {

ControlAdmissionPlan plan(std::int64_t deadline) {
    ControlAdmissionPlan result;
    result.broker_id = ControlBrokerId{"broker-1"};
    result.client_principal = "peer-secret";
    result.client_id = ControlClientId{"client-secret"};
    result.registration_id = ControlRegistrationId{"registration-1"};
    result.grant_id = ControlGrantId{"grant-secret"};
    result.session_id = "session-1";
    result.instance_id = "slot-1";
    result.publication_id = "process-7/slot-3";
    result.instance_generation = "process-7/slot-3";
    result.capability = InspectorCapability::SessionDescribe;
    result.operation_id = "session.describe";
    result.operation_version = 1;
    result.manifest_digest = std::string(64, 'a');
    result.producer_artifact_digest = std::string(64, 'b');
    result.receipt_id = ControlReceiptId{"receipt-1"};
    result.deadline_unix_ms = deadline;
    return result;
}

std::int64_t future_deadline() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch() + 5s)
        .count();
}

ControlRequestEnvelope request() {
    return {.request_id = "request-1",
            .operation_id = "session.describe",
            .operation_version = 1,
            .params_json = "{}"};
}

ControlExecutionContext context() {
    return {.report_progress = [](std::uint64_t, std::uint64_t, std::string) { return true; },
            .checkpoint = [] { return ControlExecutionCheckpoint::Continue; },
            .complete_deferred = [](ControlExecutionOutcome) {}};
}

} // namespace

TEST_CASE("Pulp host slot routing rejects stale instance generation and unload") {
    ControlHostRouter router;
    std::atomic<unsigned> deliveries{0};
    REQUIRE(router.attach_slot(ControlRegistrationId{"registration-1"}, 3, "slot-1",
                               "process-7/slot-3", [&](const ControlEnvelope&) {
                                   ++deliveries;
                                   return false;
                               }));
    auto stale = plan(future_deadline());
    stale.instance_generation = "process-7/slot-2";
    const auto stale_result = router.executor()(stale, request(), context());
    CHECK(stale_result.result.result_code == ControlResultCode::SessionStale);
    CHECK(deliveries == 0);

    router.detach(ControlRegistrationId{"registration-1"}, 3);
    const auto unloaded = router.executor()(plan(future_deadline()), request(), context());
    CHECK(unloaded.result.result_code == ControlResultCode::HostUnavailable);
    CHECK(deliveries == 0);
}

TEST_CASE("host router correlates completion to exact registration generation") {
    ControlHostRouter router;
    std::mutex mutex;
    std::optional<ControlHostExecuteEnvelope> dispatched;
    REQUIRE(router.attach(
        ControlRegistrationId{"registration-1"}, 7, [&](const ControlEnvelope& envelope) {
            if (const auto* execute = std::get_if<ControlHostExecuteEnvelope>(&envelope.payload)) {
                std::lock_guard lock(mutex);
                dispatched = *execute;
            }
            return true;
        }));

    auto outcome = std::async(std::launch::async, [&] {
        return router.executor()(plan(future_deadline()), request(), context());
    });
    std::string route;
    for (unsigned attempt = 0; attempt != 100 && route.empty(); ++attempt) {
        {
            std::lock_guard lock(mutex);
            if (dispatched)
                route = dispatched->route_id;
        }
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE_FALSE(route.empty());
    const ControlEnvelope completed{.payload = ControlHostCompleteEnvelope{
                                        .route_id = route,
                                        .terminal_state = ControlReceiptState::Completed,
                                    }};
    CHECK_FALSE(router.receive(ControlRegistrationId{"registration-1"}, 8, completed));
    CHECK(router.receive(ControlRegistrationId{"registration-1"}, 7, completed));
    REQUIRE(outcome.wait_for(1s) == std::future_status::ready);
    CHECK(outcome.get().terminal_state == ControlReceiptState::Completed);
}

TEST_CASE("host router rejects forged or revoked artifact publication before broker storage") {
    ControlHostRouter router;
    std::mutex mutex;
    std::optional<ControlHostExecuteEnvelope> dispatched;
    std::atomic<unsigned> publications{0};
    std::atomic<ControlExecutionCheckpoint> checkpoint{ControlExecutionCheckpoint::Continue};
    REQUIRE(router.attach(
        ControlRegistrationId{"registration-1"}, 7, [&](const ControlEnvelope& envelope) {
            if (const auto* execute = std::get_if<ControlHostExecuteEnvelope>(&envelope.payload)) {
                std::lock_guard lock(mutex);
                dispatched = *execute;
            }
            return true;
        }));

    auto outcome = std::async(std::launch::async, [&] {
        return router.executor()(
            plan(future_deadline()), request(),
            {.report_progress = [](std::uint64_t, std::uint64_t, std::string) { return true; },
             .checkpoint = [&] { return checkpoint.load(); },
             .complete_deferred = [](ControlExecutionOutcome) {},
             .maximum_artifact_bytes = kControlHostMaximumArtifactPublicationBytes,
             .publish_artifact =
                 [&](std::span<const std::uint8_t>, ControlArtifactPublication) {
                     ++publications;
                     return ControlArtifactStoreResult{};
                 }});
    });
    std::string route;
    for (unsigned attempt = 0; attempt != 100 && route.empty(); ++attempt) {
        {
            std::lock_guard lock(mutex);
            if (dispatched)
                route = dispatched->route_id;
        }
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE_FALSE(route.empty());
    const ControlEnvelope forged{.payload = ControlHostCompleteEnvelope{
                                     .route_id = route,
                                     .terminal_state = ControlReceiptState::Completed,
                                     .detail_json = R"({"artifact_id":"artifact-forged"})",
                                     .artifact_publications =
                                         {{.reference_id = "artifact-forged",
                                           .bytes_base64 = "AQ==",
                                           .content_type = "application/octet-stream",
                                           .sensitivity = ControlArtifactSensitivity::Sensitive,
                                           .redaction_state =
                                               ControlArtifactRedactionState::Original,
                                           .lifetime_ms = 1000}},
                                 }};
    CHECK(router.receive(ControlRegistrationId{"registration-1"}, 7, forged));
    REQUIRE(outcome.wait_for(1s) == std::future_status::ready);
    const auto rejected = outcome.get();
    CHECK(rejected.terminal_state == ControlReceiptState::Failed);
    CHECK(rejected.result.result_code == ControlResultCode::InvalidRequest);
    CHECK(publications == 0);

    {
        std::lock_guard lock(mutex);
        dispatched.reset();
    }
    auto revoked_outcome = std::async(std::launch::async, [&] {
        return router.executor()(
            plan(future_deadline()), request(),
            {.report_progress = [](std::uint64_t, std::uint64_t, std::string) { return true; },
             .checkpoint = [&] { return checkpoint.load(); },
             .complete_deferred = [](ControlExecutionOutcome) {},
             .maximum_artifact_bytes = kControlHostMaximumArtifactPublicationBytes,
             .publish_artifact =
                 [&](std::span<const std::uint8_t>, ControlArtifactPublication) {
                     ++publications;
                     return ControlArtifactStoreResult{};
                 }});
    });
    route.clear();
    for (unsigned attempt = 0; attempt != 100 && route.empty(); ++attempt) {
        {
            std::lock_guard lock(mutex);
            if (dispatched)
                route = dispatched->route_id;
        }
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE_FALSE(route.empty());
    checkpoint.store(ControlExecutionCheckpoint::AuthorityRevoked);
    const ControlEnvelope revoked{.payload = ControlHostCompleteEnvelope{
                                      .route_id = route,
                                      .terminal_state = ControlReceiptState::Completed,
                                      .detail_json =
                                          R"({"artifact_id":"host-publication-1"})",
                                      .artifact_publications =
                                          {{.reference_id = "host-publication-1",
                                            .bytes_base64 = "AQ==",
                                            .content_type = "application/octet-stream",
                                            .sensitivity = ControlArtifactSensitivity::Sensitive,
                                            .redaction_state =
                                                ControlArtifactRedactionState::Original,
                                            .lifetime_ms = 1000}},
                                  }};
    CHECK(router.receive(ControlRegistrationId{"registration-1"}, 7, revoked));
    REQUIRE(revoked_outcome.wait_for(1s) == std::future_status::ready);
    CHECK(revoked_outcome.get().terminal_state == ControlReceiptState::Cancelled);
    CHECK(publications == 0);
}

TEST_CASE("host router projects exact opaque authority without sensitive client or grant ids") {
    ControlHostRouter router;
    std::string encoded;
    std::optional<ControlHostExecuteEnvelope> dispatched;
    REQUIRE(router.attach_slot(
        ControlRegistrationId{"registration-1"}, 11, "slot-1", "process-7/slot-3",
        [&](const ControlEnvelope& envelope) {
            if (const auto* execute = std::get_if<ControlHostExecuteEnvelope>(&envelope.payload)) {
                dispatched = *execute;
                encoded = encode_control_envelope(envelope);
                return false;
            }
            return true;
        }));
    const auto outcome = router.executor()(plan(future_deadline()), request(), context());
    REQUIRE(dispatched);
    CHECK(dispatched->authority_id.starts_with("authority-"));
    CHECK(dispatched->controller_authority_id.starts_with("controller-"));
    CHECK(dispatched->session_id == "session-1");
    CHECK(dispatched->publication_id == "process-7/slot-3");
    CHECK(dispatched->manifest_digest == std::string(64, 'a'));
    CHECK_FALSE(encoded.empty());
    CHECK(encoded.find("client-secret") == std::string::npos);
    CHECK(encoded.find("grant-secret") == std::string::npos);
    CHECK(encoded.find("peer-secret") == std::string::npos);
    CHECK(outcome.result.result_code == ControlResultCode::HostUnavailable);

    const auto first_authority = dispatched->authority_id;
    const auto first_controller = dispatched->controller_authority_id;
    auto second_grant = plan(future_deadline());
    second_grant.grant_id = ControlGrantId{"grant-secret-2"};
    (void)router.executor()(second_grant, request(), context());
    REQUIRE(dispatched);
    CHECK(dispatched->authority_id != first_authority);
    CHECK(dispatched->controller_authority_id == first_controller);

    auto other_client = plan(future_deadline());
    other_client.client_id = ControlClientId{"client-secret-2"};
    other_client.grant_id = ControlGrantId{"grant-secret-3"};
    (void)router.executor()(other_client, request(), context());
    REQUIRE(dispatched);
    CHECK(dispatched->controller_authority_id != first_controller);

    router.detach(ControlRegistrationId{"registration-1"}, 11);
    REQUIRE(router.attach_slot(
        ControlRegistrationId{"registration-1"}, 12, "slot-1", "process-7/slot-3",
        [&](const ControlEnvelope& envelope) {
            if (const auto* execute = std::get_if<ControlHostExecuteEnvelope>(&envelope.payload)) {
                dispatched = *execute;
                return false;
            }
            return true;
        }));
    (void)router.executor()(plan(future_deadline()), request(), context());
    REQUIRE(dispatched);
    CHECK(dispatched->controller_authority_id != first_controller);
}

TEST_CASE("host router reports unavailable before delivery and uncertainty after disconnect") {
    ControlHostRouter router;
    const auto execution = router.executor();
    CHECK(execution(plan(future_deadline()), request(), context()).result.result_code ==
          ControlResultCode::HostUnavailable);

    REQUIRE(router.attach(ControlRegistrationId{"registration-1"}, 4,
                          [](const ControlEnvelope&) { return true; }));
    auto outcome = std::async(std::launch::async, [&] {
        return execution(plan(future_deadline()), request(), context());
    });
    std::this_thread::sleep_for(10ms);
    router.detach(ControlRegistrationId{"registration-1"}, 4);
    REQUIRE(outcome.wait_for(1s) == std::future_status::ready);
    const auto result = outcome.get();
    CHECK(result.terminal_state == ControlReceiptState::UnknownNeedsRefresh);
    CHECK(result.result.retry == ControlRetryClassification::AfterRefresh);
}

TEST_CASE("host router returns promptly on cancellation and settles one late completion") {
    ControlHostRouter router;
    std::mutex mutex;
    std::optional<ControlHostExecuteEnvelope> dispatched;
    std::atomic<bool> cancelled{false};
    std::atomic<unsigned> cancel_frames{0};
    REQUIRE(router.attach(
        ControlRegistrationId{"registration-1"}, 9, [&](const ControlEnvelope& envelope) {
            std::lock_guard lock(mutex);
            if (const auto* execute = std::get_if<ControlHostExecuteEnvelope>(&envelope.payload))
                dispatched = *execute;
            if (std::holds_alternative<ControlHostCancelEnvelope>(envelope.payload))
                ++cancel_frames;
            return true;
        }));

    std::promise<ControlExecutionOutcome> late_completion;
    std::atomic<unsigned> completion_count{0};
    auto outcome = std::async(std::launch::async, [&] {
        return router.executor()(
            plan(future_deadline()), request(),
            {.report_progress = [](std::uint64_t, std::uint64_t, std::string) { return true; },
             .checkpoint =
                 [&] {
                     return cancelled.load() ? ControlExecutionCheckpoint::Cancelled
                                             : ControlExecutionCheckpoint::Continue;
                 },
             .complete_deferred =
                 [&](ControlExecutionOutcome completed) {
                     if (completion_count.fetch_add(1) == 0)
                         late_completion.set_value(std::move(completed));
                 }});
    });

    std::string route;
    for (unsigned attempt = 0; attempt != 100 && route.empty(); ++attempt) {
        {
            std::lock_guard lock(mutex);
            if (dispatched)
                route = dispatched->route_id;
        }
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE_FALSE(route.empty());
    cancelled.store(true);
    REQUIRE(outcome.wait_for(1s) == std::future_status::ready);
    const auto immediate = outcome.get();
    CHECK(immediate.deferred);
    CHECK(immediate.terminal_state == ControlReceiptState::UnknownNeedsRefresh);
    CHECK(cancel_frames == 1);

    const ControlEnvelope completed{.payload = ControlHostCompleteEnvelope{
                                        .route_id = route,
                                        .terminal_state = ControlReceiptState::Completed,
                                    }};
    CHECK(router.receive(ControlRegistrationId{"registration-1"}, 9, completed));
    auto settled = late_completion.get_future();
    REQUIRE(settled.wait_for(1s) == std::future_status::ready);
    CHECK(settled.get().terminal_state == ControlReceiptState::Completed);
    CHECK_FALSE(router.receive(ControlRegistrationId{"registration-1"}, 9, completed));
    CHECK(completion_count == 1);
}
