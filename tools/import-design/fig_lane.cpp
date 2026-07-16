#include "fig_lane.hpp"

#include <pulp/platform/child_process.hpp>

#include <choc/text/choc_JSON.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
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
    // Forward the decoder's stderr whenever it has any — not only on failure.
    // It is a diagnostic channel (ambiguous-frame candidates, asset warnings),
    // and swallowing it on a zero exit hid, e.g., an ambiguous-name pick.
    if (!result.stderr_output.empty()) {
        std::cerr << result.stderr_output;
    }
    const bool truncated = stdout_capture && result.stdout_output.size() >= kMaxDecodeStdout;
    return {result.exit_code, truncated};
}

// A per-run, hard-to-predict scratch directory. The monotonic tick keeps two
// concurrent imports of the same file from colliding and stops a local attacker
// from pre-planting a symlink at a guessable path that node's write would follow.
// Merge N per-frame envelopes into one whose root carries frames 1..N-1 as
// `alternate_frames`. Frame 0's envelope supplies every top-level field
// (provenance, tokens, library manifest); the rest contribute only their root
// node plus their assets.
//
// Merging asset manifests is collision-safe by construction: the decoder keys
// each asset by its content hash (asset_id == hash, local_path == assets/<hash>),
// so an id that repeats across frames names byte-identical content and dedupes,
// while distinct content can never share an id. That is what lets every frame's
// assets flatten into one `assets/` directory next to the merged envelope
// without rewriting a single local_path.
std::optional<int> merge_frame_envelopes(const std::vector<fs::path>& envelopes,
                                         const fs::path& scratch,
                                         const fs::path& out_path) {
    auto read_json = [](const fs::path& p, choc::value::Value& v) -> bool {
        std::ifstream in(p, std::ios::binary);
        if (!in) return false;
        std::ostringstream buf;
        buf << in.rdbuf();
        try {
            v = choc::json::parse(buf.str());
        } catch (const std::exception&) {
            return false;
        }
        return v.isObject();
    };

    choc::value::Value merged;
    if (!read_json(envelopes.front(), merged)) {
        std::cerr << "Error: could not read decoded envelope " << envelopes.front() << "\n";
        return 3;
    }

    auto alternates = choc::value::createEmptyArray();
    for (std::size_t i = 1; i < envelopes.size(); ++i) {
        choc::value::Value env;
        if (!read_json(envelopes[i], env)) {
            std::cerr << "Error: could not read decoded envelope " << envelopes[i] << "\n";
            return 3;
        }
        if (!env.hasObjectMember("root")) {
            std::cerr << "Error: decoded envelope " << envelopes[i] << " has no root frame\n";
            return 3;
        }
        alternates.addArrayElement(env["root"]);

        // Union `src` into `dst`, skipping entries whose `id_key` is already
        // present. An empty id_key keeps every entry (unkeyed observations).
        // Returns an owning array — choc's operator[] hands back a non-owning
        // ValueView, so the result has to be built up and set back by value.
        auto union_by = [](const choc::value::ValueView& dst,
                           const choc::value::ValueView& src,
                           const char* id_key) {
            auto out = choc::value::createEmptyArray();
            std::unordered_set<std::string> seen;
            auto take = [&](const choc::value::ValueView& arr) {
                if (!arr.isArray()) return;
                for (uint32_t j = 0; j < arr.size(); ++j) {
                    const auto e = arr[static_cast<int>(j)];
                    if (id_key && e.isObject() && e.hasObjectMember(id_key) &&
                        !seen.insert(e[id_key].toString()).second)
                        continue;   // same content hash → already merged
                    out.addArrayElement(e);
                }
            };
            take(dst);
            take(src);
            return out;
        };

        // asset_manifest is an object wrapping the assets array.
        // A member that isn't there reads as an empty array, so a frame that
        // carries a section the others lack still merges.
        const auto empty = choc::value::createEmptyArray();
        auto array_member = [&empty](const choc::value::ValueView& obj, const char* key) {
            return (obj.isObject() && obj.hasObjectMember(key) && obj[key].isArray())
                       ? obj[key] : empty.getView();
        };

        if (env.hasObjectMember("asset_manifest") && env["asset_manifest"].isObject() &&
            merged.hasObjectMember("asset_manifest") && merged["asset_manifest"].isObject()) {
            choc::value::Value manifest(merged["asset_manifest"]);
            manifest.setMember("assets",
                               union_by(array_member(manifest, "assets"),
                                        array_member(env["asset_manifest"], "assets"),
                                        "asset_id"));
            merged.setMember("asset_manifest", manifest);
        }
        if (env.hasObjectMember("font_family_assets"))
            merged.setMember("font_family_assets",
                             union_by(array_member(merged, "font_family_assets"),
                                      array_member(env, "font_family_assets"), "asset_id"));
        // Diagnostics are observations, not keyed records — keep every one.
        if (env.hasObjectMember("diagnostics"))
            merged.setMember("diagnostics",
                             union_by(array_member(merged, "diagnostics"),
                                      array_member(env, "diagnostics"), nullptr));
    }

    choc::value::Value root(merged["root"]);
    root.setMember("alternate_frames", alternates);
    merged.setMember("root", root);

    // Flatten every frame's assets next to the merged envelope so each entry's
    // `assets/<hash>` local_path resolves from the merged file's directory.
    std::error_code ec;
    for (const auto& env_path : envelopes) {
        const fs::path src_assets = env_path.parent_path() / "assets";
        if (!fs::exists(src_assets, ec)) continue;
        fs::create_directories(scratch / "assets", ec);
        for (const auto& entry : fs::directory_iterator(src_assets, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            fs::copy_file(entry.path(), scratch / "assets" / entry.path().filename(),
                          fs::copy_options::skip_existing, ec);
            if (ec) {
                std::cerr << "Error: could not stage asset " << entry.path() << ": "
                          << ec.message() << "\n";
                return 3;
            }
        }
    }

    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        std::cerr << "Error: could not write merged envelope " << out_path << "\n";
        return 3;
    }
    out << choc::json::toString(merged, true);
    if (!out) {
        std::cerr << "Error: could not write merged envelope " << out_path << "\n";
        return 3;
    }
    return std::nullopt;
}

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

    if (args.frame_names.empty()) {
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

    auto decode_frame = [&](const std::string& frame, const fs::path& out) -> std::optional<int> {
        std::vector<std::string> decode_args{"emit", args.input_file, "--frame",
                                             frame, "--out", out.string()};
        if (!args.page_name.empty()) {
            decode_args.push_back("--page");
            decode_args.push_back(args.page_name);
        }
        const auto r = run_decode(decode_args, nullptr);
        if (r.exit_code != 0) {
            std::cerr << "Error: .fig decode failed for frame '" << frame << "'\n";
            return r.exit_code == 2 ? 2 : 3;
        }
        return std::nullopt;
    };

    // Single-state capture: decode straight into the scratch root, exactly as a
    // pre-multi-state import did (no merge step on the common path).
    if (args.frame_names.size() == 1) {
        if (auto err = decode_frame(args.frame_names.front(), scratch)) return *err;
        args.input_file = (scratch / "scene.pulp.json").string();
        args.source_str = "figma-plugin";
        return std::nullopt;
    }

    // Multi-state capture: the decoder emits one envelope per frame, so decode
    // each into its own subdirectory and merge them into a single envelope whose
    // root carries the rest as alternate_frames. Frame INDEX is --frame order.
    std::vector<fs::path> envelopes;
    for (std::size_t i = 0; i < args.frame_names.size(); ++i) {
        const fs::path sub = scratch / ("f" + std::to_string(i));
        fs::create_directories(sub, ec);
        if (ec) {
            std::cerr << "Error: could not create scratch dir " << sub << ": "
                      << ec.message() << "\n";
            return 1;
        }
        if (auto err = decode_frame(args.frame_names[i], sub)) return *err;
        envelopes.push_back(sub / "scene.pulp.json");
    }

    const fs::path merged = scratch / "scene.pulp.json";
    if (auto err = merge_frame_envelopes(envelopes, scratch, merged)) return *err;

    args.input_file = merged.string();
    args.source_str = "figma-plugin";
    return std::nullopt;  // continue down the figma-plugin path
}

}  // namespace pulp::import_design::fig
