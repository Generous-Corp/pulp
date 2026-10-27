// SPDX-License-Identifier: MIT
#pragma once

#include <pulp/view/design_ir.hpp>
#include <pulp/view/view.hpp>

#include <filesystem>
#include <memory>

namespace pulp::import_design {

/// A DesignIR observed through the ui.js lane: the program generate_pulp_js()
/// emits for it, run through the WidgetBridge the way a plugin loads ui.js.
/// The engine and bridge that own the program live as long as this object, and
/// are torn down before the tree they built.
class ObservedScript {
  public:
    ~ObservedScript();
    ObservedScript(const ObservedScript&) = delete;
    ObservedScript& operator=(const ObservedScript&) = delete;

    /// The host view the program builds into, sized @p width x @p height.
    /// Relative image paths resolve against @p asset_directory, the IR's own
    /// directory.
    static std::unique_ptr<ObservedScript> run(const pulp::view::DesignIR& ir,
                                               const std::filesystem::path& asset_directory,
                                               float width, float height);

    pulp::view::View& root();

  private:
    struct State;
    explicit ObservedScript(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

} // namespace pulp::import_design
