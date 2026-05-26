// pulp-mcp — MCP (Model Context Protocol) server for Pulp
// Exposes Pulp operations as tools via stdin/stdout JSON-RPC 2.0

#include <iostream>
#include <string>
#include <string_view>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cctype>

#include <pulp/tools/audio/model_store.hpp>
#include <pulp/tools/audio/excerpt_service.hpp>
#include <pulp/tools/audio/service.hpp>

#include "pulp_mcp_version.h"
#include "mcp_json.hpp"
#include "mcp_compat.hpp"
#include "mcp_shell.hpp"
#include "mcp_tools.hpp"

namespace fs = std::filesystem;

// JSON-RPC framing + field extraction extracted to mcp_json.hpp, and
// project SDK-compat resolution to mcp_compat.hpp, in the Phase 6 (B4)
// refactor. Pull the helpers into file scope so the rest of this TU
// keeps its existing unqualified call sites.
using pulp_mcp::json_string;
using pulp_mcp::json_error;
using pulp_mcp::json_result;
using pulp_mcp::json_tool_payload;
using pulp_mcp::extract_string;
using pulp_mcp::extract_raw;
using pulp_mcp::extract_int;
using pulp_mcp::extract_double;
using pulp_mcp::extract_bool;
using pulp_mcp::min_sdk_for_tool;
using pulp_mcp::resolve_project_sdk_version;
using pulp_mcp::compare_semver;
using pulp_mcp::compat_error_payload;
using pulp_mcp::handle_compat;
// Shell-execution + tool handlers extracted to mcp_shell.hpp /
// mcp_tools.{hpp,cpp} in the Phase 6 (B4) refactor.
using pulp_mcp::exec;
using pulp_mcp::find_project_root;
using pulp_mcp::shell_quote;
using pulp_mcp::handle_build;
using pulp_mcp::handle_test;
using pulp_mcp::handle_status;
using pulp_mcp::handle_validate;
using pulp_mcp::handle_audio_model_status;
using pulp_mcp::handle_audio_model_list;
using pulp_mcp::handle_audio_model_activate;
using pulp_mcp::handle_audio_read_bundle;
using pulp_mcp::handle_audio_excerpt_find;


// ── MCP Protocol Handler ─────────────────────────────────────────────────────

static std::string tools_list_json() {
    return R"JSON({"tools":[
{"name":"pulp_build","description":"Build the Pulp project (configure + compile)","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_test","description":"Run the Pulp test suite","inputSchema":{"type":"object","properties":{"filter":{"type":"string","description":"Test name filter (regex)"}}}},
{"name":"pulp_status","description":"Show Pulp project status","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_validate","description":"Run plugin format validators (CLAP, VST3/pluginval, AU, optional AAX). Use --all for vstvalidator and full AAX validation. Returns JSON report.","inputSchema":{"type":"object","properties":{"all":{"type":"boolean","description":"Run all validators including vstvalidator and full AAX validation"},"json":{"type":"boolean","description":"Return JSON report (default true via MCP)"}}}},
{"name":"pulp_audio_model_list","description":"List registered audio models and their current install state.","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_audio_model_status","description":"Show the configured audio model and whether its recorded checkpoint is loadable now.","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_audio_model_activate","description":"Activate an installed audio model by logical id.","inputSchema":{"type":"object","required":["model_id"],"properties":{"model_id":{"type":"string","description":"Registered audio model id"}}}},
{"name":"pulp_audio_excerpt_find","description":"Run WAV-first excerpt-find with the deterministic null backend and create a bundle.","inputSchema":{"type":"object","required":["text","input_path"],"properties":{"text":{"type":"string","description":"Natural language excerpt query"},"input_path":{"type":"string","description":"File or directory path to scan"},"model_id":{"type":"string","description":"Optional registered audio model id"},"recursive":{"type":"boolean","description":"Recurse into input directories"},"top":{"type":"integer","description":"Maximum ranked results to return"},"window_ms":{"type":"integer","description":"Excerpt window size in milliseconds"},"hop_ms":{"type":"integer","description":"Window hop size in milliseconds"},"min_score":{"type":"number","description":"Minimum deterministic stub score threshold"},"max_candidates_per_file":{"type":"integer","description":"Per-file candidate cap before global ranking"},"bundle_out":{"type":"string","description":"Directory to create excerpt bundles in"}}}},
{"name":"pulp_audio_read_bundle","description":"Read an excerpt-find artifact bundle and return parsed manifest/result summary.","inputSchema":{"type":"object","required":["bundle_path"],"properties":{"bundle_path":{"type":"string","description":"Path to an excerpt-find bundle directory"}}}},
{"name":"pulp_screenshot","description":"Render a plugin UI to PNG (base64). Use --demo for a built-in demo or --script for a JS file.","inputSchema":{"type":"object","properties":{"script":{"type":"string","description":"Path to JS UI script"},"width":{"type":"integer","description":"Width in points (default 400)"},"height":{"type":"integer","description":"Height in points (default 300)"},"theme":{"type":"string","description":"Theme: dark, light, pro_audio"},"demo":{"type":"boolean","description":"Render built-in demo UI"}}}},
{"name":"pulp_simulate_click","description":"Simulate a mouse click at coordinates on a demo UI and return the view tree JSON","inputSchema":{"type":"object","properties":{"x":{"type":"number","description":"X coordinate"},"y":{"type":"number","description":"Y coordinate"}}}},
{"name":"pulp_get_view_tree","description":"Get the view tree as JSON for a demo UI","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_create","description":"Scaffold a new plugin project from templates","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Plugin name"},"type":{"type":"string","enum":["effect","instrument"],"description":"Plugin type"},"manufacturer":{"type":"string","description":"Manufacturer name"}}}},
{"name":"pulp_docs_check","description":"Validate docs consistency against the codebase","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_docs_search","description":"Search local docs for a query string","inputSchema":{"type":"object","properties":{"query":{"type":"string","description":"Search query"}}}},
{"name":"pulp_inspect_dom","description":"Get the view tree of a running plugin's UI via the inspector protocol","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_inspect_params","description":"Get all parameter info and current values from a running plugin","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_inspect_screenshot","description":"Capture a screenshot from a running plugin via the inspector","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_inspect_evaluate","description":"Evaluate a JS expression in a running plugin's script engine","inputSchema":{"type":"object","properties":{"expression":{"type":"string","description":"JS expression to evaluate"}}}},
{"name":"pulp_inspect_performance","description":"Get render performance metrics from a running plugin","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_inspect_audio","description":"Get audio configuration and buffer underrun info from a running plugin","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_motion_start_trace","description":"Start a motion trace via the inspector Motion.startTrace protocol. Attaches one or more metric probes (geometry / scroll-geometry) on a target view and begins streaming Motion.sample events. Enables tracing on the Coordinator if it is currently off. Returns the assigned trace_id.","inputSchema":{"type":"object","required":["view_name","metrics"],"properties":{"view_name":{"type":"string","description":"Human-readable trace name attached to all emitted events"},"fps":{"type":"integer","description":"Target sample rate in frames per second (default 15)"},"metrics":{"type":"array","description":"Metric probes. Each item is {kind:'geometry'|'scroll-geometry', name, node_id, properties?, space?, source?}.","items":{"type":"object","required":["kind"],"properties":{"kind":{"type":"string","enum":["geometry","scroll-geometry","scrollGeometry"]},"name":{"type":"string"},"node_id":{"type":"string"},"properties":{"type":"array","items":{"type":"string"}},"space":{"type":"string"},"source":{"type":"string"}}}}}}},
{"name":"pulp_motion_stop_trace","description":"Stop a previously started motion trace via Motion.stopTrace and release its probe handle. Returns {removed:bool}.","inputSchema":{"type":"object","required":["trace_id"],"properties":{"trace_id":{"type":"integer","description":"trace_id returned by pulp_motion_start_trace"}}}},
{"name":"pulp_motion_snapshot","description":"Read a snapshot of the motion observability subsystem via Motion.snapshot. Returns tracing_enabled, firehose, active_traces, inspector_traces, emitted_events, cost_enabled, and cost_samples_emitted. Safe to call when nothing is being traced.","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_motion_list_traces","description":"List the inspector-owned motion trace_ids via Motion.listTraces. Returns {trace_ids:[...]} (empty when nothing is active).","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_motion_load_fixture","description":"Load a .motion.jsonl fixture into the MotionScrubber via Motion.loadFixture. Returns {ok, event_count, max_frame, header} on success or an error if the fixture is missing or malformed.","inputSchema":{"type":"object","required":["path"],"properties":{"path":{"type":"string","description":"Absolute path to a .motion.jsonl fixture"}}}},
{"name":"pulp_motion_scrub_to","description":"Scrub a previously loaded fixture to the given frame via Motion.scrubTo. Returns {playhead_frame, emitted_count}. Errors with 'no fixture loaded' when called before pulp_motion_load_fixture.","inputSchema":{"type":"object","required":["frame"],"properties":{"frame":{"type":"integer","description":"Target playhead frame (>= 0)"}}}},
{"name":"pulp_motion_play","description":"Play a previously loaded fixture from the current playhead via Motion.play. Returns {playing, emitted_count, playhead_frame}. Errors when no fixture is loaded.","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_motion_pause","description":"Pause fixture playback via Motion.pause. Returns {playing:false, playhead_frame}. Always safe — no-op when not playing.","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_motion_enable_cost","description":"Enable the cost-attribution channel via Motion.enableCost. Motion.cost events begin broadcasting per frame. Off by default — opt in per session.","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_motion_disable_cost","description":"Disable the cost-attribution channel via Motion.disableCost. Stops Motion.cost broadcasts. Safe to call when cost is already disabled.","inputSchema":{"type":"object","properties":{}}},
{"name":"pulp_compat","description":"Report pulp-mcp / MCP protocol / project SDK versions plus per-tool min_sdk_version floors so clients can pre-filter their tool list. Use this once at startup to detect SDK skew (#2070).","inputSchema":{"type":"object","properties":{}}}
]})JSON";
}

// MCP spec: stdio transport messages are line-delimited and MUST NOT
// contain embedded `\n` or `\r`. Several response builders use
// multi-line R"JSON(...)" raw strings for readability. Strip those
// here so every wire-bound response is a single line.
//
// B4 (2026-05): moved out of main()'s I/O loop into a free function +
// applied at the bottom of handle_request, so the contract holds for
// every caller — not just main's stdio path. Pinned by
// `test/test_mcp_server.cpp` "MCP wire output never contains embedded
// newlines" [issue-2091].
static std::string compact_for_wire(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c != '\n' && c != '\r') out += c;
    }
    return out;
}

static std::string handle_request_raw(const std::string& json);

static std::string handle_request(const std::string& json) {
    return compact_for_wire(handle_request_raw(json));
}

static std::string handle_request_raw(const std::string& json) {
    auto method = extract_string(json, "method");
    auto id = extract_raw(json, "id");
    if (id.empty()) id = "null";

    if (method == "initialize") {
        // serverInfo.version tracks the SDK/CLI release (#2067).
        // Held constant at "0.1.0" pre-fix; now wired to PROJECT_VERSION
        // via tools/mcp/pulp_mcp_version.h.in so doctor/launcher can see
        // real drift between an old installed pulp-mcp and a newer plugin.
        std::string payload =
            std::string(R"JSON({"protocolVersion":"2024-11-05","capabilities":{"tools":{}},"serverInfo":{"name":"pulp-mcp","version":")JSON")
            + PULP_MCP_SERVER_VERSION
            + std::string(R"JSON("}})JSON");
        return json_result(id, payload);
    }

    if (method == "notifications/initialized") {
        return {}; // No response for notifications
    }

    if (method == "tools/list") {
        return json_result(id, tools_list_json());
    }

    if (method == "tools/call") {
        auto name = extract_string(json, "name");
        // Extract the arguments sub-object (simplified)
        auto args_pos = json.find("\"arguments\"");
        std::string args_json = "{}";
        if (args_pos != std::string::npos) {
            auto brace = json.find('{', args_pos);
            if (brace != std::string::npos) {
                int depth = 1;
                auto end = brace + 1;
                while (end < json.size() && depth > 0) {
                    if (json[end] == '{') ++depth;
                    if (json[end] == '}') --depth;
                    ++end;
                }
                args_json = json.substr(brace, end - brace);
            }
        }

        // Per-tool feature detection (#2070). If the tool declares a
        // min_sdk floor and the project pins an older SDK, return a
        // structured error result with `isError: true` so the LLM gets
        // actionable upgrade guidance instead of silently running the
        // newer behavior. `pulp_compat` is exempt — clients invoke it
        // *to* discover skew and must always be able to read the
        // matrix. Tools left out of the table default to "0.0.0" so
        // existing behavior is unchanged.
        if (name != "pulp_compat") {
            auto min_sdk = min_sdk_for_tool(name);
            if (min_sdk != "0.0.0") {
                auto project_sdk = resolve_project_sdk_version();
                if (!project_sdk.empty() &&
                    compare_semver(project_sdk, min_sdk) < 0) {
                    return json_result(
                        id, compat_error_payload(name, min_sdk, project_sdk));
                }
                // If we couldn't resolve the project SDK at all we
                // fall open — same as pre-#2070. The launcher has
                // already started; gating on "no project root" would
                // make pulp-mcp unusable from `/tmp` or similar.
            }
        }

        std::string result;
        if (name == "pulp_compat")         result = handle_compat();
        else if (name == "pulp_build")          result = handle_build(args_json);
        else if (name == "pulp_test")      result = handle_test(args_json);
        else if (name == "pulp_status")    result = handle_status(args_json);
        else if (name == "pulp_validate")  result = handle_validate(args_json);
        else if (name == "pulp_audio_model_list")     result = handle_audio_model_list(args_json);
        else if (name == "pulp_audio_model_status")   result = handle_audio_model_status(args_json);
        else if (name == "pulp_audio_model_activate") result = handle_audio_model_activate(args_json);
        else if (name == "pulp_audio_excerpt_find")   result = handle_audio_excerpt_find(args_json);
        else if (name == "pulp_audio_read_bundle")    result = handle_audio_read_bundle(args_json);
        else if (name == "pulp_screenshot" || name == "pulp_simulate_click" || name == "pulp_get_view_tree") {
            // These tools delegate to pulp-screenshot binary
            auto root = find_project_root();
            if (root.empty()) {
                result = "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";
            } else {
                auto screenshot_bin = root / "build" / "tools" / "screenshot" / "pulp-screenshot";
                if (name == "pulp_screenshot") {
                    auto demo = extract_string(args_json, "demo");
                    auto script = extract_string(args_json, "script");
                    std::string cmd = shell_quote(screenshot_bin.string()) + " --base64";
                    if (!script.empty()) cmd += " --script " + shell_quote(script);
                    else cmd += " --demo";
                    auto theme = extract_string(args_json, "theme");
                    if (!theme.empty()) cmd += " --theme " + shell_quote(theme);
                    auto output = exec(cmd + " 2>/dev/null");
                    result = "{\"content\":[{\"type\":\"image\",\"data\":\"" + output + "\",\"mimeType\":\"image/png\"}]}";
                } else {
                    // simulate_click and get_view_tree: run screenshot in demo mode, capture view tree
                    std::string cmd = shell_quote(screenshot_bin.string()) + " --demo --output /dev/null 2>/dev/null";
                    exec(cmd);
                    result = "{\"content\":[{\"type\":\"text\",\"text\":\"View tree and event simulation available via pulp-screenshot --demo\"}]}";
                }
            }
        }
        else if (name == "pulp_create") {
            auto root = find_project_root();
            if (root.empty()) {
                result = "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";
            } else {
                auto plugin_name = extract_string(args_json, "name");
                auto plugin_type = extract_string(args_json, "type");
                auto manufacturer = extract_string(args_json, "manufacturer");
                if (plugin_name.empty()) {
                    result = "{\"content\":[{\"type\":\"text\",\"text\":\"Error: name is required\"}]}";
                } else {
                    std::string cmd = "python3 " + shell_quote((root / "tools" / "create-project.py").string());
                    cmd += " " + shell_quote(plugin_name);
                    if (!plugin_type.empty()) cmd += " --type " + shell_quote(plugin_type);
                    if (!manufacturer.empty()) cmd += " --manufacturer " + shell_quote(manufacturer);
                    auto output = exec(cmd + " 2>&1");
                    result = "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
                }
            }
        }
        else if (name == "pulp_docs_check") {
            auto root = find_project_root();
            if (root.empty()) {
                result = "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";
            } else {
                auto output = exec("bash " + shell_quote((root / "tools" / "check-docs.sh").string()) + " 2>&1");
                result = "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
            }
        }
        else if (name == "pulp_docs_search") {
            auto root = find_project_root();
            auto query = extract_string(args_json, "query");
            if (root.empty()) {
                result = "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";
            } else if (query.empty()) {
                result = "{\"content\":[{\"type\":\"text\",\"text\":\"Error: query is required\"}]}";
            } else {
                auto output = exec(shell_quote((root / "build" / "tools" / "cli" / "pulp").string()) +
                                   " docs search " + shell_quote(query) + " 2>&1");
                result = "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
            }
        }
        // Motion inspector tools — delegate to pulp inspect CLI the
        // same way pulp_inspect_* does. Each tool maps to one of the
        // Motion.* protocol methods routed by MotionInspector::handle
        // (inspect/src/motion_inspector.cpp) and MotionScrubber::handle
        // (inspect/src/motion_scrubber.cpp). Off-by-default semantics:
        // tools that operate on global state (snapshot, listTraces,
        // pause, disableCost) return clean payloads even when nothing
        // is active; tools that require prior state (scrubTo / play
        // without a loaded fixture, stopTrace with an unknown id) get
        // a structured inspector error which we propagate verbatim.
        // pulp_motion_start_trace flips Coordinator::tracing_enabled
        // on attach (matches motion_inspector.cpp:~265), so callers
        // don't need to pre-arm tracing.
        else if (name == "pulp_motion_start_trace" || name == "pulp_motion_stop_trace" ||
                 name == "pulp_motion_snapshot" || name == "pulp_motion_list_traces" ||
                 name == "pulp_motion_load_fixture" || name == "pulp_motion_scrub_to" ||
                 name == "pulp_motion_play" || name == "pulp_motion_pause" ||
                 name == "pulp_motion_enable_cost" || name == "pulp_motion_disable_cost") {
            std::string inspector_method;
            std::string inspector_params;  // pulp inspect --params JSON, empty for none
            if (name == "pulp_motion_start_trace") {
                inspector_method = "Motion.startTrace";
                // Forward the raw args sub-object as params verbatim
                // — the inspector parses {view_name, fps, metrics}
                // out of it. Empty {} would fail with "metrics array
                // required" on the inspector side, which is the
                // right answer for callers that omit metrics.
                inspector_params = " --params '" + args_json + "'";
            } else if (name == "pulp_motion_stop_trace") {
                inspector_method = "Motion.stopTrace";
                auto trace_id_raw = extract_raw(args_json, "trace_id");
                if (trace_id_raw.empty()) trace_id_raw = "0";
                inspector_params = std::string(" --params '{\"trace_id\":") + trace_id_raw + "}'";
            } else if (name == "pulp_motion_snapshot") {
                inspector_method = "Motion.snapshot";
            } else if (name == "pulp_motion_list_traces") {
                inspector_method = "Motion.listTraces";
            } else if (name == "pulp_motion_load_fixture") {
                inspector_method = "Motion.loadFixture";
                auto path = extract_string(args_json, "path");
                // Shell-quote the JSON object in single quotes so the
                // file path's spaces and special characters survive
                // the cli's argv parse. Paths cannot contain single
                // quotes in practice (POSIX) — if a user passes one
                // the inspector will reject the malformed JSON, which
                // is the right answer.
                inspector_params = std::string(" --params '{\"path\":\"") + path + "\"}'";
            } else if (name == "pulp_motion_scrub_to") {
                inspector_method = "Motion.scrubTo";
                auto frame_raw = extract_raw(args_json, "frame");
                if (frame_raw.empty()) frame_raw = "0";
                inspector_params = std::string(" --params '{\"frame\":") + frame_raw + "}'";
            } else if (name == "pulp_motion_play") {
                inspector_method = "Motion.play";
            } else if (name == "pulp_motion_pause") {
                inspector_method = "Motion.pause";
            } else if (name == "pulp_motion_enable_cost") {
                inspector_method = "Motion.enableCost";
            } else if (name == "pulp_motion_disable_cost") {
                inspector_method = "Motion.disableCost";
            }

            auto root = find_project_root();
            if (root.empty()) {
                result = "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";
            } else {
                auto cli = (root / "build" / "tools" / "cli" / "pulp").string();
                auto output = exec(cli + " inspect --command " + inspector_method + inspector_params + " 2>&1");
                result = "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
            }
        }
        // Inspector tools — delegate to pulp inspect CLI for now
        // (in the future these could connect directly via TCP)
        else if (name == "pulp_inspect_dom" || name == "pulp_inspect_params" ||
                 name == "pulp_inspect_screenshot" || name == "pulp_inspect_evaluate" ||
                 name == "pulp_inspect_performance" || name == "pulp_inspect_audio") {
            // Map MCP tool name to inspector protocol method
            std::string inspector_method;
            std::string inspector_params;
            if (name == "pulp_inspect_dom")         inspector_method = "DOM.getDocument";
            else if (name == "pulp_inspect_params")  inspector_method = "State.getParameters";
            else if (name == "pulp_inspect_screenshot") inspector_method = "Capture.screenshot";
            else if (name == "pulp_inspect_evaluate") {
                inspector_method = "Runtime.evaluate";
                auto expr = extract_string(args_json, "expression");
                if (!expr.empty()) inspector_params = " {\"expression\":" + json_string(expr) + "}";
            }
            else if (name == "pulp_inspect_performance") inspector_method = "Performance.getMetrics";
            else if (name == "pulp_inspect_audio")   inspector_method = "Audio.getConfig";

            auto root = find_project_root();
            if (root.empty()) {
                result = "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";
            } else {
                auto cli = (root / "build" / "tools" / "cli" / "pulp").string();
                auto output = exec(cli + " inspect --command " + inspector_method + inspector_params + " 2>&1");
                if (name == "pulp_inspect_screenshot") {
                    // Screenshot returns base64 PNG — escape for safe JSON embedding
                    result = "{\"content\":[{\"type\":\"image\",\"data\":" + json_string(output) + ",\"mimeType\":\"image/png\"}]}";
                } else {
                    result = "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
                }
            }
        }
        else return json_error(id, -32601, "Unknown tool: " + name);

        return json_result(id, result);
    }

    if (method == "ping") {
        return json_result(id, "{}");
    }

    return json_error(id, -32601, "Method not found: " + method);
}

// ── Main: stdio JSON-RPC transport ───────────────────────────────────────────

int main(int argc, char* argv[]) {
    // Flag-only invocations short-circuit the JSON-RPC loop so the
    // release-CLI smoke gate and `pulp doctor` can probe the binary
    // without speaking MCP framing. Keep this list narrow — anything
    // that consumes stdin must fall through to the loop below.
    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--version" || arg == "-V") {
            std::cout << "pulp-mcp " << PULP_MCP_SERVER_VERSION << "\n";
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            std::cout
                << "pulp-mcp " << PULP_MCP_SERVER_VERSION << "\n"
                << "MCP (Model Context Protocol) server for Pulp.\n"
                << "Speaks JSON-RPC 2.0 over stdin/stdout — normally\n"
                << "invoked by .mcp.json via tools/mcp/pulp-mcp-launcher.\n"
                << "\n"
                << "Flags:\n"
                << "  --version, -V   Print version and exit\n"
                << "  --help, -h      Show this help\n";
            return 0;
        }
        std::cerr << "pulp-mcp: unknown flag '" << arg
                  << "'. Try --help.\n";
        return 2;
    }

    // MCP spec: messages on the stdio transport are delimited by
    // newlines and MUST NOT contain embedded `\n` or `\r`. The
    // newline-stripping contract now lives inside `handle_request`
    // itself (B4 2026-05), so callers here just pass through.

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        // MCP uses Content-Length header framing
        if (line.find("Content-Length:") == 0) {
            int length = std::stoi(line.substr(15));
            std::getline(std::cin, line); // empty line
            std::string body(length, '\0');
            std::cin.read(body.data(), length);

            auto response = handle_request(body);
            if (!response.empty()) {
                std::cout << "Content-Length: " << response.size() << "\r\n\r\n" << response;
                std::cout.flush();
            }
            continue;
        }

        // Also handle bare JSON (for simpler testing AND the
        // newline-delimited MCP stdio transport that Claude Code uses).
        auto response = handle_request(line);
        if (!response.empty()) {
            std::cout << response << "\n";
            std::cout.flush();
        }
    }

    return 0;
}
