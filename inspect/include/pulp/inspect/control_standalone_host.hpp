#pragma once

#include <pulp/format/standalone_control_host.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

namespace pulp::inspect {

class RuntimeEvaluator;
enum class ControlTelemetrySensitivity : std::uint8_t;

} // namespace pulp::inspect

namespace pulp::view::motion {
struct RenderCostSnapshot;
}

namespace pulp::format {
class ViewBridge;
}

namespace pulp::inspect {

/// Creates the canonical broker-enrolled host bridge for an explicitly
/// control-enabled Standalone executable. A direct launch remains inert; only
/// the broker's inherited, kernel-authenticated bootstrap can open the host.
std::unique_ptr<format::StandaloneControlHost> make_control_standalone_host();

namespace detail {

struct StandaloneControlAuthorHooks {
    std::function<ControlTelemetrySensitivity(std::string_view)> telemetry_classifier;
    std::function<view::motion::RenderCostSnapshot()> motion_cost_probe;
    std::filesystem::path motion_fixture_path;
};

using StandaloneControlAuthorHooksFactory =
    StandaloneControlAuthorHooks (*)(format::Processor&);

/// Installs optional author-owned observability inputs. Unspecified telemetry
/// remains sensitive, and absent Motion probes/fixtures remain unavailable.
bool install_standalone_control_author_hooks_factory(
    StandaloneControlAuthorHooksFactory factory) noexcept;
StandaloneControlAuthorHooks
create_standalone_control_author_hooks(format::Processor& processor);

using StandaloneRuntimeEvaluatorFactory =
    std::shared_ptr<RuntimeEvaluator> (*)(format::Processor&, format::ViewBridge&);

/// Installed only by a research-unsafe author target that also links the
/// separately shipped high-risk evaluator archive.
bool install_standalone_runtime_evaluator_factory(
    StandaloneRuntimeEvaluatorFactory factory) noexcept;
std::shared_ptr<RuntimeEvaluator>
create_standalone_runtime_evaluator(format::Processor& processor,
                                    format::ViewBridge& bridge);

} // namespace detail

} // namespace pulp::inspect
