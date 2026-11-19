#pragma once

#include <memory>
#include <optional>
#include <string>

namespace pulp::host {
struct GraphLoadOptions;
struct PluginInfo;
} // namespace pulp::host

namespace pulp::host::detail {

// Resolves plugin nodes for one GraphSerializer::from_json call. Native
// builds check the stored path against the known plugin roots and scan those
// roots at most once per format; the web build has no plugin scanner, so
// every plugin node stays an unresolved placeholder.
class PluginNodeResolver {
  public:
    explicit PluginNodeResolver(const GraphLoadOptions& options);
    ~PluginNodeResolver();

    PluginNodeResolver(const PluginNodeResolver&) = delete;
    PluginNodeResolver& operator=(const PluginNodeResolver&) = delete;

    // Returns the path to load, or nullopt with `reason` set.
    std::optional<std::string> resolve(const PluginInfo& info, std::string& reason);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::host::detail
