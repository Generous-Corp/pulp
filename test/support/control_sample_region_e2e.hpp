#pragma once

// The proof host uses ordinary broker enrollment and the installed host's
// authenticated executor. Only its test-owned composition adds a checkpoint
// barrier; the target and the broker still decide whether cancellation applies.
#ifdef PULP_SAMPLE_REGION_BROKER_PROOF_HOST

#include "../../examples/sample-region-allpass/control_sample_region_allpass.hpp"
#include <pulp/inspect/control_host_preflight.hpp>
#include <pulp/inspect/control_installed_host.hpp>
#include <pulp/inspect/control_main_thread_executor.hpp>
#include <pulp/inspect/control_sample_region_edit_executor.hpp>
#include <pulp/inspect/control_sample_region_read_executor.hpp>
#include <pulp/inspect/control_state_read_executor.hpp>
#include <pulp/inspect/control_state_write_executor.hpp>
#include <pulp/inspect/motion_inspector.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/motion.hpp>
#include <pulp/view/view.hpp>

#include <deque>
#include <functional>
#include <sstream>

namespace pulp::test::sample_region_proof {
using namespace std::chrono_literals;
using namespace pulp::inspect;
namespace product = pulp::examples::control_allpass;

// The generated shipping source supplies the manifest/profile identity. These
// markers belong to this composition, which implements precisely these paths.
[[gnu::used]] inline const volatile char implementation_markers[] =
    "PULP_INSPECT_SHIPPING_MANIFEST_V1\0"
    "PULP_INSPECT_CAPABILITY_SESSION_DESCRIBE_V1\0"
    "PULP_INSPECT_CAPABILITY_SESSION_CONTROL_V1\0"
    "PULP_INSPECT_CAPABILITY_STATE_READ_V1\0"
    "PULP_INSPECT_CAPABILITY_STATE_WRITE_V1\0"
    "PULP_INSPECT_CAPABILITY_GRAPH_SAMPLE_REGION_READ_V1\0"
    "PULP_INSPECT_CAPABILITY_GRAPH_SAMPLE_REGION_EDIT_V1";

class MainQueue {
  public:
    bool post(std::function<void()> task) {
        std::lock_guard lock(mutex_);
        if (!task || tasks_.size() >= 64)
            return false;
        tasks_.push_back(std::move(task));
        return true;
    }
    void pump() {
        std::function<void()> task;
        {
            std::lock_guard lock(mutex_);
            if (tasks_.empty())
                return;
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        task();
    }

  private:
    std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;
};

inline std::string_view checkpoint_id(ControlExecutionCheckpoint value) {
    switch (value) {
    case ControlExecutionCheckpoint::Continue:
        return "continue";
    case ControlExecutionCheckpoint::Cancelled:
        return "cancelled";
    case ControlExecutionCheckpoint::AuthorityRevoked:
        return "authority_revoked";
    case ControlExecutionCheckpoint::DeadlineExceeded:
        return "deadline_exceeded";
    }
    return "unknown";
}

struct Barrier {
    std::string name;
    bool allow = false;
    double local_normalized = 0;
};

inline std::optional<Barrier> take_barrier(const std::filesystem::path& directory) {
    const auto path = directory / "arm";
    if (!std::filesystem::exists(path))
        return std::nullopt;
    std::ifstream stream(path);
    Barrier result;
    std::string mode, extra;
    stream >> result.name >> mode >> result.local_normalized;
    if (!stream || !std::isfinite(result.local_normalized) || result.local_normalized < 0 ||
        result.local_normalized > 1 || result.name.empty() || result.name.size() > 64 ||
        result.name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_") !=
            std::string::npos ||
        (!mode.empty() && mode != "allow" && mode != "stop") || (stream >> extra))
        throw std::runtime_error("invalid sample-region proof barrier");
    result.allow = mode == "allow";
    std::filesystem::remove(path);
    return result;
}

inline choc::value::Value snapshot(const ControlAdmissionPlan& plan,
                                   const examples::SampleRegionAllpassProcessor& processor,
                                   const state::StateStore& store,
                                   const ControlSampleRegionGeneration& generation,
                                   const product::RenderWorker& renderer) {
    auto result = choc::value::createObject("SampleRegionBarrier");
    result.setMember("schema", "pulp.sample-region.control-barrier.v1");
    result.setMember("receipt_id", plan.receipt_id.value);
    result.setMember("registration_id", plan.registration_id.value);
    result.setMember("instance_id", plan.instance_id);
    result.setMember("publication_id", plan.publication_id);
    result.setMember("graph_generation", static_cast<std::int64_t>(generation.value));
    result.setMember("state_generation", static_cast<std::int64_t>(store.state_generation()));
    result.setMember("catalog_generation",
                     static_cast<std::int64_t>(store.parameter_display_revision() + 1));
    result.setMember("coefficient", store.get_value(examples::kAllpassCoefficient));
    result.setMember("normalized", store.get_normalized(examples::kAllpassCoefficient));
    result.setMember("graph_sha256",
                     runtime::sha256_hex(host::GraphSerializer::to_json(processor.graph())));
    result.setMember("continuous_blocks", static_cast<std::int64_t>(renderer.blocks()));
    result.setMember("capture_sequence", static_cast<std::int64_t>(renderer.capture_sequence()));
    return result;
}

inline ControlExecutionOutcome
guarded_edit(const std::filesystem::path& directory, const std::filesystem::path& evidence,
             const ControlOperationExecutor& edit, const ControlAdmissionPlan& plan,
             const ControlRequestEnvelope& request, const ControlExecutionContext& context,
             examples::SampleRegionAllpassProcessor& processor, state::StateStore& store,
             ControlSampleRegionGeneration& generation, product::RenderWorker& renderer,
             format::HeadlessHost& app, std::thread::id main_thread) {
    if (std::this_thread::get_id() != main_thread)
        throw std::runtime_error("prepared proof must execute on the host main thread");
    const auto barrier = take_barrier(directory);
    if (!barrier)
        return edit(plan, request, context);
    auto effective = context;
    unsigned checkpoints = 0;
    auto observed = ControlExecutionCheckpoint::Continue;
    bool prepared = false;
    auto parameter_write = choc::value::createObject("InterveningHostParameterWrite");
    const auto before = snapshot(plan, processor, store, generation, renderer);
    // This wrapper is installed inside the main-thread executor, so its second
    // call is the target's post-prepare/pre-commit checkpoint, not the RPC gate.
    effective.checkpoint = [&] {
        ++checkpoints;
        if (checkpoints != 2)
            return context.checkpoint();
        prepared = true;
        auto ready = snapshot(plan, processor, store, generation, renderer);
        ready.setMember("phase", "prepared_before_commit");
        if (!product::write_atomic(directory / (barrier->name + ".prepared.json"),
                                   choc::json::toString(ready)))
            throw std::runtime_error("could not publish prepared checkpoint");
        if (context.report_progress)
            (void)context.report_progress(1, 2, R"({"phase":"prepared_before_commit"})");
        const auto progress_deadline = std::chrono::steady_clock::now() + 2s;
        const auto prepared_blocks =
            static_cast<std::uint64_t>(ready["continuous_blocks"].getInt64());
        while (renderer.blocks() == prepared_blocks && !renderer.failed() &&
               std::chrono::steady_clock::now() < progress_deadline)
            std::this_thread::sleep_for(1ms);
        if (renderer.blocks() <= prepared_blocks)
            throw std::runtime_error("audio did not advance while the candidate was prepared");
        // Model a local editor write on the actual host main thread while the
        // broker's topology candidate is prepared. A second broker operation
        // is separately admitted by the test, but its host route is serialized.
        store.set_normalized(examples::kAllpassCoefficient,
                             static_cast<float>(barrier->local_normalized));
        parameter_write = snapshot(plan, processor, store, generation, renderer);
        parameter_write.setMember("phase", "parameter_write_between_prepare_and_commit");
        parameter_write.setMember("source", "host_main_thread");
        parameter_write.setMember("on_main_thread", std::this_thread::get_id() == main_thread);
        if (!product::write_atomic(directory / (barrier->name + ".parameter.json"),
                                   choc::json::toString(parameter_write)))
            throw std::runtime_error("could not publish intervening parameter evidence");
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        const auto resume = directory / (barrier->name + ".resume");
        while (std::chrono::steady_clock::now() < deadline) {
            if (std::filesystem::exists(resume)) {
                observed = context.checkpoint();
                if (barrier->allow || observed != ControlExecutionCheckpoint::Continue)
                    return observed;
            }
            if (std::filesystem::exists(directory / "stop"))
                throw std::runtime_error("proof host stopped at prepared checkpoint");
            std::this_thread::sleep_for(1ms);
        }
        // A missing broker cancellation is a fixture failure, never a forged
        // Cancelled result that could make the journey appear successful.
        throw std::runtime_error("broker checkpoint did not resolve before the barrier deadline");
    };
    const auto outcome = edit(plan, request, effective);
    // Capture the intervening value before releasing the serialized host route
    // to the separately queued broker gesture.
    const auto capture = product::render_snapshot(app, false, evidence);
    if (!capture || !renderer.capture(*capture))
        throw std::runtime_error("could not capture the post-topology parameter value");
    auto completed = snapshot(plan, processor, store, generation, renderer);
    completed.setMember("phase", "finished");
    completed.setMember("before", before);
    completed.setMember("intervening_parameter_write", parameter_write);
    completed.setMember("prepared_checkpoint_reached", prepared);
    completed.setMember("checkpoint_count", static_cast<std::int64_t>(checkpoints));
    completed.setMember("observed_checkpoint", std::string(checkpoint_id(observed)));
    completed.setMember("terminal_state",
                        std::string(control_receipt_state_id(outcome.terminal_state)));
    completed.setMember("explanation", outcome.result.explanation);
    if (outcome.result.result_code)
        completed.setMember("result_code",
                            std::string(control_result_code_id(*outcome.result.result_code)));
    completed.setMember(
        "detail",
        choc::json::parse(outcome.result.detail_json.empty() ? "{}" : outcome.result.detail_json));
    if (!product::write_atomic(directory / (barrier->name + ".finished.json"),
                               choc::json::toString(completed)))
        throw std::runtime_error("could not publish completed checkpoint");
    return outcome;
}

inline int host_main(int argc, char** argv) {
    if (argc != 3 || implementation_markers[0] != 'P')
        return 64;
    const std::filesystem::path barriers(argv[1]), evidence(argv[2]);
    if (!barriers.is_absolute() || !evidence.is_absolute())
        return 64;
    std::filesystem::create_directories(barriers);
    std::filesystem::create_directories(evidence);
    format::HeadlessHost app(&examples::create_sample_region_allpass);
    app.prepare(product::sample_rate, product::block_size, 1, 1);
    auto* processor = dynamic_cast<examples::SampleRegionAllpassProcessor*>(app.processor());
    if (!app.valid() || !processor || !processor->ready() || !product::prepared_allpass(app))
        return 65;
    ControlSampleRegionGeneration generation;
    auto target = product::editable_target(*processor, app.state(), generation);
    if (!target)
        return 65;
    target->set_preparation_context(product::sample_rate, product::block_size);
    ControlHostPreflightDiagnostics diagnostics;
    auto bootstrap = receive_control_host_preflight(inherited_control_host_bootstrap_handle(), 10s,
                                                    std::chrono::system_clock::now(), &diagnostics);
    if (!bootstrap || bootstrap->enrollment_id.empty())
        return 66;
    auto preflight = choc::value::createObject("SampleRegionProofPreflight");
    preflight.setMember("status", diagnostics.status == ControlHostPreflightStatus::Accepted
                                      ? "accepted"
                                      : "unexpected");
    preflight.setMember("transport", "inherited_host_preflight");
    preflight.setMember("bootstrap_schema", bootstrap->schema);
    preflight.setMember("bootstrap_version", static_cast<std::int64_t>(bootstrap->version));
    preflight.setMember("credential_kind", "enrollment");
    preflight.setMember("expires_at_unix_ms", bootstrap->expires_at_unix_ms);
    const auto& broker = bootstrap->expected_broker.evidence;
    preflight.setMember("broker_process_id", broker.process_id);
    preflight.setMember("broker_process_start_id", broker.process_start_id);
    preflight.setMember("broker_executable_identity", broker.executable_identity);
    const auto producer = product::render_identity();
    if (!producer)
        return 68;

    view::FrameClock clock;
    view::motion::Coordinator::instance().reset();
    view::motion::Coordinator::instance().bind(clock);
    view::View root;
    root.set_id("sample-region-proof");
    MotionInspector motion(root);
    auto queue = std::make_shared<MainQueue>();
    const auto thread = std::this_thread::get_id();
    auto rpc = std::make_shared<InspectorMainThreadRpc>(
        InspectorMainThreadRpc::Config{10s, 64},
        [queue](std::function<void()> task) { return queue->post(std::move(task)); },
        [thread] { return thread == std::this_thread::get_id(); });
    auto region_read = make_control_sample_region_read_executor(
        [target](const ControlAdmissionPlan&) { return target; });
    auto region_edit = make_control_sample_region_edit_executor(
        [target](const ControlAdmissionPlan&) { return target; });
    auto state_read = make_control_state_read_executor(
        [&app](const ControlAdmissionPlan& plan) -> std::optional<ControlStateReadSource> {
            const auto generation = app.state().state_generation();
            if (!app.state().state_snapshot_is_current(generation))
                return std::nullopt;
            return ControlStateReadSource{.registration_id = plan.registration_id,
                                          .host_tier = ControlHostTier::Standalone,
                                          .store = &app.state(),
                                          .state_generation = generation,
                                          .catalog_generation =
                                              app.state().parameter_display_revision() + 1,
                                          .is_sensitive = [](state::ParamID) { return false; }};
        });
    auto state_write = make_control_state_write_executor(
        [&app](const ControlAdmissionPlan& plan) -> std::optional<ControlStateWriteTarget> {
            const auto generation = app.state().state_generation();
            if (!app.state().state_snapshot_is_current(generation))
                return std::nullopt;
            return ControlStateWriteTarget{.registration_id = plan.registration_id,
                                           .host_tier = ControlHostTier::Standalone,
                                           .store = &app.state(),
                                           .state_generation = generation};
        });
    product::RenderWorker renderer(app, false);
    ControlMainThreadExecutor main(rpc, [&](const ControlAdmissionPlan& plan,
                                            const ControlRequestEnvelope& request,
                                            const ControlExecutionContext& context) {
        if (request.operation_id == "dev.pulp.graph/sample-region.edit@1")
            return guarded_edit(barriers, evidence, region_edit, plan, request, context, *processor,
                                app.state(), generation, renderer, app, thread);
        if (request.operation_id == "dev.pulp.graph/sample-region.read@1")
            return region_read(plan, request, context);
        if (request.operation_id == "dev.pulp.state/read@1")
            return state_read(plan, request, context);
        if (request.operation_id == "dev.pulp.state/parameter-gesture@1")
            return state_write(plan, request, context);
        return ControlExecutionOutcome{
            .terminal_state = ControlReceiptState::Failed,
            .result = {.result_code = ControlResultCode::NotImplemented}};
    });
    auto installed = ControlInstalledHost::start({
        .bootstrap = std::move(*bootstrap),
        .main_thread_rpc = rpc,
        .trace_inspector = std::make_shared<TraceInspector>(),
        .motion_inspector = &motion,
        .host_executor = main.executor(),
        .heartbeat_interval = 100ms,
        .heartbeat_ttl = 10s,
        .handshake_timeout = 3s,
    });
    if (!installed || !installed->ready())
        return 67;
    auto binding = choc::value::createObject("SampleRegionProofBinding");
    binding.setMember("schema", "pulp.sample-region.control-proof-host.v1");
    binding.setMember("phase", "ready");
    binding.setMember("instance_id", installed->binding().instance_id);
    binding.setMember("registration_id", installed->binding().registration_id);
    binding.setMember("publication_id", installed->binding().publication_id);
    binding.setMember("instance_generation", installed->binding().instance_generation);
    binding.setMember("broker_id", installed->binding().broker_id);
    binding.setMember("session_id", installed->binding().session_id);
    binding.setMember("manifest_digest", installed->binding().manifest_digest);
    binding.setMember("producer_artifact_digest", installed->binding().producer_artifact_digest);
    binding.setMember("preflight", preflight);
    binding.setMember("producer", *producer);
    binding.setMember("capture_sequence", static_cast<std::int64_t>(renderer.capture_sequence()));
    if (!product::write_atomic(barriers / "binding.json", choc::json::toString(binding)))
        return 68;
    std::uint64_t rendered_graph = 0, rendered_state = 0;
    bool capture_failed = false;
    const auto deadline = std::chrono::steady_clock::now() + 120s;
    while (installed->ready() && !renderer.failed() &&
           std::chrono::steady_clock::now() < deadline &&
           !std::filesystem::exists(barriers / "stop")) {
        queue->pump();
        const bool requested = product::consume_capture_request(evidence);
        if (requested || rendered_graph != generation.value ||
            rendered_state != app.state().state_generation()) {
            const auto capture = product::render_snapshot(app, false, evidence);
            if (!capture || !renderer.capture(*capture)) {
                capture_failed = true;
                break;
            }
            rendered_graph = generation.value;
            rendered_state = app.state().state_generation();
        }
        std::this_thread::sleep_for(1ms);
    }
    const bool stop_requested = std::filesystem::exists(barriers / "stop");
    const bool host_ready = installed->ready();
    renderer.stop();
    installed->stop();
    rpc->cancel_and_wait();
    const bool success = stop_requested && !renderer.failed() && !capture_failed;
    auto exited = binding;
    exited.setMember("phase", "exited");
    exited.setMember("success", success);
    exited.setMember("stop_requested", stop_requested);
    exited.setMember("host_ready_before_stop", host_ready);
    exited.setMember("host_ready_after_stop", installed->ready());
    exited.setMember("renderer_failed", renderer.failed());
    exited.setMember("capture_failed", capture_failed);
    exited.setMember("continuous_blocks", static_cast<std::int64_t>(renderer.blocks()));
    exited.setMember("capture_sequence", static_cast<std::int64_t>(renderer.capture_sequence()));
    exited.setMember("graph_generation", static_cast<std::int64_t>(generation.value));
    exited.setMember("state_generation", static_cast<std::int64_t>(app.state().state_generation()));
    exited.setMember("exit_code", success ? 0 : 69);
    product::generation = nullptr;
    if (!product::write_atomic(barriers / "exited.json", choc::json::toString(exited)))
        return 68;
    return success ? 0 : 69;
}
} // namespace pulp::test::sample_region_proof

inline int sample_region_broker_proof_host_main(int argc, char** argv) {
    try {
        return pulp::test::sample_region_proof::host_main(argc, argv);
    } catch (...) {
        return 70;
    }
}
#else

#include <catch2/catch_test_macros.hpp>

#include "control_broker_daemon.hpp"
#include <pulp/events/interprocess_connection.hpp>
#include <pulp/inspect/control_client_connection.hpp>
#include <pulp/inspect/control_operations.hpp>
#include <pulp/inspect/control_peer.hpp>
#include <pulp/platform/child_process.hpp>
#include <pulp/runtime/crypto.hpp>

#include <choc/text/choc_JSON.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <initializer_list>
#include <iostream>
#include <mutex>
#include <pulp/audio/audio_file.hpp>
#include <pulp/inspect/control_manifest.hpp>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace pulp::inspect;
using namespace std::chrono_literals;

namespace {

struct Root {
    std::filesystem::path path;
    std::filesystem::path runtime;
    std::filesystem::path state;
    std::string previous_artifact_root;
    bool had_artifact_root = false;
    Root() {
        const auto bytes = pulp::runtime::secure_random_bytes(8);
        REQUIRE(bytes);
        path = std::filesystem::path{"/private/tmp"} /
               ("sample-region-c4-" + pulp::runtime::hex_encode(*bytes));
        REQUIRE(std::filesystem::create_directory(path));
        std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
        runtime = path / "runtime";
        state = path / "state";
        if (const char* prior = std::getenv("PULP_SAMPLE_REGION_ARTIFACT_DIR")) {
            had_artifact_root = true;
            previous_artifact_root = prior;
        }
        REQUIRE(::setenv("PULP_SAMPLE_REGION_ARTIFACT_DIR", (path / "audio").c_str(), 1) == 0);
    }
    ~Root() {
        if (had_artifact_root)
            (void)::setenv("PULP_SAMPLE_REGION_ARTIFACT_DIR", previous_artifact_root.c_str(), 1);
        else
            (void)::unsetenv("PULP_SAMPLE_REGION_ARTIFACT_DIR");
        if (std::getenv("PULP_CONTROL_E2E_KEEP_ARTIFACTS")) {
            std::cout << "control proof artifacts: " << path.string() << '\n';
            return;
        }
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

#ifdef __APPLE__
std::filesystem::path self() {
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    std::vector<char> bytes(size);
    if (size == 0 || _NSGetExecutablePath(bytes.data(), &size) != 0)
        return {};
    return std::filesystem::weakly_canonical(bytes.data());
}

pulp::platform::ProcessResult run(const std::filesystem::path& binary,
                                  const std::filesystem::path& runtime,
                                  std::vector<std::string> args) {
    args.insert(args.begin(), binary.string());
    pulp::platform::ProcessOptions options;
    options.capture_stdout = true;
    options.capture_stderr = true;
    options.timeout_ms = 20'000;
    std::vector<std::string> env{"TMPDIR=" + runtime.string()};
    env.insert(env.end(), args.begin(), args.end());
    return pulp::platform::ChildProcess::run("/usr/bin/env", env, options);
}

std::string cli_failure_diagnostics(const pulp::platform::ProcessResult& result,
                                    const std::filesystem::path& operations) {
    if (result.exit_code == 0)
        return {};
    std::string diagnostic = "CLI failure";
    try {
        const auto response = choc::json::parse(result.stdout_output);
        for (const auto* field : {"state", "error", "explanation"})
            if (response[field].isString())
                diagnostic +=
                    " " + std::string(field) + "=" + std::string(response[field].getString());
        if (response["receipt_id"].isString()) {
            const std::string id(response["receipt_id"].getString());
            if (!id.empty() && id.size() <= 256 &&
                id.find_first_not_of(
                    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") ==
                    std::string::npos) {
                std::ifstream stream(operations / (id + ".json"));
                const std::string bytes(std::istreambuf_iterator<char>{stream}, {});
                const auto receipt = choc::json::parse(bytes);
                if (receipt["result_code"].isString())
                    diagnostic += " result_code=" + std::string(receipt["result_code"].getString());
            }
        }
    } catch (...) {
        diagnostic += " (structured diagnostics unavailable)";
    }
    return diagnostic;
}

std::string mcp_call(int id, std::string_view name, std::string_view instance,
                     std::string_view input, std::string_view profile = "develop") {
    auto request = choc::value::createObject("");
    request.setMember("jsonrpc", "2.0");
    request.setMember("id", id);
    request.setMember("method", "tools/call");
    auto params = choc::value::createObject("");
    params.setMember("name", std::string(name));
    auto arguments = choc::value::createObject("");
    arguments.setMember("instance_id", std::string(instance));
    arguments.setMember("profile", std::string(profile));
    arguments.setMember("input", choc::json::parse(input));
    params.setMember("arguments", arguments);
    request.setMember("params", params);
    return choc::json::toString(request, false) + "\n";
}

std::vector<choc::value::Value> run_mcp(const std::filesystem::path& binary,
                                        const std::filesystem::path& runtime,
                                        std::string transcript) {
    pulp::platform::ProcessOptions options;
    options.capture_stdout = true;
    options.capture_stderr = true;
    options.timeout_ms = 30'000;
    pulp::platform::ChildProcess process;
    const std::vector<std::uint8_t> bytes(transcript.begin(), transcript.end());
    REQUIRE(process.start_with_standard_input(
        "/usr/bin/env", {"TMPDIR=" + runtime.string(), binary.string()}, bytes, options));
    const auto result = process.wait();
    REQUIRE(result.exit_code == 0);
    std::vector<choc::value::Value> responses;
    std::size_t start = 0;
    while (start < result.stdout_output.size()) {
        const auto end = result.stdout_output.find('\n', start);
        const auto line = result.stdout_output.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty()) {
            auto response = choc::json::parse(line);
            if (response.hasObjectMember("id"))
                responses.push_back(std::move(response));
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return responses;
}

std::set<std::string> operation_set(choc::value::ValueView detail) {
    std::set<std::string> result;
    const auto capabilities = detail["capabilities"];
    for (std::uint32_t i = 0; i < capabilities.size(); ++i)
        result.emplace(capabilities[i].getString());
    return result;
}

void require_manifest(choc::value::ValueView detail, std::string_view expected_plugin_id,
                      std::initializer_list<std::string_view> expected_operations) {
    REQUIRE(detail["plugin_id"].getString() == expected_plugin_id);
    std::set<std::string> expected;
    for (const auto operation : expected_operations)
        expected.emplace(operation);
    REQUIRE(operation_set(detail) == expected);
}

choc::value::Value wait_for_instance(ControlClientConnection& management,
                                     std::string_view plugin_id) {
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    std::string last_inventory;
    do {
        const auto inventory = management.manage("instances", "{}", 3s);
        REQUIRE(inventory.status_id == "completed");
        last_inventory = inventory.data_json;
        const auto document = choc::json::parse(last_inventory);
        const auto instances = document["instances"];
        for (std::uint32_t i = 0; i < instances.size(); ++i)
            if (instances[i]["plugin_id"].getString() == plugin_id)
                return choc::value::Value(instances[i]);
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    FAIL("launched child did not enroll the expected live product before the deadline");
    return {};
}

void wait_for_instance_removal(ControlClientConnection& management,
                               choc::value::ValueView identity) {
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    do {
        const auto inventory = management.manage("instances", "{}", 3s);
        REQUIRE(inventory.status_id == "completed");
        bool found = false;
        const auto document = choc::json::parse(inventory.data_json);
        for (const auto instance : document["instances"])
            found =
                found ||
                instance["instance_id"].getString() == identity["instance_id"].getString() ||
                instance["registration_id"].getString() == identity["registration_id"].getString();
        if (!found)
            return;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    FAIL("stopped proof host retained a live broker registration");
}

void require_instance_binding(choc::value::ValueView receipt, choc::value::ValueView binding) {
    for (const auto* field : {"instance_id", "registration_id", "publication_id"})
        REQUIRE(receipt[field].getString() == binding[field].getString());
}

void require_render_binding(choc::value::ValueView receipt, choc::value::ValueView binding) {
    REQUIRE(receipt["producer"]["process_id"].getInt64() ==
            binding["producer"]["process_id"].getInt64());
    for (const auto* field :
         {"process_start_id", "executable_sha256", "manifest_sha256", "build_id", "plugin_id"})
        REQUIRE(receipt["producer"][field].getString() == binding["producer"][field].getString());
}

void require_allpass_resources(choc::value::ValueView stats, std::int64_t appended_delays) {
    CHECK(stats["member_nodes"].getInt64() == 11 + appended_delays);
    CHECK(stats["internal_connections"].getInt64() == 13 + appended_delays);
    CHECK(stats["input_boundaries"].getInt64() == 1);
    CHECK(stats["output_boundaries"].getInt64() == 1);
    CHECK(stats["delay_nodes"].getInt64() == 2 + appended_delays);
    CHECK(stats["promoted_parameters"].getInt64() == 1);
    CHECK(stats["state_bytes"].getInt64() == 4 * (2 + appended_delays));
    CHECK(stats["state_alignment"].getInt64() == 4);
    CHECK(stats["logical_boundary_bytes"].getInt64() == 512);
    CHECK(stats["work_per_frame"].getInt64() == 28 + 3 * appended_delays);
    CHECK(stats["work_per_block"].getInt64() == 64 * (28 + 3 * appended_delays));
}

void require_delay_mapping(choc::value::ValueView mutation, choc::value::ValueView before,
                           choc::value::ValueView after) {
    REQUIRE(before["graph_generation"].getInt64() == mutation["old_graph_generation"].getInt64());
    REQUIRE(after["graph_generation"].getInt64() == mutation["new_graph_generation"].getInt64());
    REQUIRE(before["regions"].size() == 1);
    REQUIRE(after["regions"].size() == 1);
    REQUIRE(before["regions"][0]["region_id"].getInt64() ==
            after["regions"][0]["region_id"].getInt64());
    REQUIRE(mutation["node_mapping"].size() == 1);
    REQUIRE(mutation["node_mapping"][0]["temporary_node_id"].getString() == "t1");
    const auto added = mutation["node_mapping"][0]["node_id"].getInt64();
    REQUIRE(added > 0);
    const auto original = before["regions"][0]["definition"];
    const auto current = after["regions"][0]["definition"];
    REQUIRE(current["members"].size() >= 12);
    const auto appended_delays = static_cast<std::int64_t>(current["members"].size()) - 11;
    require_allpass_resources(mutation["resources"], appended_delays);
    require_allpass_resources(after["regions"][0]["resources"], appended_delays);
    std::set<std::int64_t> original_ids, current_ids;
    for (const auto member : original["members"])
        original_ids.insert(member["node_id"].getInt64());
    REQUIRE_FALSE(original_ids.contains(added));
    for (const auto member : current["members"]) {
        current_ids.insert(member["node_id"].getInt64());
        if (member["node_id"].getInt64() == added) {
            CHECK(member["type_id"].getString() == "pulp.core.unit-delay");
            CHECK(member["type_version"].getInt64() == 1);
            CHECK(member["config"]["kind"].getString() == "none");
        }
    }
    original_ids.insert(added);
    REQUIRE(current_ids == original_ids);
    REQUIRE(current["members"].size() == original["members"].size() + 1);
    const auto output = original["output_boundaries"][0].getInt64();
    REQUIRE(current["output_boundaries"][0].getInt64() == output);
    unsigned replaced = 0;
    for (const auto edge : original["connections"]) {
        if (edge["crossing"].getBool() || edge["destination_node_id"].getInt64() != output)
            continue;
        ++replaced;
        const auto source = edge["source_node_id"].getInt64();
        const auto source_port = edge["source_port"].getInt64();
        const auto output_port = edge["destination_port"].getInt64();
        unsigned old_direct = 0, new_input = 0, new_output = 0, incident = 0;
        for (const auto actual : current["connections"]) {
            const auto src = actual["source_node_id"].getInt64();
            const auto dst = actual["destination_node_id"].getInt64();
            const auto src_port = actual["source_port"].getInt64();
            const auto dst_port = actual["destination_port"].getInt64();
            if (src == added || dst == added)
                ++incident;
            if (actual["crossing"].getBool())
                continue;
            old_direct += src == source && src_port == source_port && dst == output &&
                          dst_port == output_port;
            new_input += src == source && src_port == source_port && dst == added && dst_port == 0;
            new_output += src == added && src_port == 0 && dst == output && dst_port == output_port;
        }
        CHECK(old_direct == 0);
        CHECK(new_input == 1);
        CHECK(new_output == 1);
        CHECK(incident == 2);
    }
    REQUIRE(replaced == 1);
}

ControlErrorEnvelope request_without_session(const std::filesystem::path& endpoint,
                                             const ControlRequestEnvelope& request) {
    std::mutex mutex;
    std::condition_variable ready;
    std::optional<ControlEnvelope> response;
    bool received = false;
    pulp::events::InterprocessConnection connection;
    connection.set_max_message_bytes(kControlMaximumEnvelopeBytes);
    connection.set_write_timeout(3s);
    connection.set_frame_read_timeout(3s);
    connection.set_secure_receive_buffer(true);
    connection.set_on_message([&](const void* data, std::size_t size) {
        auto decoded =
            decode_control_envelope(std::string_view(static_cast<const char*>(data), size));
        {
            std::lock_guard lock(mutex);
            response = std::move(decoded);
            received = true;
        }
        ready.notify_all();
    });
    REQUIRE(connection.connect(endpoint.string(), pulp::events::IpcTransport::LocalSocket, 3s));
    const auto peer = observe_control_peer(connection, ControlPeerRole::Client);
    REQUIRE(peer);
    REQUIRE(peer->process_id == static_cast<std::int64_t>(::getpid()));
    const auto encoded = encode_control_envelope(ControlEnvelope{.payload = request});
    REQUIRE_FALSE(encoded.empty());
    REQUIRE(connection.send_message(encoded));
    bool answered = false;
    {
        std::unique_lock lock(mutex);
        answered = ready.wait_for(lock, 3s, [&] { return received; });
    }
    connection.disconnect();
    REQUIRE(answered);
    REQUIRE(response);
    const auto* error = std::get_if<ControlErrorEnvelope>(&response->payload);
    REQUIRE(error);
    REQUIRE(error->request_id == request.request_id);
    return *error;
}

std::string delay_edit(choc::value::ValueView detail) {
    const auto definition = detail["regions"][0]["definition"];
    const auto output = definition["output_boundaries"][0].getInt64();
    for (const auto edge : definition["connections"]) {
        if (edge["crossing"].getBool() || edge["destination_node_id"].getInt64() != output)
            continue;
        auto request = choc::value::createObject("");
        request.setMember("region_id", 901);
        request.setMember("expected_graph_generation", detail["graph_generation"]);
        auto actions = choc::value::createEmptyArray();
        actions.addArrayElement(choc::json::parse(
            R"({"op":"add_supported_kernel","temporary_node_id":"t1","type_id":"pulp.core.unit-delay","type_version":1,"config":{"kind":"none"}})"));
        auto disconnect = choc::value::createObject("");
        disconnect.setMember("op", "disconnect");
        for (const auto* key :
             {"source_node_id", "source_port", "destination_node_id", "destination_port"})
            disconnect.setMember(key, edge[key]);
        actions.addArrayElement(disconnect);
        auto first = choc::value::createObject("");
        first.setMember("op", "connect");
        first.setMember("source_node_id", edge["source_node_id"]);
        first.setMember("source_port", edge["source_port"]);
        first.setMember("destination_port", 0);
        first.setMember("destination_temporary_node_id", "t1");
        actions.addArrayElement(first);
        auto last = choc::json::parse(
            R"({"op":"connect","source_temporary_node_id":"t1","source_port":0,"destination_port":0})");
        last.setMember("destination_node_id", output);
        actions.addArrayElement(last);
        request.setMember("actions", actions);
        return choc::json::toString(request, false);
    }
    FAIL("full definition lacks the internal output connection");
    return {};
}

ControlRequestEnvelope wire_request(std::string_view client_id, choc::value::ValueView identity,
                                    std::string_view grant, std::string_view operation,
                                    std::string params, std::string key) {
    ControlRequestEnvelope request{
        .request_id = "request-" + key,
        .client_id = std::string(client_id),
        .registration_id = std::string(identity["registration_id"].getString()),
        .grant_id = std::string(grant),
        .instance_generation = std::string(identity["publication_id"].getString()),
        .operation_id = std::string(operation),
        .operation_version = 1,
        .idempotency_key = std::move(key),
        .deadline_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                (std::chrono::system_clock::now() + 20s).time_since_epoch())
                                .count(),
        .params_json = std::move(params),
    };
    request.request_hash = *control_request_hash(request);
    return request;
}

choc::value::Value wait_json(const std::filesystem::path& path) {
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    do {
        if (std::ifstream stream{path}; stream.good()) {
            const std::string bytes(std::istreambuf_iterator<char>{stream}, {});
            return choc::json::parse(bytes);
        }
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    FAIL("expected completed evidence file: " << path.string());
    return {};
}

void require_cli_refusal(const pulp::platform::ProcessResult& result,
                         const std::filesystem::path& operations, ControlResultCode code,
                         std::string_view explanation) {
    REQUIRE(result.exit_code != 0);
    const auto response = choc::json::parse(result.stdout_output);
    REQUIRE(response["state"].getString() == "failed");
    REQUIRE(response["explanation"].getString() == explanation);
    const std::string id(response["receipt_id"].getString());
    REQUIRE_FALSE(id.empty());
    REQUIRE(id.size() <= 256);
    const bool bounded_id =
        id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") ==
        std::string::npos;
    REQUIRE(bounded_id);
    const auto receipt = wait_json(operations / (id + ".json"));
    REQUIRE(receipt["receipt_id"].getString() == id);
    CHECK(receipt["state"].getString() == "failed");
    CHECK(receipt["result_code"].getString() == control_result_code_id(code));
    CHECK(receipt["explanation"].getString() == explanation);
}

choc::value::Value wait_terminal_receipt(const std::filesystem::path& directory,
                                         const std::string& receipt_id) {
    REQUIRE_FALSE(receipt_id.empty());
    REQUIRE(receipt_id.size() <= 256);
    const bool bounded_id =
        receipt_id.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") ==
        std::string::npos;
    REQUIRE(bounded_id);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        if (std::ifstream stream{directory / (receipt_id + ".json")}; stream.good()) {
            const std::string bytes(std::istreambuf_iterator<char>{stream}, {});
            auto receipt = choc::json::parse(bytes);
            REQUIRE(receipt["schema"].getString() == kControlOperationReceiptSchemaId);
            REQUIRE(receipt["schema_version"].getInt64() == kControlOperationReceiptSchemaVersion);
            REQUIRE(receipt["receipt_id"].getString() == receipt_id);
            const auto state = receipt["state"].getString();
            if (state != "admitted" && state != "running")
                return receipt;
        }
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    FAIL("broker did not durably settle the authenticated operation receipt");
    return {};
}

void wait_running_receipt(const std::filesystem::path& directory, std::string_view request_id) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (entry.path().extension() != ".json")
                continue;
            std::ifstream stream(entry.path());
            const std::string bytes(std::istreambuf_iterator<char>{stream}, {});
            const auto receipt = choc::json::parse(bytes);
            if (receipt["request_id"].getString() == request_id &&
                receipt["state"].getString() == "running")
                return;
        }
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    FAIL("concurrent gesture never reached the durable Running state");
}

choc::value::Value require_audio(const Root& root, bool frozen, std::int64_t graph,
                                 std::int64_t state, unsigned delay_samples,
                                 double expected_coefficient, bool fresh = false,
                                 std::filesystem::path producer_binary = {}) {
    const auto directory = root.path / "audio" / (frozen ? "frozen" : "editable");
    const auto stem = "graph-" + std::to_string(graph) + "-state-" + std::to_string(state);
    auto receipt = wait_json(directory / (stem + ".json"));
    if (fresh) {
        const auto prior_sequence = receipt["capture_sequence"].getInt64();
        std::ofstream(directory / "capture.request") << "capture\n";
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        do {
            receipt = wait_json(directory / (stem + ".json"));
            if (receipt["capture_sequence"].getInt64() > prior_sequence)
                break;
            std::this_thread::sleep_for(10ms);
        } while (std::chrono::steady_clock::now() < deadline);
        REQUIRE(receipt["capture_sequence"].getInt64() > prior_sequence);
    }
    REQUIRE(receipt["graph_generation"].getInt64() == graph);
    REQUIRE(receipt["state_generation"].getInt64() == state);
    const auto render_file = directory / std::string(receipt["render"].getString());
    const auto data = pulp::audio::read_audio_file(render_file.string());
    REQUIRE(data);
    REQUIRE(data->channels.size() == 1);
    REQUIRE(data->channels[0].size() == 4096);
    const auto render_hash = pulp::runtime::sha256_file_hex(render_file, 1024 * 1024);
    REQUIRE(render_hash);
    CHECK(receipt["render_sha256"].getString() == *render_hash);
    const auto graph_file = directory / std::string(receipt[frozen ? "bake" : "graph"].getString());
    const auto graph_hash = pulp::runtime::sha256_file_hex(graph_file, 1024 * 1024);
    REQUIRE(graph_hash);
    CHECK(receipt[frozen ? "bake_sha256" : "graph_sha256"].getString() == *graph_hash);
    if (producer_binary.empty())
        producer_binary =
            frozen ? PULP_SAMPLE_REGION_FROZEN_STANDALONE : PULP_SAMPLE_REGION_EDITABLE_STANDALONE;
    const auto producer_hash =
        pulp::runtime::sha256_file_hex(producer_binary, 1024ULL * 1024 * 1024);
    REQUIRE(producer_hash);
    CHECK(receipt["producer"]["executable_sha256"].getString() == *producer_hash);
    CHECK(receipt["producer"]["process_id"].getInt64() > 0);
    CHECK_FALSE(receipt["producer"]["process_start_id"].getString().empty());
    std::ifstream manifest_stream(producer_binary.string() + ".inspector-capabilities.json");
    const std::string manifest_bytes(std::istreambuf_iterator<char>{manifest_stream}, {});
    const auto manifest = parse_control_manifest(manifest_bytes);
    REQUIRE(manifest);
    CHECK(receipt["producer"]["plugin_id"].getString() == manifest->bundle_id);
    CHECK(receipt["producer"]["build_id"].getString() == manifest->build_id);
    CHECK(receipt["producer"]["manifest_sha256"].getString() ==
          pulp::runtime::sha256_hex(manifest_bytes));
    CHECK(receipt["settling_peak"].getFloat64() <= 1.0e-7);
    CHECK_FALSE(receipt["reset_requested"].getBool());
    const double coefficient = receipt["coefficient"].getFloat64();
    REQUIRE(std::abs(coefficient - expected_coefficient) < 1.0e-6);
    double previous_input = 0.0, previous_output = 0.0, maximum_error = 0.0;
    for (unsigned frame = 0; frame < data->channels[0].size(); ++frame) {
        double expected = 0.0;
        if (frame >= delay_samples) {
            const double input = frame == delay_samples ? 1.0 : 0.0;
            expected = coefficient * input + previous_input - coefficient * previous_output;
            previous_input = input;
            previous_output = expected;
        }
        REQUIRE(std::isfinite(data->channels[0][frame]));
        maximum_error = std::max(maximum_error, std::abs(data->channels[0][frame] - expected));
    }
    INFO("maximum scalar-oracle error " << maximum_error);
    REQUIRE(maximum_error < 2.0e-6);
    return receipt;
}

void require_preserved(const Root& root, const std::filesystem::path& cli,
                       const std::string& instance, bool frozen,
                       choc::value::ValueView expected_graph, choc::value::ValueView expected_state,
                       unsigned delay_samples, bool full_definition = true) {
    const auto graph =
        run(cli, root.runtime,
            {"control", "call", "--instance", instance, "dev.pulp.graph/sample-region.read@1",
             "--params", full_definition ? R"({"region_id":901,"include_definition":true})" : "{}",
             "--json"});
    const auto state = run(cli, root.runtime,
                           {"control", "call", "--instance", instance, "dev.pulp.state/read@1",
                            "--params", "{}", "--json"});
    REQUIRE(graph.exit_code == 0);
    REQUIRE(state.exit_code == 0);
    const bool graph_preserved =
        choc::json::toString(choc::json::parse(graph.stdout_output)["detail"]) ==
        choc::json::toString(expected_graph);
    const bool state_preserved =
        choc::json::toString(choc::json::parse(state.stdout_output)["detail"]) ==
        choc::json::toString(expected_state);
    REQUIRE(graph_preserved);
    REQUIRE(state_preserved);
    (void)require_audio(root, frozen, expected_graph["graph_generation"].getInt64(),
                        expected_state["state_generation"].getInt64(), delay_samples,
                        expected_state["parameters"][0]["value"].getFloat64(), true);
}

#endif
} // namespace

#endif // PULP_SAMPLE_REGION_BROKER_PROOF_HOST
