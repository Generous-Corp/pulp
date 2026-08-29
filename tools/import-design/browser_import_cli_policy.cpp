#include "browser_import_cli.hpp"

#include <iostream>

namespace pulp::import_design {

std::optional<int> validate_browser_import_cli_options(
    bool fit_authored_frame,
    bool render_size_explicit,
    bool has_browser_interactions,
    bool offline,
    bool export_tokens,
    bool detect_only,
    bool native_panel_lowering,
    bool materialized_canvas_composition) {
    if (fit_authored_frame) {
        const char* conflict = render_size_explicit       ? "--render-size"
                               : has_browser_interactions ? "--browser-interactions"
                               : offline                  ? "--offline"
                               : export_tokens            ? "--export-tokens"
                                                          : nullptr;
        if (conflict) {
            std::cerr << "Error: --fit-authored-frame cannot be combined with "
                      << conflict << "\n";
            return 2;
        }
        if (detect_only) {
            std::cerr << "Error: --fit-authored-frame cannot be combined with "
                         "--detect-only or --report-new-format\n";
            return 2;
        }
    }
    if (native_panel_lowering && materialized_canvas_composition) {
        std::cerr << "Error: --native-panel-lowering and "
                     "--materialized-canvas-composition are mutually exclusive\n";
        return 2;
    }
    return std::nullopt;
}

std::optional<int> validate_fit_authored_frame_source_cli(
    bool fit_authored_frame,
    std::string_view source) {
    if (!fit_authored_frame || source == "html" || source == "claude" ||
        source == "stitch") {
        return std::nullopt;
    }
    std::cerr << "Error: --fit-authored-frame applies only to browser-solved "
                 "runnable HTML\n";
    return 2;
}

} // namespace pulp::import_design
