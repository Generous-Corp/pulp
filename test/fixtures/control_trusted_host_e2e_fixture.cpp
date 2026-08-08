#include <pulp/inspect/control_executor_slot.hpp>
#include <pulp/inspect/control_host_connection.hpp>
#include <pulp/inspect/control_host_preflight.hpp>
#include <pulp/inspect/control_trace_session_executor.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <thread>

using namespace std::chrono_literals;
using namespace pulp::inspect;

namespace {
const volatile char kStandalone[] = "PULP_STANDALONE_COMPONENT_V1";
const volatile char kShipping[] = "PULP_INSPECT_SHIPPING_MANIFEST_V1";
const volatile char kProfile[] = "PULP_CONTROL_PROFILE_DEVELOPER_LOCAL_V1";
const volatile char kManifest[] =
    "PULP_CONTROL_MANIFEST_SHA256_4ae42112af764b142f9852ceeb25009a2b95a3fbe10e529fb1bf5caa8b8c8a92_"
    "V1";
const volatile char kSessionControl[] = "PULP_INSPECT_CAPABILITY_SESSION_CONTROL_V1";
const volatile char kTraceControl[] = "PULP_INSPECT_CAPABILITY_TRACE_SESSION_CONTROL_V1";

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
    if (argc != 3 || kStandalone[0] != 'P' || kShipping[0] != 'P' || kProfile[0] != 'P' ||
        kManifest[0] != 'P' || kSessionControl[0] != 'P' || kTraceControl[0] != 'P')
        return 64;

    ControlHostPreflightDiagnostics diagnostics;
    auto bootstrap = receive_control_host_preflight(inherited_control_host_bootstrap_handle(), 3s,
                                                    std::chrono::system_clock::now(), &diagnostics);
    if (!bootstrap || bootstrap->enrollment_id.empty())
        return 65;

    ControlOperationExecutorSlot slot;
    ControlHostConnection connection({.endpoint_path = bootstrap->endpoint_path,
                                      .expected_broker = bootstrap->expected_broker,
                                      .connect_timeout = 3s,
                                      .write_timeout = 3s,
                                      .frame_read_timeout = 3s},
                                     slot.executor());
    if (!connection.connect())
        return 66;
    const auto opened = connection.open_host_enrollment(bootstrap->enrollment_id);
    if (!opened.accepted || opened.registration_id.empty())
        return 67;

    auto trace = std::make_shared<TraceInspector>();
    auto executor = ControlTraceSessionExecutor::create({
        .main_thread_rpc = inline_rpc(),
        .trace_inspector = trace,
        .registration_id = ControlRegistrationId{opened.registration_id},
    });
    if (!executor || !slot.install(executor->executor()))
        return 68;

    std::ofstream(argv[1]) << opened.registration_id << '\n';
    for (unsigned attempt = 0; attempt < 10'000 && !std::filesystem::exists(argv[2]); ++attempt)
        std::this_thread::sleep_for(1ms);
    const bool stopped = std::filesystem::exists(argv[2]);
    slot.close();
    connection.disconnect();
    return stopped ? 0 : 69;
}
