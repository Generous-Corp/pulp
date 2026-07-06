#include "fig_lane.hpp"

#include <pulp/platform/child_process.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace pulp::import_design::fig {

namespace fs = std::filesystem;

namespace {

// Cap on captured decoder stdout. An outline of even a very large file is well
// under this; the guard below treats hitting the cap as an error rather than
// silently emitting truncated JSON.
constexpr size_t kMaxDecodeStdout = 64 * 1024 * 1024;

// Locate the Node decoder (fig_decode.mjs). Trust order matters: an explicit
// env override (caller's own trust boundary), then the compiled-in source
// directory (the trusted install/dev tree), and only then an upward search from
// the working directory. Putting the cwd search last keeps a hostile checkout
// that ships its own fig_decode.mjs from being executed ahead of the real one.
fs::path resolve_decode_script() {
    if (const char* env = std::getenv("PULP_FIG_DECODE"); env && *env) {
        fs::path p(env);
        if (fs::exists(p)) return p;
    }
#ifdef PULP_IMPORT_DESIGN_SRC_DIR
    if (fs::path cand = fs::path(PULP_IMPORT_DESIGN_SRC_DIR) / "fig_decode.mjs"; fs::exists(cand)) {
        return cand;
    }
#endif
    fs::path cur = fs::current_path();
    for (int i = 0; i < 32; ++i) {
        auto cand = cur / "tools" / "import-design" / "fig_decode.mjs";
        if (fs::exists(cand)) return cand;
        if (!cur.has_parent_path() || cur.parent_path() == cur) break;
        cur = cur.parent_path();
    }
    return {};
}

// Result of a decoder run: the process exit code plus whether the captured
// stdout hit the cap (i.e. was truncated).
struct DecodeResult {
    int exit_code = 0;
    bool truncated = false;
};

DecodeResult run_decode(const std::vector<std::string>& args, std::string* stdout_capture) {
    auto node = pulp::platform::find_on_path("node");
    if (!node) {
        std::cerr << "Error: 'node' not found on PATH; the .fig lane needs Node >= 22\n";
        return {127, false};
    }
    auto script = resolve_decode_script();
    if (script.empty()) {
        std::cerr << "Error: could not locate fig_decode.mjs "
                     "(set PULP_FIG_DECODE to its path)\n";
        return {127, false};
    }
    std::vector<std::string> argv;
    argv.reserve(args.size() + 1);
    argv.push_back(script.string());
    for (const auto& a : args) argv.push_back(a);

    pulp::platform::ProcessOptions opts;
    opts.timeout_ms = 120000;              // a huge file decodes in a few seconds
    opts.max_output_bytes = kMaxDecodeStdout;
    auto result = pulp::platform::ChildProcess::run(node->string(), argv, opts);
    if (result.timed_out) {
        std::cerr << "Error: .fig decode timed out\n";
        return {124, false};
    }
    if (stdout_capture) *stdout_capture = result.stdout_output;
    if (result.exit_code != 0 && !result.stderr_output.empty()) {
        std::cerr << result.stderr_output;
    }
    const bool truncated = stdout_capture && result.stdout_output.size() >= kMaxDecodeStdout;
    return {result.exit_code, truncated};
}

// A per-run, hard-to-predict scratch directory. The monotonic tick keeps two
// concurrent imports of the same file from colliding and stops a local attacker
// from pre-planting a symlink at a guessable path that node's write would follow.
fs::path make_scratch_dir(const std::string& input_file) {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path()
           / ("pulp-fig-" + fs::path(input_file).stem().string() + "-" + std::to_string(tick));
}

}  // namespace

std::optional<int> handle(const LaneArgs& args) {
    if (args.source_str != "fig") {
        if (args.outline_mode) {
            std::cerr << "Error: --outline is only supported with --from fig\n";
            return 1;
        }
        return std::nullopt;  // not the fig lane — continue normally
    }

    if (args.input_file.empty()) {
        std::cerr << "Error: --from fig requires --file <path.fig>\n";
        return 1;
    }

    if (args.outline_mode) {
        std::vector<std::string> decode_args{"outline", args.input_file};
        if (args.outline_json) decode_args.push_back("--json");
        std::string captured;
        const auto r = run_decode(decode_args, &captured);
        if (r.truncated) {
            std::cerr << "Error: .fig outline exceeded the output cap; the file is "
                         "unexpectedly large\n";
            return 3;
        }
        std::cout << captured;
        return r.exit_code == 0 ? 0 : (r.exit_code == 2 ? 2 : 3);
    }

    if (args.frame_name.empty()) {
        std::cerr << "Error: --from fig requires --frame <guid|name>; "
                     "run with --outline to list frames\n";
        return 1;
    }

    const fs::path scratch = make_scratch_dir(args.input_file);
    std::error_code ec;
    fs::create_directories(scratch, ec);
    if (ec) {
        std::cerr << "Error: could not create scratch dir " << scratch << ": "
                  << ec.message() << "\n";
        return 1;
    }
    if (args.created_tmp_dir) *args.created_tmp_dir = scratch.string();

    std::vector<std::string> decode_args{"emit", args.input_file, "--frame",
                                         args.frame_name, "--out", scratch.string()};
    if (!args.page_name.empty()) {
        decode_args.push_back("--page");
        decode_args.push_back(args.page_name);
    }
    const auto r = run_decode(decode_args, nullptr);
    if (r.exit_code != 0) {
        std::cerr << "Error: .fig decode failed for frame '" << args.frame_name << "'\n";
        return r.exit_code == 2 ? 2 : 3;
    }

    args.input_file = (scratch / "scene.pulp.json").string();
    args.source_str = "figma-plugin";
    return std::nullopt;  // continue down the figma-plugin path
}

}  // namespace pulp::import_design::fig
