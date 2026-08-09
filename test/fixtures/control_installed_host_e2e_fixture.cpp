#include <pulp/inspect/control_host_preflight.hpp>
#include <pulp/inspect/control_installed_host.hpp>
#include <pulp/inspect/motion_inspector.hpp>
#include <pulp/view/motion.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/view.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
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
    "PULP_CONTROL_MANIFEST_SHA256_a07150747fc592d713bb9c78d06b30547d9e0b858ad735d37c3c3bf587f6645a_"
    "V1";
const volatile char kTraceControl[] = "PULP_INSPECT_CAPABILITY_TRACE_CONTROL_V1";
const volatile char kTraceSession[] = "PULP_INSPECT_CAPABILITY_TRACE_SESSION_CONTROL_V1";

std::shared_ptr<InspectorMainThreadRpc> inline_rpc() {
    return std::make_shared<InspectorMainThreadRpc>(
        InspectorMainThreadRpc::Config{2s, 4},
        [](std::function<void()> task) {
            task();
            return true;
        },
        [] { return false; });
}
} // namespace

int main(int argc, char** argv) {
    if ((argc != 4 && argc != 10) || kStandalone[0] != 'P' || kShipping[0] != 'P' ||
        kProfile[0] != 'P' || kManifest[0] != 'P' || kTraceControl[0] != 'P' ||
        kTraceSession[0] != 'P')
        return 64;
    ControlHostPreflightDiagnostics diagnostics;
    auto bootstrap = receive_control_host_preflight(inherited_control_host_bootstrap_handle(), 10s,
                                                    std::chrono::system_clock::now(), &diagnostics);
    if (!bootstrap || bootstrap->enrollment_id.empty())
        return 65;
    if (argc == 10) {
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
    auto installed = ControlInstalledHost::start({
        .bootstrap = std::move(*bootstrap),
        .main_thread_rpc = inline_rpc(),
        .trace_inspector = std::make_shared<TraceInspector>(),
        .telemetry = std::move(telemetry),
        .motion_inspector = motion.get(),
        .heartbeat_interval = 20ms,
        .heartbeat_ttl = 200ms,
        .handshake_timeout = 3s,
    });
    if (!installed || !installed->ready())
        return 67;
    std::ofstream(argv[1]) << installed->binding().registration_id << '\n'
#ifdef __APPLE__
                           << ::getpid() << '\n'
#else
                           << 0 << '\n'
#endif
                           << std::filesystem::current_path().string() << '\n';
    unsigned authority_phase = 0;
    for (unsigned attempt = 0; attempt < 60'000 && !std::filesystem::exists(argv[2]); ++attempt) {
        if (argc == 10) {
            const auto active = motion->active_trace_count();
            if (authority_phase == 0 && active != 0) {
                std::ofstream(argv[6]) << active << '\n';
                ++authority_phase;
            } else if (authority_phase == 1 && active == 0) {
                std::ofstream(argv[7]) << "revoked\n";
                ++authority_phase;
            } else if (authority_phase == 2 && active != 0) {
                std::ofstream(argv[8]) << active << '\n';
                ++authority_phase;
            } else if (authority_phase == 3 && active == 0) {
                std::ofstream(argv[9]) << "expired\n";
                ++authority_phase;
            }
        }
        std::this_thread::sleep_for(1ms);
    }
    const bool stopped = std::filesystem::exists(argv[2]);
    installed->stop();
    std::ofstream(argv[3]) << motion->active_trace_count() << '\n';
    motion.reset();
    pulp::view::motion::Coordinator::instance().reset();
    return stopped ? 0 : 68;
}
