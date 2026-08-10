#include <pulp/inspect/control_host_preflight.hpp>
#include <pulp/inspect/control_installed_host.hpp>
#include <pulp/inspect/control_manifest.hpp>
#include <pulp/inspect/motion_inspector.hpp>
#include <pulp/view/motion.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/view.hpp>

#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#ifdef __APPLE__
#include <unistd.h>
#endif

using namespace std::chrono_literals;
using namespace pulp::inspect;

namespace {
const volatile char kStandalone[] = "PULP_STANDALONE_COMPONENT_V1";
const volatile char kShipping[] = "PULP_INSPECT_SHIPPING_MANIFEST_V1";
const volatile char kProfile[] = "PULP_CONTROL_PROFILE_DEVELOPER_LOCAL_V1";
const volatile char kManifest[] =
    "PULP_CONTROL_MANIFEST_SHA256_1c2edef28b738259deea6c7a42661eac321bd299b233c20cd83388abc6b96c47_"
    "V1";
const volatile char kTraceControl[] = "PULP_INSPECT_CAPABILITY_TRACE_CONTROL_V1";
const volatile char kTraceSession[] = "PULP_INSPECT_CAPABILITY_TRACE_SESSION_CONTROL_V1";
const volatile char kUiInput[] = "PULP_INSPECT_CAPABILITY_UI_INPUT_V1";

constexpr std::string_view kInstalledManifest = R"({
  "schema": "dev.pulp.control/artifact-manifest@1",
  "schema_version": 1,
  "profile": "developer-local",
  "target": "pulp-control-installed-host-e2e-fixture",
  "product_name": "Pulp Installed Host E2E Fixture",
  "bundle_id": "dev.pulp.test.installed-host-e2e-fixture",
  "build_id": "build:1123456789abcdef0123456789abcdef",
  "registry_digest": "b3bfbc17c377a58531c0689ce961d33d43d7504c61f8db979cd1a0df678409bc",
  "endpoint_included": true,
  "unsafe_runtime_eval_acknowledged": false,
  "permission_terms": ["implemented", "built", "host_available", "activated", "policy_eligible", "client_granted", "session_live"],
  "capabilities": ["dev.pulp.session/control@1", "dev.pulp.trace/control@1", "dev.pulp.trace/session-control@1", "dev.pulp.ui/input@1"]
}
)";

class MainThreadQueue {
  public:
    bool post(std::function<void()> task) {
        std::lock_guard lock(mutex_);
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

class RetainedUiAdapter final : public ControlHostUiTargetAdapter {
  public:
    InspectorCapture capture_node_png(const ControlUiExactTarget&) override { return {}; }

    ControlUiApplyStatus dispatch_input(const ControlUiExactTarget& target,
                                        const ControlUiAuthorityOwner& owner,
                                        const ControlUiInput& input) override {
        if (target.node_id != "root" || target.view_generation != "view-e2e" ||
            owner.authority_id.empty())
            return ControlUiApplyStatus::InvalidEvent;
        if (const auto* focus = std::get_if<ControlUiFocusInput>(&input); focus && focus->focused)
            focus_owner_ = owner;
        if (const auto* pointer = std::get_if<ControlUiPointerInput>(&input);
            pointer && pointer->phase == ControlUiPointerInput::Phase::Down)
            pointer_owner_ = owner;
        return ControlUiApplyStatus::Applied;
    }

    void release_controller(
        const std::optional<ControlUiAuthorityOwner>& owner) noexcept override {
        if (!owner || focus_owner_ == owner)
            focus_owner_.reset();
        if (!owner || pointer_owner_ == owner)
            pointer_owner_.reset();
    }

    std::size_t retained_count() const noexcept {
        return static_cast<std::size_t>(focus_owner_.has_value()) +
               static_cast<std::size_t>(pointer_owner_.has_value());
    }

  private:
    std::optional<ControlUiAuthorityOwner> focus_owner_;
    std::optional<ControlUiAuthorityOwner> pointer_owner_;
};

std::shared_ptr<InspectorMainThreadRpc> main_thread_rpc(
    const std::shared_ptr<MainThreadQueue>& queue, std::thread::id main_thread) {
    return std::make_shared<InspectorMainThreadRpc>(
        InspectorMainThreadRpc::Config{2s, 4},
        [queue](std::function<void()> task) { return queue->post(std::move(task)); },
        [main_thread] { return std::this_thread::get_id() == main_thread; });
}
} // namespace

int main(int argc, char** argv) {
    if ((argc != 4 && argc != 13) || kStandalone[0] != 'P' || kShipping[0] != 'P' ||
        kProfile[0] != 'P' || kManifest[0] != 'P' || kTraceControl[0] != 'P' ||
        kTraceSession[0] != 'P' || kUiInput[0] != 'P')
        return 64;
    ControlHostPreflightDiagnostics diagnostics;
    auto bootstrap = receive_control_host_preflight(inherited_control_host_bootstrap_handle(), 10s,
                                                    std::chrono::system_clock::now(), &diagnostics);
    if (!bootstrap || bootstrap->enrollment_id.empty())
        return 65;
    if (argc == 13) {
        std::ofstream(argv[4]) << "bootstrap-consumed\n";
        for (unsigned attempt = 0; attempt < 10'000 && !std::filesystem::exists(argv[5]); ++attempt)
            std::this_thread::sleep_for(1ms);
        if (!std::filesystem::exists(argv[5]))
            return 66;
    }

    pulp::view::FrameClock clock;
    pulp::view::motion::Coordinator::instance().reset();
    pulp::view::motion::Coordinator::instance().bind(clock);
    pulp::view::View root;
    root.set_id("root");
    auto motion = std::make_unique<MotionInspector>(root);
    auto telemetry = std::make_shared<ControlTelemetryTap>(ControlTelemetryTapConfig{});
    ControlManifestDiagnostics manifest_diagnostics;
    auto manifest = parse_control_manifest(kInstalledManifest, &manifest_diagnostics);
    if (!manifest)
        return 69;
    auto main_queue = std::make_shared<MainThreadQueue>();
    auto ui = std::make_shared<RetainedUiAdapter>();
    auto installed = ControlInstalledHost::start({
        .bootstrap = std::move(*bootstrap),
        .main_thread_rpc = main_thread_rpc(main_queue, std::this_thread::get_id()),
        .trace_inspector = std::make_shared<TraceInspector>(),
        .telemetry = std::move(telemetry),
        .motion_inspector = motion.get(),
        .ui = ControlInstalledHostUiConfig{.manifest = std::move(*manifest),
                                           .make_targets = [ui](const ControlHostOpenResult&) {
                                               return ControlInstalledHostUiTargets{
                                                   .target_adapter = ui,
                                                   .view_generation = "view-e2e"};
                                           }},
        .heartbeat_interval = 20ms,
        .heartbeat_ttl = 200ms,
        .handshake_timeout = 3s,
    });
    if (!installed || !installed->ready())
        return 67;
#ifdef __APPLE__
    // Enrollment ownership must retain the exact broker snapshot after the
    // host becomes ready; GPU and other optional runtime dependencies may be
    // loaded lazily from this directory later in the session.
    if (!std::filesystem::is_regular_file(
            std::filesystem::path(argv[0]).parent_path() / "libwgpu_native.dylib"))
        return 70;
#endif
    std::ofstream(argv[1]) << installed->binding().registration_id << '\n'
#ifdef __APPLE__
                           << ::getpid() << '\n'
#else
                           << 0 << '\n'
#endif
                           << std::filesystem::current_path().string() << '\n';
    unsigned authority_phase = 0;
    for (unsigned attempt = 0; attempt < 60'000 && !std::filesystem::exists(argv[2]); ++attempt) {
        main_queue->pump();
        if (argc == 13) {
            const auto active = motion->active_trace_count();
            const auto retained = ui->retained_count();
            if (authority_phase == 0 && active != 0 && retained == 2) {
                std::ofstream(argv[6]) << active << ' ' << retained << '\n';
                ++authority_phase;
            } else if (authority_phase == 1 && active == 0 && retained == 0) {
                std::ofstream(argv[7]) << "revoked\n";
                ++authority_phase;
            } else if (authority_phase == 2 && active != 0 && retained == 2) {
                std::ofstream(argv[8]) << active << ' ' << retained << '\n';
                ++authority_phase;
            } else if (authority_phase == 3 && active == 0 && retained == 0) {
                std::ofstream(argv[9]) << "expired\n";
                ++authority_phase;
            } else if (authority_phase == 4 && active != 0 && retained == 2) {
                std::ofstream(argv[10]) << active << ' ' << retained << '\n';
                ++authority_phase;
            } else if (authority_phase == 5 && active == 0 && retained == 0) {
                std::ofstream(argv[11]) << "disconnected\n";
                ++authority_phase;
            } else if (authority_phase == 6 && active != 0 && retained == 2) {
                std::ofstream(argv[12]) << active << ' ' << retained << '\n';
                ++authority_phase;
            }
        }
        std::this_thread::sleep_for(1ms);
    }
    const bool stopped = std::filesystem::exists(argv[2]);
    installed->stop();
    main_queue->pump();
    std::ofstream(argv[3]) << motion->active_trace_count() << ' ' << ui->retained_count() << '\n';
    motion.reset();
    pulp::view::motion::Coordinator::instance().reset();
    return stopped ? 0 : 68;
}
