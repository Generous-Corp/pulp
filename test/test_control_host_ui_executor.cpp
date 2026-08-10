#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/capture_source.hpp>
#include <pulp/inspect/control_host_ui_executor.hpp>
#include <pulp/inspect/control_standalone_ui_adapter.hpp>
#include <pulp/inspect/main_thread_rpc.hpp>
#include <pulp/inspect/runtime_evaluator.hpp>
#include <pulp/view/pointer_dispatch.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/window_host.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using namespace pulp::inspect;

namespace {

void append_png_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value >> 24));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    bytes.push_back(static_cast<std::uint8_t>(value));
}

void append_png_chunk(std::vector<std::uint8_t>& bytes, std::string_view type,
                      std::span<const std::uint8_t> data) {
    append_png_u32(bytes, static_cast<std::uint32_t>(data.size()));
    bytes.insert(bytes.end(), type.begin(), type.end());
    bytes.insert(bytes.end(), data.begin(), data.end());
    append_png_u32(bytes, 0); // Parser preserves but does not reinterpret the encoder's CRC.
}

std::vector<std::uint8_t> fixture_capture_png(std::uint32_t width, std::uint32_t height) {
    std::vector<std::uint8_t> bytes{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<std::uint8_t> header;
    append_png_u32(header, width);
    append_png_u32(header, height);
    header.insert(header.end(), {8, 6, 0, 0, 0});
    append_png_chunk(bytes, "IHDR", header);
    constexpr std::array<std::uint8_t, 6> metadata{'s', 'e', 'c', 'r', 'e', 't'};
    append_png_chunk(bytes, "tEXt", metadata);
    constexpr std::array<std::uint8_t, 1> image_data{0};
    append_png_chunk(bytes, "IDAT", image_data);
    append_png_chunk(bytes, "IEND", {});
    return bytes;
}

struct PostedQueue {
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<std::function<void()>> tasks;

    bool post(std::function<void()> task) {
        {
            std::lock_guard lock(mutex);
            tasks.push_back(std::move(task));
        }
        ready.notify_all();
        return true;
    }

    std::function<void()> take() {
        std::unique_lock lock(mutex);
        if (!ready.wait_for(lock, 2s, [&] { return !tasks.empty(); }))
            return {};
        auto task = std::move(tasks.front());
        tasks.erase(tasks.begin());
        return task;
    }
};

class CaptureSource final : public InspectorCaptureSource {
  public:
    InspectorCapture capture_png() override {
        ++calls;
        return {.png = fixture_capture_png(64, 32), .width = 64, .height = 32};
    }
    int calls = 0;
};

class TargetAdapter final : public ControlHostUiTargetAdapter {
  public:
    InspectorCapture capture_node_png(const ControlUiExactTarget& target) override {
        ++capture_calls;
        last_target = target;
        return {.png = fixture_capture_png(24, 12), .width = 24, .height = 12};
    }

    ControlUiApplyStatus dispatch_input(const ControlUiExactTarget& target,
                                        const ControlUiAuthorityOwner& owner,
                                        const ControlUiInput& input) override {
        ++input_calls;
        last_target = target;
        last_owner = owner;
        last_input = input;
        return status;
    }

    void release_controller(const std::optional<ControlUiAuthorityOwner>& owner) noexcept override {
        ++release_calls;
        released_owner = owner;
    }

    ControlUiApplyStatus status = ControlUiApplyStatus::Applied;
    ControlUiExactTarget last_target;
    ControlUiAuthorityOwner last_owner;
    std::optional<ControlUiAuthorityOwner> released_owner;
    ControlUiInput last_input;
    int capture_calls = 0;
    int input_calls = 0;
    int release_calls = 0;
};

class TestWindowHost final : public pulp::view::WindowHost {
  public:
    void show() override {}
    void hide() override {}
    bool is_visible() const override { return true; }
    void repaint() override { ++repaint_calls; }
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}

    int repaint_calls = 0;
};

class InputNode final : public pulp::view::View {
  public:
    bool wants_mouse_input() const override { return true; }
    bool accepts_text_input() const override { return true; }
    void on_mouse_down(pulp::view::Point) override { ++down_calls; }
    void on_mouse_up(pulp::view::Point) override { ++up_calls; }
    void on_mouse_cancel(pulp::view::Point) override { ++cancel_calls; }
    bool on_key_event(const pulp::view::KeyEvent&) override {
        ++key_calls;
        return true;
    }
    void on_text_input(const pulp::view::TextInputEvent& event) override {
        text += event.text;
    }

    int down_calls = 0;
    int up_calls = 0;
    int cancel_calls = 0;
    int key_calls = 0;
    std::string text;
};

class SelfUnmountNode final : public pulp::view::View {
  public:
    SelfUnmountNode(pulp::view::View& root, std::unique_ptr<pulp::view::View>& detached)
        : root_(root), detached_(detached) {}
    bool wants_mouse_input() const override { return true; }
    void on_mouse_event(const pulp::view::MouseEvent&) override {
        ++modern_down_calls;
        detached_ = root_.remove_child(this);
    }

    int modern_down_calls = 0;

  private:
    pulp::view::View& root_;
    std::unique_ptr<pulp::view::View>& detached_;
};

class Evaluator final : public RuntimeEvaluator {
  public:
    RuntimeEvaluatorCapabilities capabilities() const override {
        return {.engine = "fixture", .can_evaluate = true, .can_interrupt = true};
    }
    RuntimeEvaluationResult evaluate(std::string_view code, std::chrono::milliseconds timeout,
                                     std::size_t maximum_result_bytes) override {
        ++calls;
        last_source = code;
        last_timeout = timeout;
        last_maximum_result_bytes = maximum_result_bytes;
        while (block_until_interrupted && !interrupted.load())
            std::this_thread::sleep_for(1ms);
        if (interrupted.load())
            return {.timed_out = true};
        return {.ok = true, .json = R"({"secret":"bounded"})"};
    }
    bool interrupt() override {
        ++interrupt_calls;
        interrupted = true;
        return true;
    }
    std::string_view binary_marker() const noexcept override {
        return "PULP_INSPECT_RUNTIME_EVAL_HIGH_RISK_COMPONENT_V1";
    }
    int calls = 0;
    std::string last_source;
    std::chrono::milliseconds last_timeout{};
    std::size_t last_maximum_result_bytes = 0;
    bool block_until_interrupted = false;
    std::atomic<bool> interrupted{false};
    std::atomic<int> interrupt_calls{0};
};

ControlHostUiBinding binding(ControlBuildProfile profile = ControlBuildProfile::DeveloperLocal) {
    ControlManifest manifest;
    manifest.profile = profile;
    manifest.target = "ControlHostUiTest";
    manifest.product_name = "Control Host UI Test";
    manifest.bundle_id = "dev.pulp.test.control-host-ui";
    manifest.build_id = "build:0123456789abcdef0123456789abcdef";
    manifest.endpoint_included = true;
    manifest.unsafe_runtime_eval_acknowledged = profile == ControlBuildProfile::ResearchUnsafe;
    manifest.capabilities =
        profile == ControlBuildProfile::ResearchUnsafe
            ? std::vector{InspectorCapability::SessionControl, InspectorCapability::RuntimeEval}
            : std::vector{InspectorCapability::SessionControl, InspectorCapability::CaptureImage,
                          InspectorCapability::UiInput};
    const auto manifest_digest = control_manifest_digest(manifest);
    const auto artifact_digest = std::string(64, 'c');
    ControlRegistration registration;
    registration.registration_id = ControlRegistrationId{"registration-a"};
    registration.broker_id = ControlBrokerId{"broker-a"};
    registration.session_id = "session-a";
    registration.instance_id = "instance-a";
    registration.publication_id = "publication-a";
    registration.manifest_digest = manifest_digest;
    registration.artifact_digest = artifact_digest;
    registration.consent_identity = control_consent_identity(manifest_digest, artifact_digest);
    registration.profile = profile;
    registration.capabilities = manifest.capabilities;
    return {.registration = std::move(registration), .manifest = std::move(manifest)};
}

ControlAdmissionPlan plan(InspectorCapability capability, std::string operation) {
    ControlAdmissionPlan value;
    value.broker_id = ControlBrokerId{"broker-a"};
    value.client_principal = "principal-a";
    value.client_id = ControlClientId{"client-a"};
    value.registration_id = ControlRegistrationId{"registration-a"};
    value.grant_id = ControlGrantId{"grant-a"};
    value.session_id = "session-a";
    value.instance_id = "instance-a";
    value.publication_id = "publication-a";
    value.instance_generation = "publication-a";
    value.capability = capability;
    value.operation_id = std::move(operation);
    value.operation_version = 1;
    value.consent_decision_id = "single-use-consent-a";
    value.manifest_digest =
        control_manifest_digest(binding(capability == InspectorCapability::RuntimeEval
                                            ? ControlBuildProfile::ResearchUnsafe
                                            : ControlBuildProfile::DeveloperLocal)
                                    .manifest);
    value.producer_artifact_digest = std::string(64, 'c');
    value.deadline_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 (std::chrono::system_clock::now() + 5s).time_since_epoch())
                                 .count();
    value.receipt_id = ControlReceiptId{"receipt-a"};
    return value;
}

ControlRequestEnvelope request(const ControlAdmissionPlan& admission, std::string params) {
    return {.request_id = "request-a",
            .client_id = admission.client_id.value,
            .registration_id = admission.registration_id.value,
            .grant_id = admission.grant_id.value,
            .instance_generation = admission.instance_generation,
            .operation_id = admission.operation_id,
            .operation_version = admission.operation_version,
            .deadline_unix_ms = admission.deadline_unix_ms,
            .params_json = std::move(params)};
}

ControlExecutionOutcome run_on_worker(const ControlOperationExecutor& executor,
                                      const ControlAdmissionPlan& admission,
                                      const ControlRequestEnvelope& envelope,
                                      ControlExecutionContext context, PostedQueue& queue) {
    ControlExecutionOutcome result;
    std::thread worker([&] { result = executor(admission, envelope, context); });
    auto task = queue.take();
    REQUIRE(task);
    task();
    worker.join();
    return result;
}

std::shared_ptr<InspectorMainThreadRpc> rpc(PostedQueue& queue) {
    return std::make_shared<InspectorMainThreadRpc>(
        InspectorMainThreadRpc::Config{2s, 4},
        [&](auto task) { return queue.post(std::move(task)); }, [] { return false; });
}

struct TestProjectedAuthority {
    bool live = true;
    std::function<void()> ended;
};

ControlUiAuthorityResolver authority_resolver(
    const std::shared_ptr<TestProjectedAuthority>& authority) {
    return [authority](const ControlAdmissionPlan&)
        -> std::optional<ControlUiProjectedAuthority> {
        if (!authority->live)
            return std::nullopt;
        return ControlUiProjectedAuthority{
            .owner = {.authority_id = "opaque-authority-a"},
            .authority_live = [authority] { return authority->live; },
            .subscribe_authority_end = [authority](std::function<void()> callback) {
                authority->ended = std::move(callback);
                return std::static_pointer_cast<void>(std::make_shared<int>(1));
            }};
    };
}

} // namespace

TEST_CASE("host UI capture publishes one exact-lineage sensitive PNG artifact",
          "[inspect][control][ui][capture][artifact][security]") {
    PostedQueue queue;
    auto source = std::make_shared<CaptureSource>();
    auto executor = ControlHostUiExecutor::create(
        {.binding = binding(), .main_thread_rpc = rpc(queue), .capture_source = source});
    REQUIRE(executor);
    const auto admission = plan(InspectorCapability::CaptureImage, "dev.pulp.ui/capture@1");
    const auto envelope = request(admission, R"({"target":"window","format":"png"})");
    bool published = false;
    const auto outcome = run_on_worker(
        executor->executor(), admission, envelope,
        {.checkpoint = [] { return ControlExecutionCheckpoint::Continue; },
         .maximum_artifact_bytes = 1024,
         .publish_artifact =
             [&](std::span<const std::uint8_t> bytes, ControlArtifactPublication publication) {
                 published = bytes.size() < fixture_capture_png(64, 32).size() &&
                             std::search(bytes.begin(), bytes.end(),
                                         reinterpret_cast<const std::uint8_t*>("secret"),
                                         reinterpret_cast<const std::uint8_t*>("secret") + 6) ==
                                 bytes.end() &&
                             publication.content_type == "image/png" &&
                             publication.sensitivity == ControlArtifactSensitivity::Sensitive &&
                             publication.redaction_state == ControlArtifactRedactionState::Redacted;
                 ControlArtifactMetadata metadata;
                 metadata.artifact_id = "artifact-0123456789abcdef0123456789abcdef";
                 metadata.sha256 = std::string(64, 'b');
                 metadata.byte_size = bytes.size();
                 metadata.content_type = publication.content_type;
                 return ControlArtifactStoreResult{ControlArtifactStatus::Stored,
                                                   std::move(metadata)};
             }},
        queue);
    REQUIRE(outcome.terminal_state == ControlReceiptState::Completed);
    REQUIRE(published);
    REQUIRE(source->calls == 1);
    REQUIRE(outcome.result.artifacts.size() == 1);
    const auto detail = choc::json::parse(outcome.result.detail_json);
    CHECK(detail["width"].getInt64() == 64);
    CHECK(detail["height"].getInt64() == 32);
    CHECK(detail["redaction_state"].getString() == "redacted");

    auto wrong_plan = admission;
    wrong_plan.session_id = "session-b";
    const auto denied = executor->executor()(
        wrong_plan, envelope, {.checkpoint = [] { return ControlExecutionCheckpoint::Continue; }});
    CHECK(denied.result.result_code == ControlResultCode::PolicyDenied);
    CHECK(source->calls == 1);
}

TEST_CASE("host UI executor captures and drives one exact-generation node",
          "[inspect][control][ui][input][node][security]") {
    PostedQueue queue;
    auto source = std::make_shared<CaptureSource>();
    auto target = std::make_shared<TargetAdapter>();
    auto authority = std::make_shared<TestProjectedAuthority>();
    auto executor = ControlHostUiExecutor::create(
        {.binding = binding(),
         .main_thread_rpc = rpc(queue),
         .capture_source = source,
         .target_adapter = target,
         .resolve_authority = authority_resolver(authority),
         .view_generation = "view-a"});
    REQUIRE(executor);
    auto admission = plan(InspectorCapability::CaptureImage, "dev.pulp.ui/capture@1");
    auto outcome = run_on_worker(
        executor->executor(), admission,
        request(admission,
                R"({"target":"node","format":"png","node_id":"gain","view_generation":"view-a"})"),
        {.checkpoint = [] { return ControlExecutionCheckpoint::Continue; },
         .maximum_artifact_bytes = 1024,
         .publish_artifact =
             [](std::span<const std::uint8_t> bytes, ControlArtifactPublication publication) {
                 ControlArtifactMetadata metadata;
                 metadata.artifact_id = "artifact-node-0123456789abcdef0123456789";
                 metadata.sha256 = std::string(64, 'd');
                 metadata.byte_size = bytes.size();
                 metadata.content_type = publication.content_type;
                 return ControlArtifactStoreResult{ControlArtifactStatus::Stored,
                                                   std::move(metadata)};
             }},
        queue);
    REQUIRE(outcome.terminal_state == ControlReceiptState::Completed);
    CHECK(target->capture_calls == 1);
    CHECK(target->last_target.instance_id == "instance-a");
    CHECK(target->last_target.instance_generation == "publication-a");
    CHECK(target->last_target.view_generation == "view-a");
    CHECK(target->last_target.node_id == "gain");
    CHECK(source->calls == 0);

    outcome = run_on_worker(
        executor->executor(), admission,
        request(admission, R"({"target":"window","format":"png","raw_method":"capture"})"),
        {.checkpoint = [] { return ControlExecutionCheckpoint::Continue; }}, queue);
    CHECK(outcome.result.result_code == ControlResultCode::InvalidRequest);
    CHECK(source->calls == 0);

    admission = plan(InspectorCapability::UiInput, "dev.pulp.ui/input@1");
    outcome = run_on_worker(
        executor->executor(), admission,
        request(admission,
                R"({"kind":"focus","target_id":"gain","view_generation":"view-a","event":{"focused":true}})"),
        {.checkpoint = [] { return ControlExecutionCheckpoint::Continue; }}, queue);
    REQUIRE(outcome.terminal_state == ControlReceiptState::Completed);
    REQUIRE(target->input_calls == 1);
    CHECK(std::get<ControlUiFocusInput>(target->last_input).focused);
    CHECK(target->last_owner.authority_id == "opaque-authority-a");
    CHECK(control_ui_input_disposition() == "supported-exact-target-grant-controlled");

    outcome = run_on_worker(
        executor->executor(), admission,
        request(admission,
                R"({"kind":"text","target_id":"gain","view_generation":"stale","event":{"text":"safe"}})"),
        {.checkpoint = [] { return ControlExecutionCheckpoint::Continue; }}, queue);
    CHECK(outcome.result.result_code == ControlResultCode::SessionStale);
    CHECK(target->input_calls == 1);

    REQUIRE(authority->ended);
    std::thread authority_end_thread([&] {
        authority->live = false;
        authority->ended();
    });
    auto authority_release_task = queue.take();
    REQUIRE(authority_release_task);
    authority_release_task();
    authority_end_thread.join();
    REQUIRE(target->released_owner);
    CHECK(target->released_owner->authority_id == "opaque-authority-a");
    CHECK(target->release_calls == 1);

    bool disconnected = false;
    std::thread disconnect_thread([&] { disconnected = executor->disconnect(); });
    auto release_task = queue.take();
    REQUIRE(release_task);
    release_task();
    disconnect_thread.join();
    CHECK(disconnected);
    CHECK(target->release_calls == 2);
    CHECK_FALSE(target->released_owner);
}

TEST_CASE("host UI input is bounded and releases controller state after revocation",
          "[inspect][control][ui][input][bounds][cancellation][security]") {
    PostedQueue queue;
    auto target = std::make_shared<TargetAdapter>();
    auto authority = std::make_shared<TestProjectedAuthority>();
    auto executor = ControlHostUiExecutor::create(
        {.binding = binding(),
         .main_thread_rpc = rpc(queue),
         .target_adapter = target,
         .resolve_authority = authority_resolver(authority),
         .view_generation = "view-a"});
    REQUIRE(executor);
    const auto admission = plan(InspectorCapability::UiInput, "dev.pulp.ui/input@1");
    auto outcome = run_on_worker(
        executor->executor(), admission,
        request(admission,
                R"({"kind":"pointer","target_id":"gain","view_generation":"view-a","event":{"phase":"down","x":10,"y":20,"button":1}})"),
        {.checkpoint = [&] {
             return target->input_calls > 0 ? ControlExecutionCheckpoint::AuthorityRevoked
                                            : ControlExecutionCheckpoint::Continue;
         }},
        queue);
    INFO(outcome.result.explanation);
    CHECK(outcome.terminal_state == ControlReceiptState::Cancelled);
    CHECK(target->input_calls == 1);
    CHECK(target->release_calls == 1);
    REQUIRE(target->released_owner);
    CHECK(target->released_owner->authority_id == "opaque-authority-a");

    outcome = run_on_worker(
        executor->executor(), admission,
        request(admission,
                R"({"kind":"pointer","target_id":"gain","view_generation":"view-a","event":{"phase":"move","x":1000001,"y":20,"button":1}})"),
        {.checkpoint = [] { return ControlExecutionCheckpoint::Continue; }}, queue);
    CHECK(outcome.result.result_code == ControlResultCode::InvalidRequest);
    CHECK(target->input_calls == 1);

    const std::string oversized_text(kControlUiMaximumTextBytes + 1, 'x');
    outcome = run_on_worker(
        executor->executor(), admission,
        request(admission,
                "{\"kind\":\"text\",\"target_id\":\"gain\",\"view_generation\":\"view-a\",\"event\":{\"text\":\"" +
                    oversized_text + "\"}}"),
        {.checkpoint = [] { return ControlExecutionCheckpoint::Continue; }}, queue);
    CHECK(outcome.result.result_code == ControlResultCode::InvalidRequest);
    CHECK(target->input_calls == 1);
}

TEST_CASE("host UI disconnect surfaces unfenced controller cleanup",
          "[inspect][control][ui][disconnect][main-thread][security]") {
    auto target = std::make_shared<TargetAdapter>();
    auto unavailable_rpc = std::make_shared<InspectorMainThreadRpc>(
        InspectorMainThreadRpc::Config{10ms, 1}, [](auto) { return false; }, [] { return false; });
    auto authority = std::make_shared<TestProjectedAuthority>();
    auto executor = ControlHostUiExecutor::create(
        {.binding = binding(),
         .main_thread_rpc = unavailable_rpc,
         .target_adapter = target,
         .resolve_authority = authority_resolver(authority),
         .view_generation = "view-a"});
    REQUIRE(executor);
    CHECK_FALSE(executor->disconnect());
    CHECK(target->release_calls == 0);
}

TEST_CASE("runtime evaluation requires research acknowledgement and returns bounded JSON",
          "[inspect][control][runtime-eval][consent][security]") {
    PostedQueue queue;
    auto evaluator = std::make_shared<Evaluator>();
    auto unsafe = binding(ControlBuildProfile::ResearchUnsafe);
    auto executor = ControlHostUiExecutor::create(
        {.binding = unsafe,
         .main_thread_rpc = rpc(queue),
         .runtime_evaluator = evaluator,
         .redact_runtime_eval_result = [](std::string_view) -> std::optional<std::string> {
             return R"({"secret":"[redacted]","nested":{"token":"[redacted]"}})";
         }});
    REQUIRE(executor);
    const auto admission = plan(InspectorCapability::RuntimeEval, "dev.pulp.runtime/evaluate@1");
    const auto envelope = request(
        admission,
        R"json({"source":"({ answer: 42 })","timeout_ms":1000,"idempotency_key":"eval-a"})json");
    const auto outcome =
        run_on_worker(executor->executor(), admission, envelope,
                      {.checkpoint = [] { return ControlExecutionCheckpoint::Continue; }}, queue);
    REQUIRE(outcome.terminal_state == ControlReceiptState::Completed);
    REQUIRE(evaluator->calls == 1);
    CHECK(evaluator->last_source == "({ answer: 42 })");
    CHECK(evaluator->last_timeout > 0ms);
    CHECK(evaluator->last_timeout <= 1000ms);
    CHECK(evaluator->last_maximum_result_bytes == kControlRuntimeEvalMaximumResultBytes);
    const auto detail = choc::json::parse(outcome.result.detail_json);
    CHECK(detail["receipt_id"].getString() == "receipt-a");
    const auto redacted = choc::json::parse(detail["result_json"].getString());
    CHECK(redacted["secret"].getString() == "[redacted]");
    CHECK(redacted["nested"]["token"].getString() == "[redacted]");

    unsafe.manifest.unsafe_runtime_eval_acknowledged = false;
    CHECK_FALSE(
        ControlHostUiExecutor::create({.binding = unsafe,
                                       .main_thread_rpc = rpc(queue),
                                       .runtime_evaluator = evaluator,
                                       .redact_runtime_eval_result = [](std::string_view value) {
                                           return std::optional<std::string>{std::string(value)};
                                       }}));
}

TEST_CASE("runtime evaluation interrupts on broker cancellation",
          "[inspect][control][runtime-eval][cancellation][security]") {
    PostedQueue queue;
    auto evaluator = std::make_shared<Evaluator>();
    evaluator->block_until_interrupted = true;
    auto executor =
        ControlHostUiExecutor::create({.binding = binding(ControlBuildProfile::ResearchUnsafe),
                                       .main_thread_rpc = rpc(queue),
                                       .runtime_evaluator = evaluator,
                                       .redact_runtime_eval_result = [](std::string_view value) {
                                           return std::optional<std::string>{std::string(value)};
                                       }});
    REQUIRE(executor);
    const auto admission = plan(InspectorCapability::RuntimeEval, "dev.pulp.runtime/evaluate@1");
    std::atomic<int> checkpoints{0};
    const auto outcome = run_on_worker(
        executor->executor(), admission,
        request(admission,
                R"({"source":"while(true){}","timeout_ms":1000,"idempotency_key":"eval-cancel"})"),
        {.checkpoint =
             [&] {
                 return ++checkpoints >= 3 ? ControlExecutionCheckpoint::Cancelled
                                           : ControlExecutionCheckpoint::Continue;
             }},
        queue);
    CHECK(outcome.terminal_state == ControlReceiptState::Cancelled);
    CHECK(evaluator->interrupt_calls == 1);
}

TEST_CASE("host UI work is cancelled before main-thread capture applies",
          "[inspect][control][ui][cancellation]") {
    PostedQueue queue;
    auto source = std::make_shared<CaptureSource>();
    auto executor = ControlHostUiExecutor::create(
        {.binding = binding(), .main_thread_rpc = rpc(queue), .capture_source = source});
    REQUIRE(executor);
    const auto admission = plan(InspectorCapability::CaptureImage, "dev.pulp.ui/capture@1");
    const auto outcome =
        run_on_worker(executor->executor(), admission,
                      request(admission, R"({"target":"window","format":"png"})"),
                      {.checkpoint = [] { return ControlExecutionCheckpoint::Cancelled; }}, queue);
    CHECK(outcome.terminal_state == ControlReceiptState::Cancelled);
    CHECK(source->calls == 0);
}

TEST_CASE("Standalone UI adapter retains exact node and releases controller input",
          "[inspect][control][ui][standalone][security]") {
    pulp::view::View root;
    root.set_bounds({0, 0, 200, 100});
    TestWindowHost window;
    root.set_window_host(&window);

    auto child = std::make_unique<InputNode>();
    auto* node = child.get();
    node->set_id("gain");
    node->set_bounds({10, 10, 80, 40});
    node->set_focusable(true);
    root.add_child(std::move(child));

    auto adapter = ControlStandaloneUiAdapter::create(
        {.root = root,
         .window = window,
         .instance_id = "instance-a",
         .instance_generation = "publication-a",
         .view_generation = "view-a"});
    REQUIRE(adapter);
    const ControlUiExactTarget target{.instance_id = "instance-a",
                                      .instance_generation = "publication-a",
                                      .view_generation = "view-a",
                                      .node_id = "gain"};
    const ControlUiAuthorityOwner owner{.authority_id = "opaque-authority-a"};

    CHECK(adapter->dispatch_input(target, owner, ControlUiFocusInput{.focused = true}) ==
          ControlUiApplyStatus::Applied);
    CHECK(node->has_focus());
    auto other_owner = owner;
    other_owner.authority_id = "opaque-authority-b";
    CHECK(adapter->dispatch_input(target, other_owner, ControlUiTextInput{.text = "denied"}) ==
          ControlUiApplyStatus::InvalidEvent);
    CHECK(adapter->dispatch_input(target, owner, ControlUiTextInput{.text = "42"}) ==
          ControlUiApplyStatus::Applied);
    CHECK(node->text == "42");
    CHECK(adapter->dispatch_input(
              target, owner,
              ControlUiKeyboardInput{.phase = ControlUiKeyboardInput::Phase::Down,
                                     .key = "Enter"}) ==
          ControlUiApplyStatus::Applied);
    CHECK(node->key_calls == 1);
    pulp::view::transfer_input_focus(root, nullptr);
    CHECK(adapter->dispatch_input(target, owner, ControlUiTextInput{.text = "drift"}) ==
          ControlUiApplyStatus::InvalidEvent);
    CHECK(adapter->dispatch_input(target, owner, ControlUiFocusInput{.focused = true}) ==
          ControlUiApplyStatus::Applied);
    CHECK(adapter->dispatch_input(
              target, owner,
              ControlUiPointerInput{.phase = ControlUiPointerInput::Phase::Down,
                                    .x = 20,
                                    .y = 20,
                                    .button = 1}) ==
          ControlUiApplyStatus::Applied);
    CHECK(node->down_calls == 1);

    adapter->release_controller();
    CHECK(node->cancel_calls == 1);
    CHECK_FALSE(node->has_focus());
    CHECK(window.repaint_calls >= 4);

    node->set_bounds({0, 0, 10'000, 10'000});
    const auto oversized_capture = adapter->capture_node_png(target);
    CHECK(oversized_capture.error_code == "capture_too_large");
    CHECK(oversized_capture.png.empty());

    auto stale = target;
    stale.view_generation = "view-b";
    CHECK(adapter->dispatch_input(stale, owner, ControlUiFocusInput{.focused = true}) ==
          ControlUiApplyStatus::StaleGeneration);
}

TEST_CASE("Standalone UI adapter reports a self-unmounting pointer event as applied",
          "[inspect][control][ui][standalone][pointer][receipt]") {
    pulp::view::View root;
    root.set_bounds({0, 0, 100, 100});
    TestWindowHost window;
    root.set_window_host(&window);
    std::unique_ptr<pulp::view::View> detached;
    auto child = std::make_unique<SelfUnmountNode>(root, detached);
    auto* node = child.get();
    node->set_id("self-unmount");
    node->set_bounds({0, 0, 50, 50});
    root.add_child(std::move(child));
    auto adapter = ControlStandaloneUiAdapter::create(
        {.root = root,
         .window = window,
         .instance_id = "instance-a",
         .instance_generation = "publication-a",
         .view_generation = "view-a"});
    REQUIRE(adapter);
    const ControlUiExactTarget target{.instance_id = "instance-a",
                                      .instance_generation = "publication-a",
                                      .view_generation = "view-a",
                                      .node_id = "self-unmount"};
    const ControlUiAuthorityOwner owner{.authority_id = "opaque-authority-a"};
    CHECK(adapter->dispatch_input(
              target, owner,
              ControlUiPointerInput{.phase = ControlUiPointerInput::Phase::Down,
                                    .x = 10,
                                    .y = 10,
                                    .button = 1}) ==
          ControlUiApplyStatus::Applied);
    REQUIRE(detached);
    CHECK(static_cast<SelfUnmountNode*>(detached.get())->modern_down_calls == 1);
}
