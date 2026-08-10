#pragma once

// What a built module actually is, in five lines.
//
// The prototype's left column carried a spec beneath the description: width,
// controls, I/O, DSP, panel. The app showed the prompt and a verdict and
// nothing about what had been made, so the only way to find out what you had
// was to open Rack.
//
// EVERY row is derived from the module's own generated manifest. None is
// retyped, and a row that cannot be derived is not shown: a spec that
// disagrees with the module is worse than no spec, because it is believed.

#include <pulp/view/view.hpp>

#include <string>
#include <utility>
#include <vector>

namespace forge_modular {

class ModuleSummary : public pulp::view::View {
public:
    /// Read a generated module manifest. False when there is nothing to show,
    /// in which case the view stays empty rather than inventing rows.
    bool set_manifest(const std::string& path);

    /// The rows as they will be drawn, so a test can assert them against the
    /// manifest they came from rather than against a screenshot.
    const std::vector<std::pair<std::string, std::string>>& rows() const {
        return rows_;
    }

    /// The module's own description, if its manifest carries one.
    const std::string& description() const { return description_; }

private:
    void rebuild();

    std::vector<std::pair<std::string, std::string>> rows_;
    std::string description_;
};

}  // namespace forge_modular
