#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <pulp/host/graph_patch.hpp>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace pulp::host {

enum class GraphFileFormat { Notation, Pulpgraph };
struct GraphFileChange {
    enum class Code { Unchanged, Pending, Changed, Unreadable, TooLarge, UnsupportedFormat };
    Code code = Code::Unchanged;
    GraphFileFormat format = GraphFileFormat::Notation;
    std::string text;
};

/// One bounded poll; no thread or timer. Two equal consecutive reads debounce
/// a save. Rejections are consumed once; identical contents do not retrigger.
class GraphFileWatch final {
  public:
    explicit GraphFileWatch(std::filesystem::path path) : path_(std::move(path)) {}
    GraphFileChange poll();

  private:
    std::filesystem::path path_;
    std::optional<std::string> pending_, observed_;
    GraphFileChange::Code previous_error_ = GraphFileChange::Code::Unchanged;
};
enum class GraphFileReloadCode {
    Prepared,
    Committed,
    InvalidDocument,
    UnsupportedContract,
    InvalidMutation,
    PrepareFailed,
    StaleBase,
    CommitFailed
};
const char* graph_file_reload_code_name(GraphFileReloadCode code) noexcept;

struct GraphFileReloadResult {
    GraphFileReloadCode code = GraphFileReloadCode::InvalidDocument;
    GraphPatchDiagnostic diagnostic;
    SignalGraph::PreparedTopologyEdit::Result topology_result =
        SignalGraph::PreparedTopologyEdit::Result::NotPrepared;
    explicit operator bool() const noexcept {
        return code == GraphFileReloadCode::Prepared || code == GraphFileReloadCode::Committed;
    }
};

/// Control-thread whole-file replacement through the existing prepared edit.
/// Parsing, state restoration and preparation cannot change the live graph.
/// Both formats refuse input larger than 1 MiB before candidate construction.
/// Commit uses the graph's existing publication boundary and stale-base guard.
/// Authored values replace prior values; opaque/unrepresentable contracts are
/// refused. This does not migrate unsaved DSP history between new instances.
class GraphFileReloadTransaction final {
  public:
    explicit GraphFileReloadTransaction(SignalGraph& graph) : graph_(graph) {}
    GraphFileReloadResult prepare(std::string_view text, GraphFileFormat format,
                                  const GraphPatchCatalog& catalog, double sample_rate,
                                  int maximum_block_size);
    GraphFileReloadResult commit();

  private:
    SignalGraph& graph_;
    std::unique_ptr<SignalGraph::PreparedTopologyEdit> edit_;
    bool attempted_ = false;
    bool ready_ = false;
};

} // namespace pulp::host
