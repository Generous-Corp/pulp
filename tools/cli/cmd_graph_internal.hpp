// cmd_graph_internal.hpp: private shared surface for the `pulp graph`
// translation units.
//
//   cmd_graph.cpp          dispatcher plus the inspect, validate, fmt, save and
//                          canonicalize handlers
//   cmd_graph_common.cpp   code names, the JSON error envelope and the bounded
//                          document reader shared by the handlers
//   cmd_graph_probe.cpp    `pulp graph probe`
//   cmd_graph_render.cpp   `pulp graph render` and `pulp graph report`
//   cmd_graph_diff.cpp     `pulp graph diff`
//
// This header lives next to the sources under tools/cli/ and is not part of
// any installed include tree.

#pragma once

#include <pulp/host/graph_patch.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/signal_graph.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pulp_cli::graph_detail {

using Code = pulp::host::GraphPatchDiagnostic::Code;

// Failures that C++ callers also see (loading a graph or notation file) use
// the GraphPatchDiagnostic code names verbatim; the CLI never renames them.
std::string patch_code(Code code);

// GraphSerializer::from_file reports file failures as text; an oversize file
// is the same ResourceLimit that validate and diff report for it.
Code file_load_code(const pulp::host::GraphSerializer::LoadResult& loaded);

// Prints the `{"valid":false,"code":...,"message":...}` envelope to stdout.
void json_error(const std::string& code, const std::string& message);

// Reads one saved document without parsing it. The bound matches the
// serializer's file limit so a diff or rewrite never reads more than a load
// would.
std::optional<std::string> read_graph_document(const std::string& path, Code& code,
                                               std::string& message);

int probe_graph(const std::vector<std::string>& args);
int report_graph(const std::vector<std::string>& args);
int diff_graphs(const std::vector<std::string>& args);

struct RenderOptions {
    std::string input;
    std::string output;
    std::uint64_t frames = 0; // 0 = input length, or the default length without input
};

std::optional<RenderOptions> parse_render_options(const std::vector<std::string>& args,
                                                  std::string& code, std::string& message);

int render_graph(pulp::host::SignalGraph& graph,
                 const pulp::host::GraphSerializer::LoadResult& loaded,
                 const RenderOptions& options, bool json);

} // namespace pulp_cli::graph_detail
