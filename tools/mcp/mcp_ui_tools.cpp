// mcp_ui_tools.cpp -- UI capture, view tree and simulated click over the
// Vellum app host.
//
// The three tools run a Vellum authoring bundle headlessly through
// `vellum-app-host` (offscreen render, tree dump, a press on a node) and
// project the host's own artifacts. Nothing here links a UI library: the host
// is a separate process resolved at call time, so the tools keep working when
// Pulp ships without its own view layer.

#include "mcp_tools.hpp"

#include "mcp_json.hpp"
#include "mcp_tools_internal.hpp"

#include <choc/text/choc_JSON.h>
#include <pulp/platform/child_process.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace pulp_mcp {
namespace {

namespace fs = std::filesystem;

constexpr const char* kHostEnv = "PULP_VELLUM_APP_HOST";
constexpr int kHostTimeoutMs = 90 * 1000;
constexpr std::size_t kMaxHitCandidates = 8;

// A small panel with a pressable button, used when a call names no bundle. It
// carries a gradient so the host's blank-frame floor accepts the capture, and
// has no text so the render does not depend on installed fonts.
constexpr std::string_view kDemoBundle = R"JS((() => {
  let count = 0; let dirty = true;
  const tree = () => ({ type: "stack", id: "screen",
    style: { width: 240, height: 120, backgroundColor: "#0F172A", padding: 10 },
    children: [
      { type: "view", id: "bar", style: { width: 40 + count * 40, height: 40, backgroundColor: "#14B8A6" }, children: [] },
      { type: "button", id: "go", events: { press: "inc" }, style: { width: 80, height: 30, backgroundColor: "#F59E0B" }, children: [] },
      { type: "view", id: "ramp", style: { width: 200, height: 20, backgroundLinearGradient: { angle: 90, stops: [{ position: 0.01, color: "#F8FAFC" }, { position: 1, color: "#EF4444" }] } }, children: [] } ] });
  const render = () => { dirty = false; return JSON.stringify({ protocol: "vellum.authoring-host.v1", tree: tree() }); };
  globalThis.__vellum = { protocol: "vellum.authoring-host.v1", hostProtocol: "vellum.authoring-host.v2",
    renderJSON: render, dispatchJSON() { count++; return render(); }, isDirty() { return dirty; },
    pumpJSON() { dirty = false; return JSON.stringify({ protocol: "vellum.authoring-host.v2", kind: "render-result", revision: count, tree: tree() }); } };
})();
)JS";

struct UiError {
    std::string code;
    std::string message;
    std::string detail;
};

std::string error_result(const UiError& error) {
    auto evidence = "{\"ok\":false,\"error\":{\"code\":" + json_string(error.code) +
                    ",\"message\":" + json_string(error.message);
    if (!error.detail.empty())
        evidence += ",\"detail\":" + json_string(error.detail);
    evidence += "}}";
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(evidence) +
           "}],\"isError\":true}";
}

std::string base64(const std::vector<std::uint8_t>& bytes) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t chunk = (std::uint32_t(bytes[i]) << 16) |
                                    (i + 1 < bytes.size() ? std::uint32_t(bytes[i + 1]) << 8 : 0) |
                                    (i + 2 < bytes.size() ? std::uint32_t(bytes[i + 2]) : 0);
        out += alphabet[(chunk >> 18) & 63];
        out += alphabet[(chunk >> 12) & 63];
        out += i + 1 < bytes.size() ? alphabet[(chunk >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? alphabet[chunk & 63] : '=';
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {});
}

// Removes the call's scratch directory on every exit.
class Scratch {
  public:
    Scratch() {
        std::error_code ec;
        const auto base = fs::temp_directory_path(ec);
        std::random_device random;
        for (int attempt = 0; attempt < 16 && !ec; ++attempt) {
            const auto candidate = base / ("pulp-ui-capture-" + std::to_string(random()));
            if (fs::create_directory(candidate, ec)) {
                path_ = fs::canonical(candidate, ec);
                return;
            }
        }
    }
    ~Scratch() {
        std::error_code ec;
        if (!path_.empty())
            fs::remove_all(path_, ec);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    const fs::path& path() const {
        return path_;
    }

  private:
    fs::path path_;
};

// The host named by the environment, else the one staged beside an installed
// SDK (`<prefix>/share/pulp/vellum-sdk/<version>/sdk/bin`). Never PATH or cwd.
std::optional<fs::path> resolve_host() {
    if (const char* named = std::getenv(kHostEnv); named != nullptr && *named != '\0') {
        std::error_code ec;
        const fs::path path(named);
        if (fs::is_regular_file(path, ec))
            return path;
        return std::nullopt;
    }
    const auto executable = current_process_executable_path();
    if (executable.empty())
        return std::nullopt;
    std::error_code ec;
    const auto staged = executable.parent_path().parent_path() / "share" / "pulp" / "vellum-sdk";
    if (!fs::is_directory(staged, ec))
        return std::nullopt;
    std::optional<fs::path> best;
    for (const auto& version : fs::directory_iterator(staged, ec)) {
        const auto candidate = version.path() / "sdk" / "bin" / "vellum-app-host";
        if (fs::is_regular_file(candidate, ec) && (!best || candidate > *best))
            best = candidate;
    }
    return best;
}

struct Request {
    fs::path bundle;         ///< Absolute path to the bundle to run.
    std::string scale = "1"; ///< Host capture scale: "1" or "2".
};

struct HostRun {
    std::vector<std::uint8_t> png;
    std::string tree_json;
};

struct Node {
    std::string id;
    std::string type;
    double x = 0, y = 0, width = 0, height = 0;
};

// Reads one JSON number-like field as a double, accepting integer or float.
std::optional<double> number_member(const choc::value::ValueView& object, const char* name) {
    if (!object.isObject() || !object.hasObjectMember(name))
        return std::nullopt;
    const auto member = object[name];
    if (member.isFloat64() || member.isFloat32())
        return member.getFloat64();
    if (member.isInt32() || member.isInt64())
        return static_cast<double>(member.getInt64());
    return std::nullopt;
}

std::optional<std::vector<Node>> parse_nodes(const std::string& tree_json) {
    try {
        const auto tree = choc::json::parse(tree_json);
        if (!tree.isObject() || !tree.hasObjectMember("nodes") || !tree["nodes"].isArray())
            return std::nullopt;
        std::vector<Node> nodes;
        const auto list = tree["nodes"];
        for (uint32_t i = 0; i < list.size(); ++i) {
            const auto item = list[i];
            if (!item.isObject() || !item.hasObjectMember("id") || !item["id"].isString() ||
                !item.hasObjectMember("box"))
                return std::nullopt;
            const auto box = item["box"];
            const auto x = number_member(box, "x");
            const auto y = number_member(box, "y");
            const auto width = number_member(box, "width");
            const auto height = number_member(box, "height");
            if (!x || !y || !width || !height)
                return std::nullopt;
            Node node;
            node.id = std::string(item["id"].getString());
            if (item.hasObjectMember("type") && item["type"].isString())
                node.type = std::string(item["type"].getString());
            node.x = *x;
            node.y = *y;
            node.width = *width;
            node.height = *height;
            nodes.push_back(std::move(node));
        }
        return nodes;
    } catch (...) {
        return std::nullopt;
    }
}

// Runs the host once. `press` is zero or one node id; `want_png` adds a
// capture, and the tree dump is always written.
std::optional<UiError> run_host(const fs::path& host, const Request& request,
                                const std::string& press, bool want_png, HostRun& out) {
    Scratch scratch;
    if (scratch.path().empty())
        return UiError{"scratch-unavailable", "could not create a scratch directory", {}};
    const auto png_path = scratch.path() / "frame.png";
    const auto tree_path = scratch.path() / "tree.json";

    std::vector<std::string> arguments{"--bundle", request.bundle.string(), "--no-window"};
    if (!press.empty()) {
        arguments.emplace_back("--press");
        arguments.push_back(press);
    }
    arguments.emplace_back("--dump-tree");
    arguments.push_back(tree_path.string());
    if (want_png) {
        arguments.emplace_back("--capture-scale");
        arguments.push_back(request.scale);
        arguments.emplace_back("--capture");
        arguments.push_back(png_path.string());
    }

    pulp::platform::ProcessOptions options;
    options.timeout_ms = kHostTimeoutMs;
    options.max_output_bytes = 1 << 16;
    const auto process = pulp::platform::ChildProcess::run(host.string(), arguments, options);
    if (process.timed_out)
        return UiError{"host-timeout", "the Vellum app host exceeded its time limit",
                       process.stderr_output};
    if (process.exit_code != 0)
        return UiError{process.stderr_output.find("not pressable") != std::string::npos
                           ? "not-pressable"
                           : "host-failed",
                       "the Vellum app host refused the request", process.stderr_output};

    const auto tree = read_bytes(tree_path);
    if (!tree || tree->empty())
        return UiError{"host-no-tree", "the host reported success but wrote no tree dump",
                       process.stderr_output};
    out.tree_json.assign(tree->begin(), tree->end());
    if (want_png) {
        auto png = read_bytes(png_path);
        if (!png || png->empty())
            return UiError{"host-no-capture", "the host reported success but wrote no frame",
                           process.stderr_output};
        out.png = std::move(*png);
    }
    return std::nullopt;
}

// Parses the arguments shared by the three tools. `allowed` lists the
// tool-specific names; `bundle` and `scale` are always accepted.
std::optional<UiError> parse_request(const std::string& params_json,
                                     std::initializer_list<std::string_view> allowed,
                                     choc::value::Value& params, Request& request,
                                     Scratch& demo_scratch) {
    try {
        params = choc::json::parse(params_json.empty() ? "{}" : params_json);
    } catch (...) {
        return UiError{"invalid-arguments", "arguments must be valid JSON", {}};
    }
    if (!params.isObject())
        return UiError{"invalid-arguments", "arguments must be an object", {}};
    std::string unknown;
    params.getView().visitObjectMembers([&](std::string_view name, const choc::value::ValueView&) {
        const bool known = name == "bundle" || name == "scale" ||
                           std::find(allowed.begin(), allowed.end(), name) != allowed.end();
        if (!known && unknown.empty())
            unknown = std::string(name);
    });
    if (!unknown.empty())
        return UiError{
            "invalid-arguments", "unsupported argument for the Vellum host: " + unknown, {}};

    if (params.hasObjectMember("scale")) {
        const auto scale = number_member(params.getView(), "scale");
        if (!scale || (*scale != 1.0 && *scale != 2.0))
            return UiError{"invalid-arguments", "scale must be 1 or 2", {}};
        request.scale = *scale == 2.0 ? "2" : "1";
    }
    if (params.hasObjectMember("bundle")) {
        if (!params["bundle"].isString() || params["bundle"].getString().empty())
            return UiError{"invalid-arguments", "bundle must be a non-empty path", {}};
        std::error_code ec;
        const fs::path given{std::string(params["bundle"].getString())};
        const auto absolute = fs::weakly_canonical(fs::absolute(given, ec), ec);
        if (ec || !fs::is_regular_file(absolute, ec))
            return UiError{"bundle-not-found", "bundle file does not exist", given.string()};
        request.bundle = absolute;
        return std::nullopt;
    }
    if (demo_scratch.path().empty())
        return UiError{"scratch-unavailable", "could not create a scratch directory", {}};
    request.bundle = demo_scratch.path() / "demo.js";
    std::ofstream(request.bundle, std::ios::binary) << kDemoBundle;
    return std::nullopt;
}

std::optional<UiError> require_host(fs::path& host) {
    const auto resolved = resolve_host();
    if (!resolved)
        return UiError{"host-unavailable",
                       std::string("no Vellum app host found; set ") + kHostEnv +
                           " or install the Pulp SDK with its staged Vellum SDK",
                       {}};
    host = *resolved;
    return std::nullopt;
}

std::string image_block(const std::vector<std::uint8_t>& png) {
    return "{\"type\":\"image\",\"data\":\"" + base64(png) + "\",\"mimeType\":\"image/png\"}";
}

std::string text_block(const std::string& text) {
    return "{\"type\":\"text\",\"text\":" + json_string(text) + "}";
}

} // namespace

bool ui_request_uses_vellum_host(const std::string& params_json) {
    return !extract_raw(params_json, "bundle").empty();
}

std::string handle_ui_screenshot(const std::string& params_json) {
    choc::value::Value params;
    Request request;
    Scratch demo;
    if (auto error = parse_request(params_json, {}, params, request, demo))
        return error_result(*error);
    fs::path host;
    if (auto error = require_host(host))
        return error_result(*error);
    HostRun run;
    if (auto error = run_host(host, request, {}, true, run))
        return error_result(*error);
    return "{\"content\":[" + image_block(run.png) + "]}";
}

std::string handle_ui_view_tree(const std::string& params_json) {
    choc::value::Value params;
    Request request;
    Scratch demo;
    if (auto error = parse_request(params_json, {}, params, request, demo))
        return error_result(*error);
    fs::path host;
    if (auto error = require_host(host))
        return error_result(*error);
    HostRun run;
    if (auto error = run_host(host, request, {}, false, run))
        return error_result(*error);
    return "{\"content\":[" + text_block(run.tree_json) +
           "],\"structuredContent\":" + run.tree_json + "}";
}

std::string handle_ui_simulate_click(const std::string& params_json) {
    choc::value::Value params;
    Request request;
    Scratch demo;
    if (auto error =
            parse_request(params_json, {"node_id", "x", "y", "capture"}, params, request, demo))
        return error_result(*error);
    fs::path host;
    if (auto error = require_host(host))
        return error_result(*error);

    const bool has_node = params.hasObjectMember("node_id");
    const bool has_point = params.hasObjectMember("x") || params.hasObjectMember("y");
    if (has_node == has_point)
        return error_result({"invalid-arguments", "pass either node_id or both x and y", {}});
    bool want_png = true;
    if (params.hasObjectMember("capture")) {
        if (!params["capture"].isBool())
            return error_result({"invalid-arguments", "capture must be a boolean", {}});
        want_png = params["capture"].getBool();
    }

    std::vector<std::string> candidates;
    if (has_node) {
        if (!params["node_id"].isString() || params["node_id"].getString().empty())
            return error_result({"invalid-arguments", "node_id must be a non-empty string", {}});
        candidates.emplace_back(std::string(params["node_id"].getString()));
    } else {
        const auto x = number_member(params.getView(), "x");
        const auto y = number_member(params.getView(), "y");
        if (!x || !y || !std::isfinite(*x) || !std::isfinite(*y))
            return error_result({"invalid-arguments", "x and y must be finite numbers", {}});
        HostRun before;
        if (auto error = run_host(host, request, {}, false, before))
            return error_result(*error);
        const auto nodes = parse_nodes(before.tree_json);
        if (!nodes)
            return error_result(
                {"malformed-host-output", "the tree dump was not understood", before.tree_json});
        // Later nodes paint on top of earlier ones, so try the deepest hit
        // first and fall back to the boxes beneath it.
        for (auto it = nodes->rbegin(); it != nodes->rend(); ++it) {
            if (*x >= it->x && *x < it->x + it->width && *y >= it->y && *y < it->y + it->height &&
                candidates.size() < kMaxHitCandidates)
                candidates.push_back(it->id);
        }
        if (candidates.empty())
            return error_result({"no-target", "no node contains the point", {}});
    }

    std::optional<UiError> last;
    for (const auto& candidate : candidates) {
        HostRun run;
        auto error = run_host(host, request, candidate, want_png, run);
        if (!error) {
            const auto structured =
                "{\"clicked\":" + json_string(candidate) + ",\"tree\":" + run.tree_json + "}";
            std::string content = text_block(structured);
            if (want_png)
                content += "," + image_block(run.png);
            return "{\"content\":[" + content + "],\"structuredContent\":" + structured + "}";
        }
        last = *error;
        if (error->code != "not-pressable")
            break;
    }
    if (last && last->code == "not-pressable" && has_point)
        return error_result({"no-target", "no pressable node contains the point", last->detail});
    return error_result(*last);
}

} // namespace pulp_mcp
