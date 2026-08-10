#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/control_motion_executor.hpp>
#include <pulp/inspect/control_service.hpp>
#include <pulp/inspect/motion_inspector.hpp>
#include <pulp/inspect/motion_scrubber.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/motion.hpp>
#include <pulp/view/motion_cost.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>

#include <choc/text/choc_JSON.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace pulp::inspect;
using namespace std::chrono_literals;
namespace view = pulp::view;

namespace {

namespace fs = std::filesystem;

struct TemporaryDirectory {
    fs::path path;

    TemporaryDirectory() {
        const auto random = pulp::runtime::secure_random_bytes(8);
        REQUIRE(random.has_value());
        path = fs::temp_directory_path() /
               ("pulp-control-motion-" + pulp::runtime::hex_encode(*random));
        REQUIRE(fs::create_directory(path));
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

VerifiedControlPeerIdentity verified_peer(ControlPeerRole role, std::int64_t process_id,
                                          std::string start_id) {
    ControlPeerVerifier verifier([](const ControlPeerEvidence&) { return true; });
    auto verified = verifier.verify({
        .role = role,
        .user_id = "uid:501",
        .process_id = process_id,
        .process_start_id = std::move(start_id),
        .executable_identity = "signed:dev.pulp.control-motion-test",
        .publisher_id = "publisher.pulp",
    });
    REQUIRE(verified.has_value());
    return std::move(*verified);
}

std::shared_ptr<InspectorMainThreadRpc> inline_rpc() {
    return std::make_shared<InspectorMainThreadRpc>(
        InspectorMainThreadRpc::Config{1s, 8},
        [](std::function<void()> task) {
            task();
            return true;
        },
        [] { return false; });
}

ControlAdmissionPlan plan() {
    ControlAdmissionPlan value;
    value.client_id = ControlClientId{"client-a"};
    value.registration_id = ControlRegistrationId{"registration-a"};
    value.grant_id = ControlGrantId{"grant-a"};
    value.session_id = "session-a";
    value.instance_id = "instance-a";
    value.publication_id = "publication-a";
    value.instance_generation = "generation-a";
    value.capability = InspectorCapability::TraceControl;
    value.operation_id = "dev.pulp.trace/control@1";
    value.operation_version = 1;
    value.receipt_id = ControlReceiptId{"receipt-a"};
    value.deadline_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 (std::chrono::system_clock::now() + 5s).time_since_epoch())
                                 .count();
    return value;
}

ControlRequestEnvelope request(std::string params) {
    const auto admission = plan();
    return {.request_id = "request-a",
            .client_id = admission.client_id.value,
            .registration_id = admission.registration_id.value,
            .grant_id = admission.grant_id.value,
            .instance_generation = admission.instance_generation,
            .operation_id = admission.operation_id,
            .operation_version = admission.operation_version,
            .idempotency_key = "motion-a",
            .deadline_unix_ms = admission.deadline_unix_ms,
            .params_json = std::move(params)};
}

ControlExecutionOutcome invoke(const ControlOperationExecutor& executor, std::string params,
                               ControlExecutionGuard guard = [] {
                                   return ControlExecutionCheckpoint::Continue;
                               }) {
    return executor(plan(), request(std::move(params)),
                    ControlExecutionContext{.checkpoint = std::move(guard)});
}

struct MotionFixture {
    MotionFixture() {
        view::motion::Coordinator::instance().reset();
        view::motion::CostAttributor::instance().reset();
        view::motion::Coordinator::instance().bind(clock);
        root.set_id("root");
        auto child = std::make_unique<view::View>();
        child->set_id("card");
        root.add_child(std::move(child));
        auto scroll = std::make_unique<view::ScrollView>();
        scroll->set_id("scroll");
        root.add_child(std::move(scroll));
        inspector = std::make_unique<MotionInspector>(root);
    }

    ~MotionFixture() {
        inspector.reset();
        view::motion::Coordinator::instance().reset();
        view::motion::CostAttributor::instance().reset();
        if (!fixture_path.empty()) std::remove(fixture_path.c_str());
    }

    void load_replay_fixture() {
        fixture_path = "/tmp/pulp-control-motion-fixture-" +
                       std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".jsonl";
        const auto sink = view::motion::make_fixture_sink(fixture_path);
        sink({.kind = view::motion::SampleEvent::Kind::Baseline,
              .view_name = "Card", .metric_name = "opacity", .frame = 0});
        sink({.kind = view::motion::SampleEvent::Kind::Sample,
              .view_name = "Card", .metric_name = "opacity", .frame = 1});
        sink({.kind = view::motion::SampleEvent::Kind::End,
              .view_name = "Card", .metric_name = "opacity", .frame = 2});
        REQUIRE(scrubber.load_fixture(fixture_path));
    }

    ControlMotionTarget target() {
        return {.registration_id = ControlRegistrationId{"registration-a"},
                .host_tier = ControlHostTier::Standalone,
                .session_id = "session-a",
                .instance_id = "instance-a",
                .publication_id = "publication-a",
                .instance_generation = "generation-a",
                .authority_live = [authority = authority_live] { return *authority; },
                .subscribe_authority_end = [this](std::function<void()> callback) {
                    authority_end_callbacks.push_back(std::move(callback));
                    return std::shared_ptr<void>{std::make_shared<int>(0)};
                },
                .inspector = inspector.get(),
                .scrubber = &scrubber};
    }

    void revoke_authority() {
        *authority_live = false;
        auto callbacks = std::move(authority_end_callbacks);
        for (auto& callback : callbacks)
            callback();
    }

    view::FrameClock clock;
    view::View root;
    std::unique_ptr<MotionInspector> inspector;
    MotionScrubber scrubber;
    std::string fixture_path;
    std::shared_ptr<bool> authority_live = std::make_shared<bool>(true);
    std::vector<std::function<void()>> authority_end_callbacks;
};

std::unique_ptr<ControlMotionExecutor> adapter(MotionFixture& fixture,
                                               bool exact_target = true) {
    return ControlMotionExecutor::create({
        .main_thread_rpc = inline_rpc(),
        .resolve_target = [&fixture, exact_target](const ControlAdmissionPlan&) {
            auto target = fixture.target();
            if (!exact_target) target.session_id = "stale-session";
            return std::optional{std::move(target)};
        },
    });
}

} // namespace

TEST_CASE("canonical motion executor attaches geometry and scroll observation",
          "[inspect][control][motion][t1]") {
    MotionFixture fixture;
    auto owner = adapter(fixture);
    REQUIRE(owner);
    const auto outcome = invoke(owner->executor(), R"({
      "action":"motion-start-trace","view_name":"panel","fps":60,
      "metrics":[
        {"kind":"geometry","node_id":"card","properties":["minX","width"]},
        {"kind":"scroll-geometry","node_id":"scroll","properties":["contentOffsetY"]}
      ]})");
    REQUIRE(outcome.terminal_state == ControlReceiptState::Completed);
    const auto detail = choc::json::parse(outcome.result.detail_json);
    CHECK(detail["receipt_id"].getString() == std::string_view{"receipt-a"});
    const auto trace_id = detail["trace_id"].getInt64();
    CHECK(trace_id > 0);
    CHECK(fixture.inspector->active_trace_count() == 1);
    CHECK_FALSE(fixture.inspector->detach_trace(trace_id, "other-client"));
    const auto stopped =
        invoke(owner->executor(), "{\"action\":\"motion-stop-trace\",\"trace_id\":" +
                                      std::to_string(trace_id) + "}");
    CHECK(stopped.terminal_state == ControlReceiptState::Completed);
    CHECK(fixture.inspector->active_trace_count() == 0);
    CHECK_FALSE(view::motion::Coordinator::instance().tracing_enabled());

    auto stale = adapter(fixture, false);
    REQUIRE(stale);
    const auto rejected = invoke(stale->executor(), R"({"action":"motion-pause"})");
    CHECK(rejected.terminal_state == ControlReceiptState::Failed);
    CHECK(rejected.result.result_code == ControlResultCode::SessionStale);
}

TEST_CASE("canonical motion fixture replay is bounded and cancellation checked",
          "[inspect][control][motion][fixture]") {
    MotionFixture fixture;
    fixture.load_replay_fixture();
    auto owner = adapter(fixture);
    REQUIRE(owner);

    const auto played = invoke(owner->executor(),
                               R"({"action":"motion-play","maximum_events":2})");
    REQUIRE(played.terminal_state == ControlReceiptState::Completed);
    const auto detail = choc::json::parse(played.result.detail_json);
    CHECK(detail["emitted_count"].getInt64() == 2);
    CHECK(detail["truncated"].getBool());
    CHECK(detail["playing"].getBool());

    const auto paused = invoke(owner->executor(), R"({"action":"motion-pause"})");
    REQUIRE(paused.terminal_state == ControlReceiptState::Completed);
    CHECK_FALSE(choc::json::parse(paused.result.detail_json)["playing"].getBool());

    const auto cancelled_outcome = invoke(
        owner->executor(), R"({"action":"motion-scrub-to","frame":1})",
        [] { return ControlExecutionCheckpoint::AuthorityRevoked; });
    CHECK(cancelled_outcome.terminal_state == ControlReceiptState::Cancelled);

    std::size_t reentrant_events = 0;
    fixture.scrubber.add_sink([&](const auto&) {
        ++reentrant_events;
        fixture.revoke_authority();
    });
    const auto interrupted = invoke(owner->executor(), R"({"action":"motion-play"})");
    CHECK(interrupted.terminal_state == ControlReceiptState::UnknownNeedsRefresh);
    CHECK(reentrant_events == 1);
}

TEST_CASE("canonical motion cost outcome is finite bounded and redacted",
          "[inspect][control][motion][cost]") {
    MotionFixture fixture;
    auto owner = adapter(fixture);
    REQUIRE(owner);
    auto unrelated_builder = view::motion::Coordinator::instance().trace("unrelated");
    unrelated_builder.value("unrelated", [] { return 1.0; });
    auto unrelated = unrelated_builder.attach();
    REQUIRE(unrelated.is_attached());
    view::motion::CostAttributor::instance().set_probe([] {
        return view::motion::RenderCostSnapshot{12.5, 400.0, 2};
    });
    view::motion::CostAttributor::instance().set_enabled(true);
    fixture.clock.tick(1.0 / 60.0);
    const auto enabled = invoke(owner->executor(), R"({"action":"motion-enable-cost"})");
    REQUIRE(enabled.terminal_state == ControlReceiptState::Completed);
    const auto isolated = invoke(
        owner->executor(), R"({"action":"motion-sample-cost","maximum_samples":1})");
    REQUIRE(isolated.terminal_state == ControlReceiptState::Completed);
    CHECK(choc::json::parse(isolated.result.detail_json)["samples"].size() == 0);
    const auto started = invoke(
        owner->executor(),
        R"({"action":"motion-start-trace","metrics":[{"kind":"geometry","node_id":"card"}]})");
    REQUIRE(started.terminal_state == ControlReceiptState::Completed);
    const auto controlled_trace_id =
        choc::json::parse(started.result.detail_json)["trace_id"].getInt64();
    fixture.clock.tick(1.0 / 60.0);

    const auto sampled = invoke(
        owner->executor(), R"({"action":"motion-sample-cost","maximum_samples":1})");
    REQUIRE(sampled.terminal_state == ControlReceiptState::Completed);
    const auto detail = choc::json::parse(sampled.result.detail_json);
    CHECK(detail["redacted"].getBool());
    REQUIRE(detail["samples"].size() == 1);
    CHECK(detail["samples"][0]["render_pass_duration_ms"].getFloat64() == 12.5);
    REQUIRE(detail["samples"][0]["active_trace_ids"].size() == 1);
    CHECK(detail["samples"][0]["active_trace_ids"][0].getInt64() == controlled_trace_id);
    CHECK(detail["samples"][0]["active_provenance"].size() == 0);
    const auto stopped =
        invoke(owner->executor(), "{\"action\":\"motion-stop-trace\",\"trace_id\":" +
                                      std::to_string(controlled_trace_id) + "}");
    REQUIRE(stopped.terminal_state == ControlReceiptState::Completed);
    const auto retained = invoke(
        owner->executor(), R"({"action":"motion-sample-cost","maximum_samples":1})");
    REQUIRE(retained.terminal_state == ControlReceiptState::Completed);
    const auto retained_detail = choc::json::parse(retained.result.detail_json);
    const auto retained_sample = retained_detail["samples"][0];
    REQUIRE(retained_sample["active_trace_ids"].size() == 1);
    CHECK(retained_sample["active_trace_ids"][0].getInt64() == controlled_trace_id);
    const auto disabled = invoke(owner->executor(), R"({"action":"motion-disable-cost"})");
    REQUIRE(disabled.terminal_state == ControlReceiptState::Completed);
    CHECK(view::motion::CostAttributor::instance().enabled());
}

TEST_CASE("canonical motion trace quota bounds persistent frame work",
          "[inspect][control][motion][quota][security]") {
    MotionFixture fixture;
    auto owner = adapter(fixture);
    REQUIRE(owner);
    for (std::size_t index = 0; index < 32; ++index) {
        const auto started = invoke(
            owner->executor(),
            R"({"action":"motion-start-trace","metrics":[{"kind":"geometry","node_id":"card"}]})");
        INFO(index);
        REQUIRE(started.terminal_state == ControlReceiptState::Completed);
    }
    const auto exhausted = invoke(
        owner->executor(),
        R"({"action":"motion-start-trace","metrics":[{"kind":"geometry","node_id":"card"}]})");
    CHECK(exhausted.terminal_state == ControlReceiptState::Failed);
    CHECK(exhausted.result.result_code == ControlResultCode::ResourceExhausted);
    CHECK(fixture.inspector->active_trace_count() == 32);
    fixture.revoke_authority();
    CHECK(fixture.inspector->active_trace_count() == 0);
    CHECK_FALSE(view::motion::Coordinator::instance().tracing_enabled());
}

TEST_CASE("control service dispatches canonical motion through admitted exact authority",
          "[inspect][control][motion][service][t1]") {
    MotionFixture fixture;
    TemporaryDirectory temporary;
    ControlBrokerConfig broker_config;
    broker_config.operation_store = {.directory = temporary.path / "receipts"};
    broker_config.artifact_store = {.root = temporary.path / "artifacts"};
    broker_config.admission.host_available = [](const auto&, const auto&) { return true; };
    broker_config.admission.activated = [](const auto&, const auto&) { return true; };
    broker_config.admission.policy_eligible = [](const auto&, const auto&) { return true; };
    ControlBroker broker{std::move(broker_config)};

    const auto client = verified_peer(ControlPeerRole::Client, 101, "client-start");
    const auto host = verified_peer(ControlPeerRole::StandaloneHost, 201, "host-start");
    const auto ticket = broker.issue_bootstrap(client);
    REQUIRE(ticket.ticket);
    const auto connected =
        broker.redeem_bootstrap(ticket.ticket->ticket_id, ticket.ticket->secret.bytes(), client);
    REQUIRE(connected.client);

    ControlManifest manifest;
    manifest.profile = ControlBuildProfile::DeveloperLocal;
    manifest.target = "ControlMotionFixture";
    manifest.product_name = "Control Motion Fixture";
    manifest.bundle_id = "dev.pulp.control-motion-fixture";
    manifest.build_id = "build:0123456789abcdef0123456789abcdef";
    manifest.endpoint_included = true;
    manifest.capabilities = {InspectorCapability::SessionControl,
                             InspectorCapability::TraceControl};
    const auto registered = broker.register_instance(
        host, {.host_tier = ControlHostTier::Standalone,
               .session_id = "session-a",
               .instance_id = "instance-a",
               .publication_id = "publication-a",
               .manifest = std::move(manifest),
               .artifact_digest = std::string(64, 'a')});
    REQUIRE(registered.registration);
    const auto granted = broker.issue_grant(
        client,
        {.client_id = connected.client->client_id,
         .registration_id = registered.registration->registration_id,
         .capabilities = {InspectorCapability::TraceControl},
         .ttl = 5min},
        {.approved = true,
         .authority = ControlConsentAuthority::TrustedPulpCli,
         .decision_id = "decision-motion"});
    REQUIRE(granted.grant);

    auto owner = ControlMotionExecutor::create({
        .main_thread_rpc = inline_rpc(),
        .resolve_target = [&](const ControlAdmissionPlan& plan) {
            auto target = fixture.target();
            target.registration_id = registered.registration->registration_id;
            target.instance_generation = registered.registration->publication_id;
            target.authority_live = [&broker, &client, plan] {
                return broker.revalidate_operation(client, plan);
            };
            return std::optional{std::move(target)};
        },
    });
    REQUIRE(owner);
    ControlService service{broker, owner->executor()};
    auto session = service.open_session(client, connected.client->client_id);
    REQUIRE(session.is_open());
    const auto negotiated = session.dispatch(encode_control_envelope({
        .schema_version = kControlProtocolVersion,
        .payload = ControlNegotiationOffer{
            .versions = {1, 1},
            .mandatory_features = {"receipts"},
            .optional_features = {"cancellation"},
        },
    }));
    REQUIRE(negotiated.status == ControlServiceStatus::Responded);

    ControlRequestEnvelope operation{
        .request_id = "request-service-motion",
        .client_id = connected.client->client_id.value,
        .registration_id = registered.registration->registration_id.value,
        .grant_id = granted.grant->grant_id.value,
        .instance_generation = registered.registration->publication_id,
        .operation_id = "dev.pulp.trace/control@1",
        .operation_version = 1,
        .idempotency_key = "motion-service-a",
        .deadline_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                (std::chrono::system_clock::now() + 5s).time_since_epoch())
                                .count(),
        .params_json = R"({"action":"motion-start-trace","view_name":"panel","fps":60,"metrics":[{"kind":"geometry","node_id":"card","properties":["width"]}]})",
    };
    operation.request_hash = *control_request_hash(operation);
    const auto completed = session.dispatch(encode_control_envelope({
        .schema_version = kControlProtocolVersion,
        .payload = operation,
    }));
    REQUIRE(completed.status == ControlServiceStatus::Responded);
    REQUIRE(completed.response);
    const auto* receipt = std::get_if<ControlReceiptEnvelope>(&completed.response->payload);
    REQUIRE(receipt);
    CHECK(receipt->state == ControlReceiptState::Completed);
    CHECK(fixture.inspector->active_trace_count() == 1);
}
