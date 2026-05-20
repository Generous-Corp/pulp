// cmd_tweaks.cpp — `pulp tweaks diff` subcommand (Inspector Phase 2).
//
// Compares the inspector tweak sidecar (pulp-tweaks.json) against a
// design snapshot and reports which stored tweaks still apply cleanly,
// which drifted (anchor survives but the property is gone), and which
// are orphaned (the anchor itself is gone). This is the CLI mirror of
// the inspector overlay's drift drawer — same TweakStore::diff() logic
// underneath, so the two surfaces never disagree.
// Spec: planning/2026-05-18-inspector-direct-manipulation-roadmap.md
//       (Phase 2 — Drift UI + CLI).
//
// The design snapshot is supplied via `--design <file>`, a small JSON
// "anchors manifest" in one of three accepted shapes:
//
//   1. A flat array of anchor strings:
//        ["anchor-a", "anchor-b", ...]
//   2. An object with an `anchors` array (anchor-only matching):
//        { "anchors": ["anchor-a", "anchor-b"] }
//   3. An object whose `anchors` is a map of anchor → property paths
//      (enables property-level drift detection):
//        { "anchors": { "anchor-a": ["paint.color", "layout.padding"] } }
//
// When `--design` is omitted the design snapshot is empty, so every
// stored tweak is reported as orphaned — useful as a "what tweaks do I
// have, and would any survive a from-scratch reimport" sanity check.

#include "cli_common.hpp"

#include <pulp/inspect/tweak_store.hpp>

#include <choc/text/choc_JSON.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using pulp::inspect::TweakStore;

void print_tweaks_usage() {
    std::cout << "pulp tweaks — inspect the pulp-tweaks.json sidecar\n\n";
    std::cout << "Usage: pulp tweaks <subcommand> [options]\n\n";
    std::cout << "Subcommands:\n";
    std::cout << "  diff   Compare stored tweaks against a design snapshot\n";
    std::cout << "         and report clean / drifted / orphaned tweaks.\n\n";
    std::cout << "`pulp tweaks diff` options:\n";
    std::cout << "  --tweaks FILE   Path to pulp-tweaks.json\n";
    std::cout << "                  (default: auto-resolved project sidecar)\n";
    std::cout << "  --design FILE   Anchors manifest JSON to diff against\n";
    std::cout << "                  (default: empty — every tweak orphaned)\n";
    std::cout << "  --json          Emit the report as JSON instead of text\n";
    std::cout << "\nExit code is 0 when no drift, 1 when drift is found,\n";
    std::cout << "2 on a usage / file error.\n";
}

bool read_file_to_string(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// Parse a `--design` anchors manifest into a DesignSnapshot. Returns
// false (with `err` set) on a malformed file. An empty `path` yields an
// empty snapshot — a valid "design has nothing" state.
bool load_design_snapshot(const std::string& path,
                          TweakStore::DesignSnapshot& snap,
                          std::string& err) {
    if (path.empty()) return true;  // empty snapshot is valid

    std::string content;
    if (!read_file_to_string(path, content)) {
        err = "could not read design file: " + path;
        return false;
    }

    choc::value::Value parsed;
    try {
        parsed = choc::json::parse(content);
    } catch (const std::exception& e) {
        err = std::string("design file is not valid JSON: ") + e.what();
        return false;
    } catch (...) {
        err = "design file is not valid JSON";
        return false;
    }

    // Accept either the bare array form or the {"anchors": ...} wrapper.
    choc::value::Value anchors;
    if (parsed.isArray()) {
        anchors = parsed;
    } else if (parsed.isObject() && parsed.hasObjectMember("anchors")) {
        anchors = parsed["anchors"];
    } else {
        err = "design file must be an anchor array or an object with "
              "an `anchors` member";
        return false;
    }

    if (anchors.isArray()) {
        // Flat anchor list — anchor-only matching.
        for (uint32_t i = 0; i < anchors.size(); ++i) {
            if (anchors[i].isString())
                snap.anchors.insert(std::string(anchors[i].getString()));
        }
    } else if (anchors.isObject()) {
        // Map of anchor → property-path array. Enables property-level
        // drift detection.
        for (uint32_t i = 0; i < anchors.size(); ++i) {
            auto member = anchors.getObjectMemberAt(i);
            std::string anchor(member.name);
            snap.anchors.insert(anchor);
            auto& props = snap.properties[anchor];
            if (member.value.isArray()) {
                for (uint32_t j = 0; j < member.value.size(); ++j) {
                    if (member.value[j].isString())
                        props.insert(
                            std::string(member.value[j].getString()));
                }
            }
        }
    } else {
        err = "`anchors` must be an array or an object";
        return false;
    }
    return true;
}

// Render a single tweak's stored value compactly for human output.
std::string value_preview(const choc::value::Value& v) {
    if (v.isString()) return std::string(v.getString());
    try {
        return choc::json::toString(v);
    } catch (...) {
        return "?";
    }
}

int run_tweaks_diff(const std::vector<std::string>& args) {
    std::string tweaks_path;
    std::string design_path;
    bool as_json = false;

    for (size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "--json") {
            as_json = true;
        } else if (a == "--tweaks") {
            if (i + 1 >= args.size()) {
                std::cerr << "Error: --tweaks requires a value\n";
                return 2;
            }
            tweaks_path = args[++i];
        } else if (a == "--design") {
            if (i + 1 >= args.size()) {
                std::cerr << "Error: --design requires a value\n";
                return 2;
            }
            design_path = args[++i];
        } else if (a == "--help" || a == "-h") {
            print_tweaks_usage();
            return 0;
        } else {
            std::cerr << "Error: unknown `tweaks diff` argument: " << a << "\n";
            std::cerr << "Run `pulp tweaks --help` for usage.\n";
            return 2;
        }
    }

    // Load the tweak sidecar. An absent file is a usage error — there
    // is nothing to diff.
    TweakStore store;
    auto load = store.load_from_disk(tweaks_path);
    if (!load.ok) {
        std::cerr << "Error: " << load.error << "\n";
        if (tweaks_path.empty()) {
            std::cerr << "       (looked for the project sidecar at "
                      << load.path << ")\n";
            std::cerr << "       Pass --tweaks FILE to point at an "
                         "explicit pulp-tweaks.json.\n";
        }
        return 2;
    }

    // Build the design snapshot from --design (empty if omitted).
    TweakStore::DesignSnapshot snap;
    std::string design_err;
    if (!load_design_snapshot(design_path, snap, design_err)) {
        std::cerr << "Error: " << design_err << "\n";
        return 2;
    }

    auto report = store.diff(snap);

    if (as_json) {
        std::cout << TweakStore::drift_report_to_json(report) << "\n";
        return report.has_drift() ? 1 : 0;
    }

    // ── Human-readable output ──────────────────────────────────────
    std::cout << "Tweaks: " << load.path << "\n";
    if (design_path.empty()) {
        std::cout << "Design: (none — every tweak treated as orphaned)\n";
    } else {
        std::cout << "Design: " << design_path << " ("
                  << snap.anchors.size() << " anchors)\n";
    }
    std::cout << "\n";
    std::cout << "  clean    " << report.clean.size() << "\n";
    std::cout << "  drifted  " << report.drifted.size() << "\n";
    std::cout << "  orphaned " << report.orphaned.size() << "\n";
    std::cout << "  ───────────\n";
    std::cout << "  total    " << report.total() << "\n\n";

    auto print_drift_rows =
        [](const char* heading,
           const std::vector<TweakStore::DriftedTweak>& list) {
            if (list.empty()) return;
            std::cout << heading << ":\n";
            for (const auto& d : list) {
                std::cout << "  " << d.anchor_id << "  " << d.property_path
                          << " = " << value_preview(d.value) << "  ["
                          << TweakStore::drift_reason_str(d.reason) << "]\n";
            }
            std::cout << "\n";
        };
    print_drift_rows("Orphaned tweaks", report.orphaned);
    print_drift_rows("Drifted tweaks", report.drifted);

    if (!report.has_drift()) {
        std::cout << "No drift — every stored tweak still maps cleanly.\n";
        return 0;
    }
    std::cout << report.orphaned.size() + report.drifted.size()
              << " tweak(s) no longer apply. Open the inspector drift "
                 "drawer to resolve them.\n";
    return 1;
}

}  // namespace

int cmd_tweaks(const std::vector<std::string>& args) {
    if (args.empty()) {
        print_tweaks_usage();
        return 0;
    }
    const std::string& sub = args[0];
    if (sub == "--help" || sub == "-h" || sub == "help") {
        print_tweaks_usage();
        return 0;
    }
    if (sub == "diff") {
        return run_tweaks_diff({args.begin() + 1, args.end()});
    }
    std::cerr << "Error: unknown `tweaks` subcommand: " << sub << "\n";
    std::cerr << "Run `pulp tweaks --help` for usage.\n";
    return 2;
}
