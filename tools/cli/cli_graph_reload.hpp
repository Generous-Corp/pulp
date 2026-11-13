#pragma once
#include <memory>
namespace pulp::host {
class GraphFileWatch;
}
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct GraphReloadOptions {
    std::filesystem::path file;
    std::string instance;
    std::string grant;
    std::uint64_t generation = 0;
    bool generation_set = false;
    unsigned watch_polls = 0;
};
// Returns true for a consumed option; an error is returned separately.
bool parse_graph_reload_option(const std::vector<std::string>& args, std::size_t& index,
                               GraphReloadOptions& options, std::string& error);
bool validate_graph_reload_options(const GraphReloadOptions& options, std::string& error);
// Construction reads the file once as the baseline, so a watch never
// reapplies the contents it started with.
class GraphReloadWatch {
  public:
    explicit GraphReloadWatch(GraphReloadOptions options);
    ~GraphReloadWatch();
    void poll();

  private:
    void report_file_refusal(int code) const;
    unsigned polls_ = 0;
    bool lease_ready_ = false;
    bool halted_ = false;
    GraphReloadOptions options_;
    std::unique_ptr<pulp::host::GraphFileWatch> watch_;
};
