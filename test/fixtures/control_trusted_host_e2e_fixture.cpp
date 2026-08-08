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

#include <choc/text/choc_JSON.h>

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
    "PULP_CONTROL_MANIFEST_SHA256_4c761fc1d01625a6dae0c7d47e455901f5023a063a812c3322bf50161c8f7534_"
    "V1";
const volatile char kSessionDescribe[] = "PULP_INSPECT_CAPABILITY_SESSION_DESCRIBE_V1";
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
    if ((argc != 3 && argc != 4) || kStandalone[0] != 'P' || kShipping[0] != 'P' ||
        kProfile[0] != 'P' || kManifest[0] != 'P' || kSessionDescribe[0] != 'P' ||
        kSessionControl[0] != 'P' || kTraceControl[0] != 'P')
        return 64;

    ControlHostPreflightDiagnostics diagnostics;
    auto bootstrap = receive_control_host_preflight(inherited_control_host_bootstrap_handle(), 10s,
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
    if (!executor)
        return 68;

    auto routed_executor = executor->executor();
    if (argc == 4) {
        routed_executor = [fallback = std::move(routed_executor), ready_path = argv[3]](
                              const ControlAdmissionPlan& plan,
                              const ControlRequestEnvelope& request,
                              const ControlExecutionContext& context) {
            if (request.operation_id == "dev.pulp.trace/session-control@1") {
                try {
                    const auto params = choc::json::parse(request.params_json);
                    if (params["action"].getString() == "start" &&
                        params["ring_mb"].getWithDefault<std::int64_t>(0) == 9) {
                        std::ofstream(ready_path) << "deferred\n";
                        return ControlExecutionOutcome{.deferred = true};
                    }
                } catch (...) {
                    // The normal executor owns validation and error projection.
                }
            }
            return fallback(plan, request, context);
        };
    }
    if (!slot.install(std::move(routed_executor)))
        return 68;

    std::ofstream(argv[1]) << opened.registration_id << '\n'
#ifdef __APPLE__
                           << ::getpid() << '\n'
#else
                           << 0 << '\n'
#endif
        ;
    // Installed process tests own teardown explicitly and may exercise several
    // client timeout budgets before signalling this fixture to stop.
    for (unsigned attempt = 0; attempt < 60'000 && !std::filesystem::exists(argv[2]); ++attempt)
        std::this_thread::sleep_for(1ms);
    const bool stopped = std::filesystem::exists(argv[2]);
    slot.close();
    connection.disconnect();
    return stopped ? 0 : 69;
}
