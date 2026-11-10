#pragma once

#include <pulp/host/graph_serializer.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::host {

/// Control-thread diagnostics shared by notation, strict file validation and tools.
/// Mutation codes retain GraphResult's spellings. Supplementary fields identify
/// the source line and give an actionable correction; no graph is returned on failure.
struct GraphPatchDiagnostic {
    enum class Code {
        Ok, UnknownNode, UnknownPort, KindMismatch, IllegalCycle, Duplicate,
        SyntaxError, InvalidDocument, UnsupportedContract, ParameterOutOfRange,
        RestoreFailed, ResourceLimit, FileUnreadable
    };
    Code code = Code::Ok;
    std::size_t line = 0;
    std::string node;
    std::string port;
    std::string rule;
    std::string suggested_fix;
    explicit operator bool() const noexcept { return code == Code::Ok; }
    std::string message() const;
};
const char* graph_patch_code_name(GraphPatchDiagnostic::Code code) noexcept;
std::string graph_patch_diagnostic_json(const GraphPatchDiagnostic& diagnostic);

struct GraphPatchResult {
    GraphPatchDiagnostic diagnostic;
    std::unique_ptr<SignalGraph> graph;
    explicit operator bool() const noexcept { return bool(diagnostic) && bool(graph); }
};
struct GraphPatchText {
    GraphPatchDiagnostic diagnostic;
    std::string text;
    explicit operator bool() const noexcept { return bool(diagnostic); }
};

/// Projection of existing runtime registrations, not a DSP inventory. Processor
/// descriptors are registered once here into the existing restore registry;
/// port names and parameter metadata are read from constructed runtime nodes.
/// Finish registration before use. Factories must return fresh instances and
/// must not mutate external state. Concurrent access is unsupported.
class GraphPatchCatalog {
  public:
    ProcessorRestoreCode register_processor(ProcessorRestoreDescriptor descriptor);
    bool register_custom(CustomNodeType type);
    std::unique_ptr<SignalGraph> make_graph() const;
    NodeId instantiate(SignalGraph& graph, std::string_view type, int version,
                       const std::string& name) const;
    GraphLoadOptions load_options() const;
    std::string to_json() const;

  private:
    mutable ProcessorRestoreRegistry processors_;
    std::vector<ProcessorRestoreDescriptor> descriptors_;
    std::vector<CustomNodeType> custom_;
};

/// Extends patch-language v1 declarations, named edges, chain/fanout and values.
/// `~> parameter @ depth` maps [0,1] to [range.min, min+depth*span] through the
/// existing sparse automation route. Dense, additive and smoothed routes are
/// explicitly unsupported in this notation profile.
GraphPatchResult parse_graph_patch(std::string_view text, const GraphPatchCatalog& catalog = {});
GraphPatchText format_graph_patch(const SignalGraph& graph, const GraphPatchCatalog& catalog = {});

/// Additive strict loader: rebuilds an isolated candidate and validates every
/// original edge before returning it. GraphSerializer's tolerant legacy loader
/// is unchanged. Unsupported contracts are refused, never silently discarded.
GraphPatchResult load_graph_patch_json(std::string_view json,
                                      const GraphPatchCatalog& catalog = {});
GraphPatchResult load_graph_patch_file(const std::filesystem::path& path,
                                      const GraphPatchCatalog& catalog = {});

} // namespace pulp::host
