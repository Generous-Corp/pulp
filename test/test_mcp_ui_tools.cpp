// test_mcp_ui_tools.cpp -- pulp_screenshot (bundle form), pulp_get_view_tree and
// pulp_simulate_click over the Vellum app host.
//
// A scripted stand-in for the host pins the argument handling, hit testing and
// failure surfacing on every POSIX machine. One case drives a real host when
// PULP_TEST_VELLUM_APP_HOST names one, and otherwise skips with a reason.

#include <catch2/catch_test_macros.hpp>

#include <choc/text/choc_JSON.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "mcp_server_test_support.hpp"

#include "mcp_json.hpp"
#include "mcp_server.hpp"
#include "mcp_tools.hpp"

using namespace mcp_test;
using namespace pulp_mcp;
using namespace pulp_mcp::server;

namespace {

namespace fs = std::filesystem;

// Environment variable set for one scope, restored on exit.
class ScopedHostEnv {
  public:
    explicit ScopedHostEnv(const std::string& value) {
        if (const char* previous = std::getenv("PULP_VELLUM_APP_HOST")) {
            had_previous_ = true;
            previous_ = previous;
        }
        set(value);
    }
    ~ScopedHostEnv() {
        if (had_previous_)
            set(previous_);
        else
            unset();
    }
    ScopedHostEnv(const ScopedHostEnv&) = delete;
    ScopedHostEnv& operator=(const ScopedHostEnv&) = delete;

  private:
    static void set(const std::string& value) {
#if defined(_WIN32)
        _putenv_s("PULP_VELLUM_APP_HOST", value.c_str());
#else
        setenv("PULP_VELLUM_APP_HOST", value.c_str(), 1);
#endif
    }
    static void unset() {
#if defined(_WIN32)
        _putenv_s("PULP_VELLUM_APP_HOST", "");
#else
        unsetenv("PULP_VELLUM_APP_HOST");
#endif
    }
    bool had_previous_ = false;
    std::string previous_;
};

// One-pixel PNG, base64. The stand-in host writes these bytes for --capture.
constexpr const char* kPixelPngBase64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5E"
    "rkJggg==";

// Mirrors the host's contract: --press NODE succeeds only for `go`, which
// widens `bar`; any other node is refused with the host's own message. The
// invocation is appended to <script>.log so a test can assert the arguments.
std::string host_script() {
    return std::string(R"SH(#!/bin/sh
echo "$@" >> "$0.log"
press=""; tree=""; cap=""
while [ $# -gt 0 ]; do
  case "$1" in
    --press) press="$2"; shift 2;;
    --dump-tree) tree="$2"; shift 2;;
    --capture) cap="$2"; shift 2;;
    --bundle|--capture-scale) shift 2;;
    *) shift;;
  esac
done
if [ -n "$FAKE_HOST_FLOOR" ]; then
  echo "rendered frame failed content floor: colors=1 stddev=0 content=0" >&2
  exit 1
fi
width=40
if [ -n "$press" ]; then
  if [ "$press" = "go" ]; then width=80; else
    echo "scenario press target is missing or not pressable: $press" >&2
    exit 1
  fi
fi
if [ -n "$tree" ]; then
  printf '{"protocol":"vellum.tree-dump.v1","width":240,"height":120,"nodes":[{"id":"screen","type":"stack","box":{"x":0,"y":0,"width":240,"height":120}},{"id":"bar","type":"view","box":{"x":10,"y":10,"width":%s,"height":40}},{"id":"go","type":"button","box":{"x":10,"y":50,"width":80,"height":30}}]}' "$width" > "$tree"
fi
if [ -n "$cap" ]; then
  printf '%s' ')SH") +
           kPixelPngBase64 + R"SH(' | base64 -d > "$cap"
fi
)SH";
}

struct FakeHost {
    TempDir dir;
    fs::path script;
    fs::path bundle;

    FakeHost() {
        script = dir.path / "fake-host.sh";
        std::ofstream(script) << host_script();
        fs::permissions(script, fs::perms::owner_all, fs::perm_options::add);
        bundle = dir.path / "panel.js";
        std::ofstream(bundle) << "// bundle\n";
    }

    std::string log() const {
        std::ifstream in(script.string() + ".log");
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }
};

std::string call(const std::string& tool, const std::string& arguments) {
    return handle_request(tool_call("1", tool, arguments));
}

std::string quoted(const fs::path& path) {
    return json_string(path.string());
}

// The text of the first content block of a tools/call response.
std::string first_text(const std::string& response) {
    const auto parsed = choc::json::parse(response);
    return std::string(parsed["result"]["content"][0]["text"].getString());
}

bool is_error(const std::string& response) {
    const auto parsed = choc::json::parse(response);
    return parsed["result"].hasObjectMember("isError") && parsed["result"]["isError"].getBool();
}

double bar_width(const choc::value::ValueView& tree) {
    const auto nodes = tree["nodes"];
    for (uint32_t i = 0; i < nodes.size(); ++i)
        if (nodes[i]["id"].getString() == "bar")
            return nodes[i]["box"]["width"].getWithDefault<double>(-1.0);
    return -1.0;
}

} // namespace

#if !defined(_WIN32)

TEST_CASE("pulp_screenshot with a bundle returns the host's frame", "[mcp][tools][ui]") {
    FakeHost host;
    ScopedHostEnv env(host.script.string());

    const auto response = call("pulp_screenshot", "{\"bundle\":" + quoted(host.bundle) + "}");
    REQUIRE_FALSE(is_error(response));
    const auto parsed = choc::json::parse(response);
    const auto block = parsed["result"]["content"][0];
    CHECK(block["type"].getString() == "image");
    CHECK(block["mimeType"].getString() == "image/png");
    CHECK(block["data"].getString() == kPixelPngBase64);

    // The host is handed an absolute bundle path and the default scale.
    const auto log = host.log();
    CHECK(log.find("--bundle " + fs::canonical(host.bundle).string()) != std::string::npos);
    CHECK(log.find("--capture-scale 1") != std::string::npos);
    CHECK(log.find("--no-window") != std::string::npos);
}

TEST_CASE("pulp_screenshot passes scale 2 and refuses other scales", "[mcp][tools][ui]") {
    FakeHost host;
    ScopedHostEnv env(host.script.string());
    const auto bundle = "\"bundle\":" + quoted(host.bundle);

    REQUIRE_FALSE(is_error(call("pulp_screenshot", "{" + bundle + ",\"scale\":2}")));
    CHECK(host.log().find("--capture-scale 2") != std::string::npos);

    const auto refused = call("pulp_screenshot", "{" + bundle + ",\"scale\":3}");
    CHECK(is_error(refused));
    require_contains(refused, "scale must be 1 or 2");

    // The host has no theme or size control, so the request is refused rather
    // than silently rendered at a different size.
    for (const char* extra : {"\"theme\":\"light\"", "\"width\":800"}) {
        const auto response = call("pulp_screenshot", "{" + bundle + "," + extra + "}");
        CHECK(is_error(response));
        require_contains(response, "unsupported argument for the Vellum host");
    }
}

TEST_CASE("pulp_screenshot surfaces a host refusal, never an empty image", "[mcp][tools][ui]") {
    FakeHost host;
    ScopedHostEnv env(host.script.string());
    setenv("FAKE_HOST_FLOOR", "1", 1);
    const auto response = call("pulp_screenshot", "{\"bundle\":" + quoted(host.bundle) + "}");
    unsetenv("FAKE_HOST_FLOOR");
    CHECK(is_error(response));
    require_contains(response, "host-failed");
    require_contains(response, "content floor");
    CHECK(response.find("image/png") == std::string::npos);
}

TEST_CASE("UI tools report a missing host and a missing bundle", "[mcp][tools][ui]") {
    FakeHost host;
    {
        ScopedHostEnv env((host.dir.path / "absent-host").string());
        const auto response =
            call("pulp_get_view_tree", "{\"bundle\":" + quoted(host.bundle) + "}");
        CHECK(is_error(response));
        require_contains(response, "host-unavailable");
    }
    ScopedHostEnv env(host.script.string());
    const auto response =
        call("pulp_get_view_tree", "{\"bundle\":" + quoted(host.dir.path / "nope.js") + "}");
    CHECK(is_error(response));
    require_contains(response, "bundle-not-found");
}

TEST_CASE("pulp_get_view_tree returns node ids and boxes", "[mcp][tools][ui]") {
    FakeHost host;
    ScopedHostEnv env(host.script.string());

    // No bundle named: the built-in demo panel is written and run.
    const auto response = call("pulp_get_view_tree", "{}");
    REQUIRE_FALSE(is_error(response));
    const auto tree = choc::json::parse(first_text(response));
    CHECK(tree["protocol"].getString() == "vellum.tree-dump.v1");
    CHECK(tree["nodes"].size() == 3);
    CHECK(bar_width(tree.getView()) == 40.0);
    CHECK(host.log().find("demo.js") != std::string::npos);
}

TEST_CASE("pulp_simulate_click presses a node and returns the new tree and frame",
          "[mcp][tools][ui]") {
    FakeHost host;
    ScopedHostEnv env(host.script.string());
    const auto bundle = "\"bundle\":" + quoted(host.bundle);

    const auto response = call("pulp_simulate_click", "{" + bundle + ",\"node_id\":\"go\"}");
    REQUIRE_FALSE(is_error(response));
    const auto parsed = choc::json::parse(response);
    const auto structured = parsed["result"]["structuredContent"];
    CHECK(structured["clicked"].getString() == "go");
    CHECK(bar_width(structured["tree"]) == 80.0);
    CHECK(parsed["result"]["content"][1]["type"].getString() == "image");
    CHECK(parsed["result"]["content"][1]["data"].getString() == kPixelPngBase64);

    const auto without_frame =
        call("pulp_simulate_click", "{" + bundle + ",\"node_id\":\"go\",\"capture\":false}");
    CHECK(choc::json::parse(without_frame)["result"]["content"].size() == 1);
}

TEST_CASE("pulp_simulate_click at a point presses the deepest pressable node", "[mcp][tools][ui]") {
    FakeHost host;
    ScopedHostEnv env(host.script.string());
    const auto bundle = "\"bundle\":" + quoted(host.bundle);

    // (20,60) is inside screen and go; go is on top and pressable.
    auto response = call("pulp_simulate_click", "{" + bundle + ",\"x\":20,\"y\":60}");
    REQUIRE_FALSE(is_error(response));
    CHECK(choc::json::parse(response)["result"]["structuredContent"]["clicked"].getString() ==
          "go");

    // (20,20) is inside screen and bar; neither is pressable, so the click has
    // no target and says so instead of reporting a press that did not happen.
    response = call("pulp_simulate_click", "{" + bundle + ",\"x\":20,\"y\":20}");
    CHECK(is_error(response));
    require_contains(response, "no-target");

    // Outside every box.
    response = call("pulp_simulate_click", "{" + bundle + ",\"x\":500,\"y\":500}");
    CHECK(is_error(response));
    require_contains(response, "no node contains the point");
}

TEST_CASE("pulp_simulate_click rejects an absent target and ambiguous arguments",
          "[mcp][tools][ui]") {
    FakeHost host;
    ScopedHostEnv env(host.script.string());
    const auto bundle = "\"bundle\":" + quoted(host.bundle);

    auto response = call("pulp_simulate_click", "{" + bundle + ",\"node_id\":\"missing\"}");
    CHECK(is_error(response));
    require_contains(response, "not-pressable");
    require_contains(response, "missing");
    CHECK(response.find("image/png") == std::string::npos);

    response = call("pulp_simulate_click", "{" + bundle + "}");
    CHECK(is_error(response));
    require_contains(response, "pass either node_id or both x and y");

    response = call("pulp_simulate_click", "{" + bundle + ",\"node_id\":\"go\",\"x\":1,\"y\":1}");
    CHECK(is_error(response));

    response = call("pulp_simulate_click", "{" + bundle + ",\"x\":10}");
    CHECK(is_error(response));
}

#endif // !_WIN32

TEST_CASE("UI tools drive a real Vellum app host when one is named", "[mcp][tools][ui][live]") {
    const char* named = std::getenv("PULP_TEST_VELLUM_APP_HOST");
    if (named == nullptr || *named == '\0' || !fs::is_regular_file(named)) {
        SKIP("PULP_TEST_VELLUM_APP_HOST does not name a Vellum app host");
    }
    ScopedHostEnv env(named);

    const auto before = call("pulp_get_view_tree", "{}");
    REQUIRE_FALSE(is_error(before));
    const auto before_tree = choc::json::parse(first_text(before));
    CHECK(bar_width(before_tree.getView()) == 40.0);

    const auto after = call("pulp_simulate_click", "{\"node_id\":\"go\"}");
    REQUIRE_FALSE(is_error(after));
    const auto parsed = choc::json::parse(after);
    CHECK(bar_width(parsed["result"]["structuredContent"]["tree"]) == 80.0);

    // The frame decodes as a PNG of the panel's size at scale 1.
    const auto png_b64 = std::string(parsed["result"]["content"][1]["data"].getString());
    CHECK(png_b64.rfind("iVBORw0KGgo", 0) == 0);
    CHECK(png_b64.size() > 400);

    // Keep the delivered frame for a human look: the base64 text of the PNG.
    if (const char* keep = std::getenv("PULP_TEST_UI_CAPTURE_DIR")) {
        std::ofstream(fs::path(keep) / "click.png.b64") << png_b64;
        const auto shot = choc::json::parse("{\"result\":" + handle_ui_screenshot("{}") + "}");
        std::ofstream(fs::path(keep) / "screenshot.png.b64")
            << std::string(shot["result"]["content"][0]["data"].getString());
    }

    // A node the panel does not have is refused, not reported as pressed.
    const auto refused = call("pulp_simulate_click", "{\"node_id\":\"absent\"}");
    CHECK(is_error(refused));
    require_contains(refused, "not pressable");
}
