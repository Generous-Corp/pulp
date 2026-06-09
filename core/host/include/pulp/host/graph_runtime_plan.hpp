#pragma once

#include <pulp/graph/graph_runtime_plan.hpp>
#include <pulp/host/graph_types.hpp>

namespace pulp::host {

// Source-compatibility aliases for the old host-owned graph runtime headers.
// Canonical symbols and SDK ownership live in pulp::graph.
using ::pulp::graph::GraphRuntimeConnectionPlan;
using ::pulp::graph::GraphRuntimeConnectionSpec;
using ::pulp::graph::GraphRuntimeLimits;
using ::pulp::graph::GraphRuntimeNodeKind;
using ::pulp::graph::GraphRuntimeNodePlan;
using ::pulp::graph::GraphRuntimeNodeSpec;
using ::pulp::graph::GraphRuntimePlan;
using ::pulp::graph::GraphRuntimePlanError;
using ::pulp::graph::GraphRuntimePlanErrorCode;
using ::pulp::graph::GraphRuntimePlanResult;
using ::pulp::graph::build_graph_runtime_plan;

} // namespace pulp::host
