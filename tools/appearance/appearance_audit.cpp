// pulp-appearance-audit — run the painted-appearance defect detectors over a
// scripted UI and print what they saw.
//
// This exists because a defect a user can point at ("those two labels are on
// top of each other") was previously argued from proxies: a value reaching the
// runtime, a node existing in a snapshot. Those can all be true while the
// screen is wrong. This loads the same script a host loads, lays it out, and
// reports painted geometry.
//
// Every run prints a coverage line. An empty finding list from a run that
// measured a fraction of the text on screen is not evidence of absence, and
// the tool exits non-zero rather than let that read as a pass.

#include <pulp/state/store.hpp>
#include <pulp/view/appearance_defects.hpp>
#include <pulp/view/scripted_ui.hpp>
#include <pulp/view/view.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Args {
    std::filesystem::path script;
    float width = 1320.0f;
    float height = 860.0f;
    int frames = 8;
    bool list_runs = false;
    bool allow_untrustworthy = false;
    /// Positive control: inflate every run's ink before the collision pass.
    /// An empty finding list is only evidence of a clean panel if the same
    /// instrument, on the same tree, can be made to report something.
    float control_inflate = 0.0f;
    /// Clicks to drive before measuring, in root coordinates. A panel reached
    /// only by navigating to it cannot be audited from the initial state.
    std::vector<pulp::view::Point> clicks;
};

void usage() {
    std::cerr <<
        "usage: pulp-appearance-audit <script.js> [options]\n"
        "  --width N          viewport width  (default 1320)\n"
        "  --height N         viewport height (default 860)\n"
        "  --frames N         frames to poll before measuring (default 8)\n"
        "  --list-runs        print every measured text run\n"
        "  --click X,Y        click at root coordinates before measuring\n"
        "                     (repeatable; frames are polled between clicks)\n"
        "  --allow-untrustworthy  exit 0 even when coverage is too thin to\n"
        "                     support a clean verdict\n"
        "  --control-inflate N  positive control: grow every run's ink by N px\n"
        "                     before the collision pass. On any panel with text\n"
        "                     this MUST report collisions; if it does not, the\n"
        "                     instrument never reached the panel and a clean run\n"
        "                     means nothing. Findings under inflation describe\n"
        "                     the control, not the panel.\n"
        "\n"
        "exit: 0 clean and trustworthy, 1 defects found, 2 could not run,\n"
        "      3 no defects but coverage too thin for that to mean anything\n";
}

bool parse(int argc, char** argv, Args& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto next = [&](float& dst) {
            if (i + 1 >= argc) return false;
            dst = std::strtof(argv[++i], nullptr);
            return true;
        };
        if (a == "--width") { if (!next(out.width)) return false; }
        else if (a == "--height") { if (!next(out.height)) return false; }
        else if (a == "--frames") {
            if (i + 1 >= argc) return false;
            out.frames = std::atoi(argv[++i]);
        }
        else if (a == "--click") {
            if (i + 1 >= argc) return false;
            const std::string spec = argv[++i];
            const auto comma = spec.find(',');
            if (comma == std::string::npos) return false;
            out.clicks.push_back({std::strtof(spec.substr(0, comma).c_str(), nullptr),
                                  std::strtof(spec.substr(comma + 1).c_str(), nullptr)});
        }
        else if (a == "--control-inflate") { if (!next(out.control_inflate)) return false; }
        else if (a == "--list-runs") out.list_runs = true;
        else if (a == "--allow-untrustworthy") out.allow_untrustworthy = true;
        else if (a == "-h" || a == "--help") return false;
        else if (!a.empty() && a.front() == '-') return false;
        else if (out.script.empty()) out.script = std::string(a);
        else return false;
    }
    return !out.script.empty();
}

} // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse(argc, argv, args)) { usage(); return 2; }
    if (!std::filesystem::exists(args.script)) {
        std::cerr << "appearance-audit: no such script: " << args.script << "\n";
        return 2;
    }

    pulp::view::View root;
    root.set_bounds({0.0f, 0.0f, args.width, args.height});
    pulp::state::StateStore store;

    pulp::view::ScriptedUiOptions options;
    options.script_path = args.script;
    options.enable_hot_reload = false;
    options.enable_theme_reload = false;
    // A materialized browser document mounts through the runtime-import
    // endpoint; without it a materialized bundle loads to an empty tree, which
    // would look exactly like a panel with no defects.
    options.enable_runtime_import = true;

    pulp::view::ScriptedUiSession session(root, store, std::move(options));
    std::string error;
    if (!session.load(&error)) {
        std::cerr << "appearance-audit: script did not load: " << error << "\n";
        return 2;
    }
    auto pump = [&] {
        for (int i = 0; i < args.frames; ++i) {
            std::string poll_error;
            session.poll(&poll_error);
            root.layout_children();
        }
    };
    pump();
    for (const auto& click : args.clicks) {
        root.simulate_click(click);
        pump();
    }

    pulp::view::AppearanceOptions detect_options;
    detect_options.control_inflate_ink_px = args.control_inflate;
    const auto report = pulp::view::detect_appearance_defects(root, detect_options);
    if (args.control_inflate > 0.0f) {
        std::cout << "POSITIVE CONTROL: ink inflated by " << args.control_inflate
                  << "px; collisions below are the control, not the panel\n";
    }
    std::cout << report.to_string() << "\n";

    if (args.list_runs) {
        for (const auto& run : report.runs) {
            std::cout << "  run " << run.paint_order << " " << run.kind
                      << " id=" << (run.node_id.empty() ? "<anon>" : run.node_id)
                      << " ink=(" << run.ink.x << "," << run.ink.y << ","
                      << run.ink.width << "," << run.ink.height << ")"
                      << " text=\"" << run.text << "\"\n";
        }
    }

    // Under the control the meaning of the exit code inverts: finding nothing
    // is the failure, because it means the instrument cannot see this panel.
    if (args.control_inflate > 0.0f) {
        if (report.findings.empty()) {
            std::cerr << "appearance-audit: CONTROL FAILED — inflating ink "
                         "produced no collisions, so the detector is not "
                         "reaching this panel\n";
            return 4;
        }
        return 0;
    }

    if (!report.clean()) return 1;
    if (!report.coverage.trustworthy() && !args.allow_untrustworthy) return 3;
    return 0;
}
