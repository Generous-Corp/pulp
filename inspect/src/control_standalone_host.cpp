#include <pulp/inspect/control_standalone_host.hpp>

#include <pulp/inspect/control_host_connection.hpp>
#include <pulp/inspect/control_host_preflight.hpp>
#include <pulp/inspect/control_state_read_executor.hpp>

#include <chrono>
#include <optional>

namespace pulp::inspect {
namespace {

using namespace std::chrono_literals;

#if defined(_MSC_VER)
#define PULP_CONTROL_COMPONENT_MARKER __declspec(dllexport)
#else
#define PULP_CONTROL_COMPONENT_MARKER __attribute__((used, visibility("default")))
#endif

extern "C" PULP_CONTROL_COMPONENT_MARKER const volatile char
    pulp_control_standalone_host_markers_v1[] =
        "PULP_INSPECT_SHIPPING_MANIFEST_V1\0"
        "PULP_INSPECT_CAPABILITY_SESSION_DESCRIBE_V1\0"
        "PULP_INSPECT_CAPABILITY_STATE_READ_V1";

#undef PULP_CONTROL_COMPONENT_MARKER

ControlExecutionOutcome unavailable_operation() {
    return {
        .terminal_state = ControlReceiptState::Failed,
        .result = {.result_code = ControlResultCode::NotBuilt,
                   .explanation =
                       "this Standalone host does not compose the requested typed executor"},
    };
}

class CanonicalStandaloneControlHost final : public format::StandaloneControlHost {
  public:
    bool start(format::Processor&, state::StateStore& store) override {
        const auto handle = inherited_control_host_bootstrap_handle();
#ifdef _WIN32
        if (handle == nullptr)
#else
        if (handle < 0)
#endif
            return true;

        ControlHostPreflightDiagnostics diagnostics;
        auto bootstrap = receive_control_host_preflight(
            handle, 10s, std::chrono::system_clock::now(), &diagnostics);
        if (!bootstrap || bootstrap->enrollment_id.empty())
            return false;

        store_ = &store;
        const auto state_read = make_control_state_read_executor(
            [this](const ControlAdmissionPlan& plan)
                -> std::optional<ControlStateReadSource> {
                if (!store_)
                    return std::nullopt;
                const auto generation = stable_generation();
                if (!generation)
                    return std::nullopt;
                return ControlStateReadSource{
                    .registration_id = plan.registration_id,
                    .host_tier = ControlHostTier::Standalone,
                    .store = store_,
                    .state_generation = *generation,
                    .catalog_generation = store_->parameter_display_revision() + 1,
                    .is_sensitive = [](state::ParamID) { return false; }};
            });
        connection_ = std::make_unique<ControlHostConnection>(
            ControlHostConnectionConfig{
                .endpoint_path = bootstrap->endpoint_path,
                .expected_broker = bootstrap->expected_broker,
                .connect_timeout = 3s,
                .write_timeout = 3s,
                .frame_read_timeout = 3s,
            },
            [state_read](const ControlAdmissionPlan& plan,
                         const ControlRequestEnvelope& request,
                         const ControlExecutionContext& context) {
                if (request.operation_id == "dev.pulp.state/read@1")
                    return state_read(plan, request, context);
                return unavailable_operation();
            });
        if (!connection_->connect()) {
            connection_.reset();
            return false;
        }
        const auto opened = connection_->open_host_enrollment(bootstrap->enrollment_id, 3s);
        if (!opened.accepted || opened.registration_id.empty()) {
            connection_->disconnect();
            connection_.reset();
            return false;
        }
        return true;
    }

    void stop() noexcept override {
        if (connection_)
            connection_->disconnect();
        connection_.reset();
        store_ = nullptr;
    }

  private:
    std::unique_ptr<ControlHostConnection> connection_;
    state::StateStore* store_ = nullptr;

    std::optional<std::uint64_t> stable_generation() const noexcept {
        if (!store_)
            return std::nullopt;
        const auto state = store_->state_generation();
        if (!store_->state_snapshot_is_current(state))
            return std::nullopt;
        return state;
    }
};

} // namespace

std::unique_ptr<format::StandaloneControlHost> make_control_standalone_host() {
    return std::make_unique<CanonicalStandaloneControlHost>();
}

} // namespace pulp::inspect
