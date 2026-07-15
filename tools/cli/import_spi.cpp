// SPDX-License-Identifier: MIT
#include "import_spi.hpp"
#include "json_parser.hpp"

#include <pulp/platform/child_process.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

namespace pulp::cli::import_spi {

namespace fs = std::filesystem;
using pulp::cli::pkg::JsonParser;
using pulp::cli::pkg::JsonValue;

namespace {

// Compact re-serialiser for a parsed JsonValue. The minimal registry
// JsonParser doesn't track source spans, so we round-trip the parsed
// `result` object back to text for callers that reparse it. Numbers are
// emitted without trailing zeros where possible; strings are escaped.
void escape_json_string(const std::string& s, std::string& out) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    out += '"';
}

void serialize_value(const JsonValue& v, std::string& out) {
    switch (v.type) {
        case JsonValue::Null: out += "null"; break;
        case JsonValue::Bool: out += v.bool_val ? "true" : "false"; break;
        case JsonValue::Number: {
            double d = v.num_val;
            if (d == static_cast<long long>(d)) {
                out += std::to_string(static_cast<long long>(d));
            } else {
                std::ostringstream os;
                os << d;
                out += os.str();
            }
            break;
        }
        case JsonValue::String: escape_json_string(v.str_val, out); break;
        case JsonValue::Array: {
            out += '[';
            bool first = true;
            for (auto& e : v.arr()) {
                if (!first) out += ',';
                first = false;
                serialize_value(e, out);
            }
            out += ']';
            break;
        }
        case JsonValue::Object: {
            out += '{';
            bool first = true;
            for (auto& [k, val] : v.obj()) {
                if (!first) out += ',';
                first = false;
                escape_json_string(k, out);
                out += ':';
                serialize_value(val, out);
            }
            out += '}';
            break;
        }
    }
}

std::string serialize(const JsonValue& v) {
    std::string out;
    serialize_value(v, out);
    return out;
}

}  // namespace

// ── Request building ──

std::string build_request(const std::string& verb,
                          const std::string& id,
                          const std::string& payload_json,
                          int spi_version) {
    // payload_json is embedded verbatim. Callers pass a JSON object literal.
    std::ostringstream os;
    os << "{\"spi_version\":" << spi_version
       << ",\"verb\":\"" << verb << "\""
       << ",\"id\":\"" << id << "\""
       << ",\"payload\":" << (payload_json.empty() ? "{}" : payload_json)
       << "}";
    return os.str();
}

// ── Response parsing ──

SpiResponse parse_response(const std::string& line) {
    SpiResponse r;
    r.raw_stdout = line;

    // Trim leading whitespace to find the JSON object.
    size_t start = line.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        r.transport_error = "importer produced no response line";
        return r;
    }
    std::string trimmed = line.substr(start);

    JsonParser parser{trimmed};
    JsonValue root = parser.parse();
    if (root.type != JsonValue::Object) {
        r.transport_error = "response is not a JSON object";
        return r;
    }

    const JsonValue* ok = root.get("ok");
    if (!ok || ok->type != JsonValue::Bool) {
        r.transport_error = "response missing required boolean field 'ok'";
        return r;
    }

    r.transport_ok = true;
    r.ok = ok->as_bool();

    if (auto v = root.get("spi_version"); v && v->type == JsonValue::Number)
        r.spi_version = v->as_int();
    if (auto v = root.get("id")) r.id = v->as_string();

    if (auto result = root.get("result"); result && result->type == JsonValue::Object) {
        // Re-serialize the result object back to text so callers can reparse
        // the verb-specific shape with their own reader.
        r.result_json = serialize(*result);
    }

    if (auto err = root.get("error"); err && err->type == JsonValue::Object) {
        if (auto v = err->get("code")) r.error_code = v->as_string();
        if (auto v = err->get("message")) r.error_message = v->as_string();
    }

    if (auto diags = root.get("diagnostics"); diags && diags->type == JsonValue::Array) {
        for (auto& d : diags->arr()) {
            if (d.type != JsonValue::Object) continue;
            Diagnostic diag;
            if (auto v = d.get("severity")) diag.severity = v->as_string();
            if (auto v = d.get("code")) diag.code = v->as_string();
            if (auto v = d.get("message")) diag.message = v->as_string();
            r.diagnostics.push_back(std::move(diag));
        }
    }

    return r;
}

// ── Version negotiation ──

std::string check_version(int response_spi_version, int spi_min, int spi_max) {
    if (response_spi_version < 0) {
        return "importer response carried no spi_version";
    }
    if (response_spi_version < spi_min) {
        return "SPI version mismatch: importer spoke v"
               + std::to_string(response_spi_version)
               + " but this importer is registered for v"
               + std::to_string(spi_min) + "–v" + std::to_string(spi_max)
               + " (upgrade the importer)";
    }
    if (response_spi_version > spi_max) {
        return "SPI version mismatch: importer spoke v"
               + std::to_string(response_spi_version)
               + " but this Pulp build understands v"
               + std::to_string(spi_min) + "–v" + std::to_string(spi_max)
               + " (upgrade Pulp)";
    }
    return {};
}

// ── Command splitting ──

std::vector<std::string> split_command(const std::string& cmd) {
    std::vector<std::string> out;
    std::string cur;
    bool in_single = false, in_double = false, has_token = false;
    for (size_t i = 0; i < cmd.size(); ++i) {
        char c = cmd[i];
        if (in_single) {
            if (c == '\'') in_single = false;
            else { cur += c; }
            has_token = true;
        } else if (in_double) {
            if (c == '"') in_double = false;
            else if (c == '\\' && i + 1 < cmd.size()) { cur += cmd[++i]; }
            else { cur += c; }
            has_token = true;
        } else if (c == '\'') {
            in_single = true; has_token = true;
        } else if (c == '"') {
            in_double = true; has_token = true;
        } else if (c == '\\' && i + 1 < cmd.size()) {
            cur += cmd[++i]; has_token = true;
        } else if (c == ' ' || c == '\t') {
            if (has_token) { out.push_back(cur); cur.clear(); has_token = false; }
        } else {
            cur += c; has_token = true;
        }
    }
    if (has_token) out.push_back(cur);
    return out;
}

// ── Spawn helpers ──

namespace {

// Quote a single argument for inclusion in a POSIX `sh -c` line.
std::string sh_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

fs::path make_request_tempfile(const std::string& request_line) {
    std::random_device rd;
    std::ostringstream name;
    name << "pulp-import-spi-" << std::hex << rd() << rd() << ".req.json";
    fs::path p = fs::temp_directory_path() / name.str();
    std::ofstream f(p, std::ios::binary);
    f << request_line << "\n";
    f.close();
    return p;
}

}  // namespace

SpiResponse run(const ImporterInvocation& inv, const std::string& request_line) {
    SpiResponse result;

    std::vector<std::string> argv =
        !inv.argv.empty() ? inv.argv : split_command(inv.command_line);
    if (argv.empty()) {
        result.transport_error = "no importer command to run";
        return result;
    }

    // The SPI contract is "one request line on the importer's stdin, one
    // response line on its stdout". ChildProcess::run() captures stdout but
    // does not feed stdin, so we deliver the request via a temp file
    // redirected into the importer through a shell. No new platform infra,
    // and the importer still reads a real stdin pipe.
    fs::path req = make_request_tempfile(request_line);

    pulp::platform::ProcessOptions opts;
    opts.working_directory = inv.working_directory;
    opts.timeout_ms = inv.timeout_ms;
    opts.capture_stdout = true;
    opts.capture_stderr = true;

    std::string shell_cmd;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) shell_cmd += ' ';
#ifdef _WIN32
        // cmd /c: wrap each token in double quotes; cmd has weaker escaping
        // than sh, but importer paths + flags rarely need more.
        shell_cmd += '"' + argv[i] + '"';
#else
        shell_cmd += sh_quote(argv[i]);
#endif
    }

    pulp::platform::ProcessResult proc;
#ifdef _WIN32
    shell_cmd += " < \"" + req.string() + "\"";
    proc = pulp::platform::ChildProcess::run("cmd", {"/c", shell_cmd}, opts);
#else
    shell_cmd += " < " + sh_quote(req.string());
    proc = pulp::platform::ChildProcess::run("/bin/sh", {"-c", shell_cmd}, opts);
#endif

    std::error_code ec;
    fs::remove(req, ec);

    if (proc.timed_out) {
        result.transport_error = "importer timed out";
        return result;
    }

    // Take the first non-empty stdout line as the response envelope.
    std::string first_line;
    {
        std::istringstream is(proc.stdout_output);
        std::string l;
        while (std::getline(is, l)) {
            if (l.find_first_not_of(" \t\r\n") != std::string::npos) {
                first_line = l;
                break;
            }
        }
    }

    if (first_line.empty()) {
        result.transport_error =
            "importer produced no stdout response (exit "
            + std::to_string(proc.exit_code) + ")";
        if (!proc.stderr_output.empty())
            result.transport_error += "; stderr: " + proc.stderr_output;
        return result;
    }

    return parse_response(first_line);
}

}  // namespace pulp::cli::import_spi
