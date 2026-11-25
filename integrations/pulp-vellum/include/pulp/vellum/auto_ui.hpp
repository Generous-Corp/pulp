#pragma once

/// @file
/// AutoUi on Vellum views: the generated parameter editor for a processor
/// that declares no UI of its own. A knob per continuous parameter (with its
/// name and value rows beneath it), a toggle per two-state one, wrapped in a
/// centered grid, one titled section per registered parameter group, and a
/// vertical scroll once the grid outgrows the window. Every control is bound
/// through `vellum::view::bind_parameter` over the editor's `ParamModel`, so
/// a drag is one host gesture, automation turns the control, and host
/// modulation is drawn, with no plug-in code.

#include <pulp/format/editor_host.hpp>

#include <vellum/value/param_model.hpp>
#include <vellum/view/view.hpp>

#include <cstdint>
#include <memory>

namespace pulp::format {
class EditorFactory;
}

namespace pulp::vellum {

class AutoUi {
  public:
    /// Logical-pixel size that fits the generated grid without clipping.
    struct SizeHint {
        std::uint32_t width;
        std::uint32_t height;
    };

    /// Build the editor for the parameters `port` lists, bound through
    /// `model`, which must resolve each parameter by its decimal id (as
    /// `ParamPortModel` does) and outlive the returned tree.
    static std::unique_ptr<::vellum::view::View> build(const format::ParamPort& port,
                                                       ::vellum::value::ParamModel& model);

    /// A design size that fits every tile `build()` makes for `port`: the
    /// grid wrapped to a roughly square column count, the title, padding and
    /// group boxes. Never smaller than 320 x 240.
    static SizeHint preferred_size(const format::ParamPort& port);
};

/// The AutoUi editor as an editor factory, for a plug-in that opts into the
/// Vellum editor without a script (`UI_ENGINE vellum` with no `UI_SCRIPT`).
/// It opens at the processor's declared size, or, when the processor keeps
/// the SDK default (400 x 300 with no bounds), at `preferred_size()` with
/// resizable bounds derived from it.
format::EditorFactory* auto_ui_editor_factory();

} // namespace pulp::vellum

#include <pulp/vellum/detail/auto_ui_impl.hpp>
