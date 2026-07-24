// mcp_audio_tools.cpp — Audio service, probe, render, and comparison MCP handlers.

#include "mcp_json.hpp"
#include "mcp_shell.hpp"
#include "mcp_tools.hpp"
#include "mcp_tools_internal.hpp"

#include <pulp/tools/audio/excerpt_service.hpp>
#include <pulp/tools/audio/model_store.hpp>
#include <pulp/tools/audio/service.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace pulp_mcp {

std::string handle_audio_model_status(const std::string& /*params_json*/) {
    auto status = pulp::tools::audio::query_model_status();
    return json_tool_payload(pulp::tools::audio::to_json(status));
}

std::string handle_audio_model_list(const std::string& /*params_json*/) {
    auto result = pulp::tools::audio::list_models();
    return json_tool_payload(pulp::tools::audio::to_json(result));
}

std::string handle_audio_model_activate(const std::string& params_json) {
    auto model_id = extract_string(params_json, "model_id");
    if (model_id.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: model_id is required\"}]}";
    }

    auto result = pulp::tools::audio::activate_model(model_id);
    return json_tool_payload(pulp::tools::audio::to_json(result));
}

std::string handle_audio_read_bundle(const std::string& params_json) {
    auto bundle_path = extract_string(params_json, "bundle_path");
    if (bundle_path.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: bundle_path is required\"}]}";
    }

    auto bundle = pulp::tools::audio::read_excerpt_bundle(bundle_path);
    return json_tool_payload(pulp::tools::audio::to_json(bundle));
}

std::string handle_audio_excerpt_find(const std::string& params_json) {
    auto text = extract_string(params_json, "text");
    auto input_path = extract_string(params_json, "input_path");
    if (text.empty() || input_path.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: text and input_path are "
               "required\"}]}";
    }

    pulp::tools::audio::ExcerptFindRequest request;
    request.text = text;
    request.input_path = input_path;
    request.model_id = extract_string(params_json, "model_id");
    request.recursive = extract_bool(params_json, "recursive", false);
    request.top_k = static_cast<std::size_t>(extract_int(params_json, "top", 5));
    request.window_ms = static_cast<uint64_t>(extract_int(params_json, "window_ms", 1500));
    request.hop_ms = static_cast<uint64_t>(extract_int(params_json, "hop_ms", 250));
    request.min_score = extract_double(params_json, "min_score", 0.0);
    request.max_candidates_per_file =
        static_cast<std::size_t>(extract_int(params_json, "max_candidates_per_file", 3));
    request.bundle_out = extract_string(params_json, "bundle_out");

    auto result = pulp::tools::audio::run_excerpt_find(request);
    return json_tool_payload(pulp::tools::audio::to_json(result));
}

std::string handle_audio_probe_json(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";
    }

    int frames = 90;
    auto frames_raw = extract_raw(params_json, "frames");
    if (!frames_raw.empty() && frames_raw != "null") {
        frames = extract_int(params_json, "frames", -1);
        if (frames <= 0) {
            return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: frames must be a positive "
                   "integer\"}]}";
        }
    }

    auto target = extract_string(params_json, "target");
    if (!target.empty() && target.front() == '-') {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: target must be a standalone "
               "target name, not an option\"}]}";
    }
    std::string temp_error;
    auto temp = make_private_probe_json_temp(temp_error);
    if (temp.json_path.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string("Error: " + temp_error) +
               "}]}";
    }
    auto output_path = temp.json_path;

    std::string cmd = shell_quote(source_build_cli_path(root).string()) + " run";
    if (!target.empty())
        cmd += " " + shell_quote(target);
    cmd += " --audio-probe-json " + shell_quote(output_path.string());
    cmd += " --frames " + std::to_string(frames);
    cmd += " 2>&1";

    auto output = exec(cmd);
    auto probe_json = read_text_file(output_path);
    std::error_code remove_ec;
    fs::remove_all(temp.directory, remove_ec);

    if (probe_json.empty()) {
        std::string message = "Error: pulp run did not write audio probe JSON";
        if (!output.empty())
            message += "\n" + output;
        return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(message) + "}]}";
    }
    std::string normalized_json;
    std::string parse_error;
    if (!normalize_structured_json(probe_json, normalized_json, parse_error)) {
        std::string message = "Error: " + parse_error + "\n" + probe_json;
        if (!output.empty())
            message += "\n" + output;
        return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(message) + "}]}";
    }

    return json_tool_payload(normalized_json);
}

std::string handle_audio_scope(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";
    }

    int frames = 90;
    auto frames_raw = extract_raw(params_json, "frames");
    if (!frames_raw.empty() && frames_raw != "null") {
        frames = extract_int(params_json, "frames", -1);
        if (frames <= 0) {
            return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: frames must be a positive "
                   "integer\"}]}";
        }
    }

    int window = 2048;
    auto window_raw = extract_raw(params_json, "window");
    if (!window_raw.empty() && window_raw != "null") {
        window = extract_int(params_json, "window", -1);
        if (window <= 0) {
            return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: window must be a positive "
                   "integer\"}]}";
        }
    }

    int channel = 0;
    auto channel_raw = extract_raw(params_json, "channel");
    if (!channel_raw.empty() && channel_raw != "null") {
        channel = extract_int(params_json, "channel", -1);
        if (channel < 0) {
            return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: channel must be a "
                   "non-negative integer\"}]}";
        }
    }

    auto trigger = extract_string(params_json, "trigger");
    if (trigger.empty())
        trigger = "rising-zero";
    auto normalized_trigger = trigger;
    std::replace(normalized_trigger.begin(), normalized_trigger.end(), '_', '-');
    if (normalized_trigger != "none" && normalized_trigger != "off" &&
        normalized_trigger != "raw" && normalized_trigger != "rising-zero") {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: trigger must be one of none, "
               "raw, off, rising-zero\"}]}";
    }

    auto target = extract_string(params_json, "target");
    if (!target.empty() && target.front() == '-') {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: target must be a standalone "
               "target name, not an option\"}]}";
    }

    auto input_wav = extract_string(params_json, "input_wav");
    auto png_path = extract_string(params_json, "png_path");
    if (!input_wav.empty() && !target.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: target and input_wav are "
               "mutually exclusive\"}]}";
    }
    if (!png_path.empty() && input_wav.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: png_path is only supported with "
               "input_wav\"}]}";
    }

    std::string temp_error;
    auto temp = make_private_probe_json_temp(temp_error);
    if (temp.json_path.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string("Error: " + temp_error) +
               "}]}";
    }
    auto output_path = temp.directory / "scope.json";

    std::string cmd = shell_quote(source_build_cli_path(root).string()) + " audio scope";
    if (!target.empty())
        cmd += " " + shell_quote(target);
    if (!input_wav.empty())
        cmd += " --input-wav " + shell_quote(input_wav);
    cmd += " --json " + shell_quote(output_path.string());
    cmd += " --frames " + std::to_string(frames);
    cmd += " --window " + std::to_string(window);
    cmd += " --trigger " + shell_quote(trigger);
    cmd += " --channel " + std::to_string(channel);
    if (!png_path.empty())
        cmd += " --png " + shell_quote(png_path);
    cmd += " 2>&1";

    auto output = exec(cmd);
    auto scope_json = read_text_file(output_path);
    std::error_code remove_ec;
    fs::remove_all(temp.directory, remove_ec);

    if (scope_json.empty()) {
        std::string message = "Error: pulp audio scope did not write scope JSON";
        if (!output.empty())
            message += "\n" + output;
        return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(message) + "}]}";
    }

    std::string normalized_json;
    std::string parse_error;
    if (!normalize_structured_json(scope_json, normalized_json, parse_error)) {
        std::string message = "Error: " + parse_error + "\n" + scope_json;
        if (!output.empty())
            message += "\n" + output;
        return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(message) + "}]}";
    }

    return json_tool_payload(normalized_json);
}

std::string handle_audio_plugin_inspect(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";
    }
    auto arg_error = [](const std::string& msg) {
        return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(msg) + "}]}";
    };
    const auto plugin = extract_string(params_json, "plugin");
    if (plugin.empty() || plugin.front() == '-')
        return arg_error("Error: plugin is required and must be a bundle path");

    std::string cmd = shell_quote(source_build_cli_path(root).string()) +
                      " audio plugin-inspect --plugin " + shell_quote(plugin);
    auto add_str = [&](const char* key, const char* flag) {
        const auto value = extract_string(params_json, key);
        if (!value.empty())
            cmd += std::string(" ") + flag + " " + shell_quote(value);
    };
    add_str("format", "--format");
    add_str("id", "--id");

    auto has = [&](const char* key) {
        const auto raw = extract_raw(params_json, key);
        return !raw.empty() && raw != "null";
    };
    auto add_int = [&](const char* key, const char* flag, int minimum) -> std::string {
        if (!has(key))
            return {};
        const int value = extract_int(params_json, key, minimum - 1);
        if (value < minimum)
            return std::string("Error: ") + key +
                   " must be an integer >= " + std::to_string(minimum);
        cmd += std::string(" ") + flag + " " + std::to_string(value);
        return {};
    };
    for (auto error : {add_int("block", "--block", 1), add_int("warmup_ms", "--warmup-ms", 0),
                       add_int("timeout_ms", "--timeout-ms", 1)}) {
        if (!error.empty())
            return arg_error(error);
    }
    if (has("sample_rate")) {
        const double rate = extract_double(params_json, "sample_rate", -1.0);
        if (!(std::isfinite(rate) && rate > 0.0))
            return arg_error("Error: sample_rate must be positive");
        cmd += " --sample-rate " + std::to_string(rate);
    }
    std::string temp_error;
    auto temp = make_private_probe_json_temp(temp_error);
    if (temp.json_path.empty())
        return arg_error("Error: " + temp_error);
    const auto diagnostics_path = temp.directory / "diagnostics.txt";
    // Keep stdout as the JSON protocol. Vendor libraries and the host scanner
    // may write diagnostics to stderr even on success.
    cmd += " 2> " + shell_quote(diagnostics_path.string());
    const auto run = exec_with_status(cmd);
    const auto diagnostics = read_text_file(diagnostics_path);
    std::error_code remove_ec;
    fs::remove_all(temp.directory, remove_ec);
    if (run.failed()) {
        std::string message = "Error: isolated plugin inspection failed.";
        if (!diagnostics.empty())
            message += "\n" + diagnostics;
        if (!run.output.empty())
            message += "\n" + run.output;
        return arg_error(message);
    }
    std::string normalized;
    std::string parse_error;
    if (!normalize_structured_json(run.output, normalized, parse_error))
        return arg_error("Error: " + parse_error + "\n" + run.output);
    return json_tool_payload(normalized);
}

std::string handle_audio_render(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";
    }

    auto plugin = extract_string(params_json, "plugin");
    if (plugin.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: plugin is required (path to a "
               "plugin bundle)\"}]}";
    }
    if (plugin.front() == '-') {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: plugin must be a bundle path, "
               "not an option\"}]}";
    }

    auto out = extract_string(params_json, "out");
    if (!out.empty() && out.front() == '-') {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: out must be a file path, not an "
               "option\"}]}";
    }

    auto has = [&](const char* key) {
        auto raw = extract_raw(params_json, key);
        return !raw.empty() && raw != "null";
    };
    auto arg_error = [](const std::string& msg) {
        return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(msg) + "}]}";
    };

    // ── Validate every argument BEFORE creating any temp dir, so an early
    //    rejection can't leak one. The command flags are accumulated here. ──
    std::string flags;

    // Exactly one duration source (mirrors the CLI's mutually-exclusive contract).
    const bool has_ms = has("duration_ms");
    const bool has_frames = has("duration_frames");
    if (has_ms == has_frames) {
        return arg_error("Error: pass exactly one of duration_ms / duration_frames");
    }
    if (has_ms) {
        int ms = extract_int(params_json, "duration_ms", -1);
        if (ms <= 0)
            return arg_error("Error: duration_ms must be a positive integer");
        flags += " --duration-ms " + std::to_string(ms);
    } else {
        int fr = extract_int(params_json, "duration_frames", -1);
        if (fr <= 0)
            return arg_error("Error: duration_frames must be a positive integer");
        flags += " --duration-frames " + std::to_string(fr);
    }

    auto add_str = [&](const char* key, const char* flag) {
        auto v = extract_string(params_json, key);
        if (!v.empty())
            flags += std::string(" ") + flag + " " + shell_quote(v);
    };
    add_str("format", "--format");
    add_str("id", "--id");
    add_str("input", "--input");
    add_str("input_signal", "--input-signal");
    add_str("initial_param", "--initial-param");
    add_str("wav_format", "--wav-format");
    add_str("param", "--param"); // a single id=value[@frame]; multiple → use the CLI
    add_str("midi", "--midi");   // a single note:n,vel,on[,off]; multiple → use the CLI

    auto add_int = [&](const char* key, const char* flag, int min_value) -> std::string {
        if (has(key)) {
            int v = extract_int(params_json, key, min_value - 1);
            if (v < min_value)
                return std::string("Error: ") + key +
                       " must be an integer >= " + std::to_string(min_value);
            flags += std::string(" ") + flag + " " + std::to_string(v);
        }
        return {};
    };
    for (auto err :
         {add_int("block", "--block", 1), add_int("in_channels", "--in-channels", 0),
          add_int("out_channels", "--out-channels", 1), add_int("warmup_ms", "--warmup-ms", 0),
          add_int("settle_ms", "--settle-ms", 0), add_int("timeout_ms", "--timeout-ms", 1),
          add_int("tail_ms", "--tail-ms", 0)}) {
        if (!err.empty())
            return arg_error(err);
    }
    if (has("sample_rate")) {
        double sr = extract_double(params_json, "sample_rate", -1.0);
        if (sr <= 0.0)
            return arg_error("Error: sample_rate must be positive");
        flags += " --sample-rate " + std::to_string(sr);
    }

    // ── All args valid. Now make a private temp dir (for the metrics manifest,
    //    and the WAV when `out` is omitted); it is cleaned on every exit below. ──
    std::string temp_error;
    auto temp = make_private_probe_json_temp(temp_error);
    if (temp.json_path.empty()) {
        return arg_error("Error: " + temp_error);
    }
    const fs::path temp_dir = temp.directory;
    if (out.empty())
        out = (temp_dir / "render.wav").string();
    const auto manifest_path = (temp_dir / "metrics.json").string();

    // Latency proof: opt-in. `latency: true` asks the render to prove that the
    // plugin's reported latency matches the delay actually in its output.
    const bool want_latency = has("latency") && extract_bool(params_json, "latency", false);
    const auto latency_path = (temp_dir / "latency.json").string();

    std::string cmd = shell_quote(source_build_cli_path(root).string()) + " audio render";
    cmd += " --plugin " + shell_quote(plugin);
    cmd += " --out " + shell_quote(out);
    cmd += " --manifest " + shell_quote(manifest_path);
    if (want_latency) {
        cmd += " --latency-report " + shell_quote(latency_path);
        add_str("latency_policy", "--latency-policy");
        if (has("latency_tolerance")) {
            const int tol = extract_int(params_json, "latency_tolerance", -1);
            if (tol < 0)
                return arg_error("Error: latency_tolerance must be an integer >= 0");
            cmd += " --latency-tolerance " + std::to_string(tol);
        }
        if (has("latency_intrinsic")) {
            const int intrinsic = extract_int(params_json, "latency_intrinsic", -1);
            if (intrinsic < 0)
                return arg_error("Error: latency_intrinsic must be an integer >= 0");
            cmd += " --latency-intrinsic " + std::to_string(intrinsic);
        }
        if (has("latency_expect")) {
            const int expect = extract_int(params_json, "latency_expect", -1);
            if (expect < 0)
                return arg_error("Error: latency_expect must be an integer >= 0");
            cmd += " --latency-expect " + std::to_string(expect);
        }
    }
    cmd += flags;
    // Read the manifest from a FILE (like audio scope) so stderr — plugin load /
    // prepare / write failures, objc warnings — can be captured via 2>&1 and
    // surfaced on failure instead of corrupting the JSON or vanishing.
    cmd += " 2>&1";

    // Keep the EXIT STATUS. Merging stderr into stdout above means the status is
    // the only signal left that the render failed, and a latency proof fails by
    // exiting nonzero while still writing a perfectly well-formed report. Reading
    // only the artifact would report a DISPROVEN latency to the agent as a
    // success — the exact conflation this proof exists to prevent.
    const auto run = exec_with_status(cmd);
    const auto& output = run.output;

    auto manifest_json = read_text_file(manifest_path);
    auto latency_json = want_latency ? read_text_file(latency_path) : std::string{};
    std::error_code ec;
    fs::remove_all(temp_dir, ec);

    if (manifest_json.empty()) {
        std::string message = "Error: pulp audio render did not write a metrics manifest";
        if (!output.empty())
            message += "\n" + output;
        return arg_error(message);
    }

    std::string normalized_json;
    std::string parse_error;
    if (!normalize_structured_json(manifest_json, normalized_json, parse_error)) {
        std::string message = "Error: " + parse_error + "\n" + manifest_json;
        if (!output.empty())
            message += "\n" + output;
        return arg_error(message);
    }

    if (want_latency) {
        if (latency_json.empty()) {
            std::string message = "Error: pulp audio render did not write a latency report";
            if (!output.empty())
                message += "\n" + output;
            return arg_error(message);
        }
        // A failed proof is an ERROR result, not a payload the caller has to
        // remember to inspect. The evidence goes with it so the reason is right
        // there: which delay was measured, which was reported, or why neither.
        if (run.failed())
            return arg_error("Error: the plugin's reported latency was not proven.\n" +
                             latency_json);
        return json_tool_payload(latency_json);
    }

    return json_tool_payload(normalized_json);
}

std::string handle_audio_compare(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";
    }
    auto arg_error = [](const std::string& msg) {
        return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(msg) + "}]}";
    };

    auto reference = extract_string(params_json, "reference");
    auto candidate = extract_string(params_json, "candidate");
    if (reference.empty() || candidate.empty()) {
        return arg_error("Error: reference and candidate are required (paths to WAV files)");
    }
    if (reference.front() == '-' || candidate.front() == '-') {
        return arg_error("Error: reference and candidate must be WAV paths, not options");
    }

    std::string flags;
    auto profile = extract_string(params_json, "profile");
    if (!profile.empty()) {
        // Passthrough: the valid profile SET lives in the Python `_AXES` registry (surfaced as the
        // CLI's argparse `choices`), which is the single source of truth and grows as axes are
        // added — so this mirror is not re-edited per new axis. Guard only that it is not an
        // option-looking string; an unknown profile is rejected downstream by the delegated CLI.
        if (profile.front() == '-')
            return arg_error("Error: profile must be an axis name, not an option");
        flags += " --profile " + shell_quote(profile);
    }
    auto role = extract_string(params_json, "reference_role");
    if (!role.empty()) {
        if (role != "peer" && role != "golden")
            return arg_error("Error: reference_role must be peer or golden");
        flags += " --reference-role " + shell_quote(role);
    }
    auto align = extract_string(params_json, "align");
    if (!align.empty()) {
        // Validate the MODE prefix here for a fast, helpful error; the full `mode[:param]` grammar
        // (e.g. varispeed:1.5) is owned + re-validated by the Python alignment.parse layer.
        auto mode = align.substr(0, align.find(':'));
        if (mode != "none" && mode != "latency" && mode != "varispeed" && mode != "stretch" &&
            mode != "pitch" && mode != "ratio")
            return arg_error("Error: align must be none, latency, varispeed:<ratio>, "
                             "stretch:<ratio>, pitch:<semitones>, or ratio:auto");
        flags += " --align " + shell_quote(align);
    }
    if (auto raw = extract_raw(params_json, "threshold"); !raw.empty() && raw != "null") {
        double t = extract_double(params_json, "threshold", -1.0);
        // Passthrough: the VALID range is per-axis and lives in the Python registry (a
        // dimensionless fraction in (0,1) for tonal-balance, a dB magnitude for added-hf), which
        // is the single source of truth and rejects an out-of-range value itself. Guarding only
        // the universal invariant here — a threshold is a finite positive magnitude
        // (abs(delta) >= threshold) — so a valid dB threshold like 3.0 is no longer rejected by a
        // hardcoded (0,1) fraction bound that only fits one axis.
        if (!(std::isfinite(t) && t > 0.0))
            return arg_error("Error: threshold must be a finite positive number");
        // Shortest round-trippable form — std::to_string fixes 6 decimals and would
        // silently floor a small threshold (e.g. 1e-4 → "0.000100", 1e-7 → "0.000000").
        std::array<char, 32> buf{};
        auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), t);
        if (ec != std::errc())
            return arg_error("Error: threshold could not be formatted");
        flags += " --threshold " + shell_quote(std::string(buf.data(), ptr));
    }

    // Always capture the structured report to a temp file and return it — the MCP
    // caller wants the typed evidence envelope, not the human summary line.
    std::string temp_error;
    auto temp = make_private_probe_json_temp(temp_error);
    if (temp.json_path.empty()) {
        return arg_error("Error: " + temp_error);
    }
    const fs::path temp_dir = temp.directory;
    const auto report_path = (temp_dir / "compare.json").string();

    std::string cmd = shell_quote(source_build_cli_path(root).string()) + " audio compare";
    cmd += " " + shell_quote(reference) + " " + shell_quote(candidate);
    cmd += flags;
    cmd += " --json " + shell_quote(report_path);
    cmd += " 2>&1"; // capture the install hint / tool stderr so failures surface as text
    auto output = exec(cmd);

    auto report_json = read_text_file(report_path);
    std::error_code ec;
    fs::remove_all(temp_dir, ec);

    if (report_json.empty()) {
        // No report written → the opt-in tool is absent or could not run. Surface the
        // captured hint/output (e.g. "Audio Quality Lab is not installed …") verbatim.
        std::string message =
            "Error: pulp audio compare produced no report (is the Audio Quality Lab tool "
            "installed? `pulp tool install audio-quality-lab`)";
        if (!output.empty())
            message += "\n" + output;
        return arg_error(message);
    }

    std::string normalized_json;
    std::string parse_error;
    if (!normalize_structured_json(report_json, normalized_json, parse_error)) {
        std::string message = "Error: " + parse_error + "\n" + report_json;
        if (!output.empty())
            message += "\n" + output;
        return arg_error(message);
    }
    return json_tool_payload(normalized_json);
}

} // namespace pulp_mcp
