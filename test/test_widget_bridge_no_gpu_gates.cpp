// Regression test: every call into pulp::render::GpuSurface methods from
// widget_bridge.cpp must be gated behind PULP_WIDGET_BRIDGE_HAS_GPU_SURFACE
// (or, equivalently, the PULP_HAS_SKIA #else branch that already implies the
// header is reachable).
//
// Why this test exists: the file forward-declares render::GpuSurface in
// widget_bridge.hpp and pulls in the full definition only via
// `__has_include(<pulp/render/gpu_surface.hpp>)`. When PULP_ENABLE_GPU=OFF
// (iOS Simulator default, sanitizer matrix), the render module is not added
// as a subdirectory, the include path is not propagated, and any
// `gpu_surface_->member()` call outside the gate becomes a compile error
// ("member access into incomplete type 'render::GpuSurface'").
//
// Ungated calls to gpu_surface_->has_surface() in __gpuCanvasConfigureImpl and
// __gpuCanvasDescribeCurrentTextureImpl broke iOS Simulator AUv3 configure
// end-to-end. The macOS required build lane defaults to GPU=ON, so it never
// compiled the no-GPU TU; the sanitizer matrix that DOES set PULP_ENABLE_GPU=OFF
// is advisory + path-filtered and the slow `cmake-ios-auv3-configure`
// end-to-end test is excluded from PR runs.
//
// This test is a fast, deterministic static check: it scans widget_bridge.cpp
// line by line, tracks the preprocessor gate state, and asserts that every
// `gpu_surface_->` dereference sits inside a region where the full
// GpuSurface type is visible. Runs in milliseconds in the default macOS PR
// lane — no Xcode, no iOS toolchain required.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct GateFrame {
    enum class Kind { HasGpuSurface, NotHasSkia, HasSkia, Other };
    Kind kind = Kind::Other;
    bool in_else_branch = false;
};

// Returns true if the current preprocessor stack guarantees the full
// pulp::render::GpuSurface type is reachable in the surrounding TU.
//
// Two patterns count as "guarantees visibility":
//   1. `#if PULP_WIDGET_BRIDGE_HAS_GPU_SURFACE` — explicit gate added when
//      __has_include(<pulp/render/gpu_surface.hpp>) succeeded.
//   2. `#ifndef PULP_HAS_SKIA ... #else` — the #else branch implies Skia is
//      on, and PULP_HAS_SKIA being defined implies the render module was
//      built and its include path is on the compile line.
//
// `#ifdef PULP_HAS_SKIA` (no negation) counts the same as case #2 in its
// then-branch.
bool stack_makes_gpu_surface_complete(const std::vector<GateFrame>& stack) {
    for (const auto& frame : stack) {
        switch (frame.kind) {
            case GateFrame::Kind::HasGpuSurface:
                if (!frame.in_else_branch) {
                    return true;
                }
                break;
            case GateFrame::Kind::HasSkia:
                if (!frame.in_else_branch) {
                    return true;
                }
                break;
            case GateFrame::Kind::NotHasSkia:
                if (frame.in_else_branch) {
                    return true;
                }
                break;
            case GateFrame::Kind::Other:
                break;
        }
    }
    return false;
}

std::string trim_leading(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
        ++i;
    }
    return s.substr(i);
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() &&
           s.compare(0, prefix.size(), prefix) == 0;
}

} // namespace

TEST_CASE("widget_bridge.cpp gates all gpu_surface_ method calls behind "
          "PULP_WIDGET_BRIDGE_HAS_GPU_SURFACE or PULP_HAS_SKIA "
          "[issue-3157][widget-bridge][no-gpu][ios]",
          "[widget-bridge][no-gpu]") {
    namespace fs = std::filesystem;

    const fs::path source_root = PULP_SOURCE_DIR;
    const fs::path widget_bridge_cpp =
        source_root / "core" / "view" / "src" / "widget_bridge.cpp";

    REQUIRE(fs::exists(widget_bridge_cpp));

    std::ifstream in(widget_bridge_cpp);
    REQUIRE(in.is_open());

    std::vector<GateFrame> stack;
    std::vector<std::string> ungated_lines;

    std::string raw;
    int lineno = 0;
    while (std::getline(in, raw)) {
        ++lineno;
        std::string line = trim_leading(raw);

        // Track preprocessor state (best-effort; mirrors the gates the
        // file actually uses).
        if (starts_with(line, "#if ")) {
            GateFrame f;
            if (line.find("PULP_WIDGET_BRIDGE_HAS_GPU_SURFACE") !=
                std::string::npos) {
                f.kind = GateFrame::Kind::HasGpuSurface;
            } else {
                f.kind = GateFrame::Kind::Other;
            }
            stack.push_back(f);
        } else if (starts_with(line, "#ifdef ")) {
            GateFrame f;
            if (line.find("PULP_HAS_SKIA") != std::string::npos) {
                f.kind = GateFrame::Kind::HasSkia;
            } else if (line.find("PULP_WIDGET_BRIDGE_HAS_GPU_SURFACE") !=
                       std::string::npos) {
                f.kind = GateFrame::Kind::HasGpuSurface;
            } else {
                f.kind = GateFrame::Kind::Other;
            }
            stack.push_back(f);
        } else if (starts_with(line, "#ifndef ")) {
            GateFrame f;
            if (line.find("PULP_HAS_SKIA") != std::string::npos) {
                f.kind = GateFrame::Kind::NotHasSkia;
            } else {
                f.kind = GateFrame::Kind::Other;
            }
            stack.push_back(f);
        } else if (starts_with(line, "#else") ||
                   starts_with(line, "#elif")) {
            if (!stack.empty()) {
                stack.back().in_else_branch = !stack.back().in_else_branch;
            }
        } else if (starts_with(line, "#endif")) {
            if (!stack.empty()) {
                stack.pop_back();
            }
        } else {
            // Look for member-access expressions on gpu_surface_. Method
            // calls (->method()), member arrows in conditionals — anything
            // that requires the type to be complete. Plain comparison
            // against nullptr does NOT require completeness and is fine.
            const auto arrow_pos = line.find("gpu_surface_->");
            if (arrow_pos != std::string::npos) {
                // Skip lines that are comments (// or *).
                bool is_comment = false;
                for (size_t i = 0; i < arrow_pos; ++i) {
                    if (line[i] == '/' && i + 1 < arrow_pos &&
                        line[i + 1] == '/') {
                        is_comment = true;
                        break;
                    }
                    if (line[i] == '*') {
                        // Heuristic: continuation lines of a block comment
                        // start with " *" after trim. Good enough for this
                        // file's house style.
                        if (arrow_pos > 0 && line[0] == '*') {
                            is_comment = true;
                        }
                    }
                }
                if (is_comment) {
                    continue;
                }
                if (!stack_makes_gpu_surface_complete(stack)) {
                    std::ostringstream msg;
                    msg << "widget_bridge.cpp:" << lineno
                        << " — gpu_surface_ dereference outside "
                           "PULP_WIDGET_BRIDGE_HAS_GPU_SURFACE / "
                           "PULP_HAS_SKIA gate: "
                        << raw;
                    ungated_lines.push_back(msg.str());
                }
            }
        }
    }

    if (!ungated_lines.empty()) {
        std::ostringstream summary;
        summary << "Found " << ungated_lines.size()
                << " ungated gpu_surface_ dereference(s). "
                   "These break PULP_ENABLE_GPU=OFF builds (iOS Simulator, "
                   "sanitizer matrix). Gate them behind "
                   "#if PULP_WIDGET_BRIDGE_HAS_GPU_SURFACE or move them into "
                   "the #else branch of the surrounding #ifndef PULP_HAS_SKIA "
                   "block. Offenders:\n";
        for (const auto& l : ungated_lines) {
            summary << "  " << l << "\n";
        }
        FAIL(summary.str());
    }

    SUCCEED("All gpu_surface_ member accesses are properly gated.");
}
