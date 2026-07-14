// SPDX-License-Identifier: MIT
#include "import_emit.hpp"
#include "json_parser.hpp"

namespace pulp::cli::import_emit {

using pulp::cli::pkg::JsonParser;
using pulp::cli::pkg::JsonValue;

namespace {

// Re-serialize a parsed JsonValue back to compact text (the registry parser
// keeps no source spans). Used to hand the migration_status object back to the
// SDK writer verbatim.
void escape(const std::string& s, std::string& out) {
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

void serialize(const JsonValue& v, std::string& out) {
    switch (v.type) {
        case JsonValue::Null: out += "null"; break;
        case JsonValue::Bool: out += v.bool_val ? "true" : "false"; break;
        case JsonValue::Number: {
            double d = v.num_val;
            if (d == static_cast<long long>(d))
                out += std::to_string(static_cast<long long>(d));
            else {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%g", d);
                out += buf;
            }
            break;
        }
        case JsonValue::String: escape(v.str_val, out); break;
        case JsonValue::Array: {
            out += '[';
            bool first = true;
            for (auto& e : v.arr()) {
                if (!first) out += ',';
                first = false;
                serialize(e, out);
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
                escape(k, out);
                out += ':';
                serialize(val, out);
            }
            out += '}';
            break;
        }
    }
}

std::vector<std::string> string_array(const JsonValue* v) {
    std::vector<std::string> out;
    if (v && v->type == JsonValue::Array) out = v->as_string_array();
    return out;
}

}  // namespace

Provenance parse_provenance(const std::string& s) {
    if (s == "generated") return Provenance::Generated;
    if (s == "copied-user-file") return Provenance::CopiedUserFile;
    if (s == "stub") return Provenance::Stub;
    return Provenance::Unknown;
}

const char* provenance_name(Provenance p) {
    switch (p) {
        case Provenance::Generated: return "generated";
        case Provenance::CopiedUserFile: return "copied-user-file";
        case Provenance::Stub: return "stub";
        default: return "unknown";
    }
}

std::string format_unresolved_summary(const Manifest& manifest) {
    std::string out = std::to_string(manifest.unresolved.size()) +
                      " item(s) to migrate";
    if (!manifest.migration_status_json.empty())
        out += " (see migration_status.json)";
    else
        out += " (no migration_status.json emitted)";
    return out;
}

std::string format_parity_review_step(const Manifest& manifest) {
    if (!manifest.migration_status_json.empty())
        return "Review migration_status.json for the unresolved DSP/UI parity work.";
    return "Review the importer-reported unresolved DSP/UI parity work.";
}

Manifest parse_manifest(const std::string& result_json) {
    Manifest m;
    JsonParser parser{result_json};
    JsonValue root = parser.parse();
    if (root.type != JsonValue::Object) {
        m.parse_error = "emit result is not a JSON object";
        return m;
    }

    if (auto v = root.get("schema")) m.schema = v->as_string();
    // Reject a wrong-schema response before reading any further: an analyze
    // ProjectIR or a future incompatible emit_result version carries a
    // different `schema`, and must not be parsed as this manifest. Absent is
    // tolerated (the file/provenance/content guards below still apply).
    if (!m.schema.empty() && m.schema != kEmissionManifestSchema) {
        m.parse_error = "emit result has schema '" + m.schema +
                        "', expected '" + kEmissionManifestSchema +
                        "' (wrong verb or incompatible importer?)";
        return m;
    }
    if (auto v = root.get("importer_id")) m.importer_id = v->as_string();
    if (auto v = root.get("framework")) m.framework = v->as_string();
    if (auto v = root.get("verdict")) m.verdict = v->as_string();

    m.formats = string_array(root.get("formats"));
    m.deferred_formats = string_array(root.get("deferred_formats"));
    m.unresolved = string_array(root.get("unresolved"));

    if (auto v = root.get("migration_status");
        v && v->type == JsonValue::Object) {
        std::string s;
        serialize(*v, s);
        m.migration_status_json = std::move(s);
    }

    const JsonValue* files = root.get("files");
    if (!files || files->type != JsonValue::Array) {
        m.parse_error = "emit result missing required 'files' array";
        return m;
    }

    for (auto& f : files->arr()) {
        if (f.type != JsonValue::Object) continue;
        ManifestFile mf;
        if (auto v = f.get("path")) mf.path = v->as_string();
        if (auto v = f.get("provenance"))
            mf.provenance = parse_provenance(v->as_string());
        if (auto v = f.get("classification")) mf.classification = v->as_string();
        if (auto v = f.get("content")) {
            mf.content = v->as_string();
            mf.has_content = true;
        }
        if (auto v = f.get("copy_from")) {
            mf.copy_from = v->as_string();
            mf.has_copy_from = true;
        }
        if (mf.path.empty()) {
            m.parse_error = "manifest file entry has an empty path";
            return m;
        }
        if (mf.provenance == Provenance::Unknown) {
            m.parse_error = "manifest file '" + mf.path +
                            "' has missing/unknown provenance";
            return m;
        }
        m.files.push_back(std::move(mf));
    }

    if (m.files.empty()) {
        m.parse_error = "manifest proposes no files";
        return m;
    }

    m.ok = true;
    return m;
}

namespace {

// A normalized, output-dir-relative path is safe when it neither is absolute
// nor traverses above the output dir via `..`.
bool is_safe_relative(const fs::path& rel) {
    if (rel.is_absolute()) return false;
    int depth = 0;
    for (const auto& part : rel) {
        const std::string s = part.string();
        if (s == "..") {
            if (--depth < 0) return false;
        } else if (s != "." && !s.empty()) {
            ++depth;
        }
    }
    return true;
}

}  // namespace

WritePlan compute_write_plan(const Manifest& manifest, const fs::path& output_dir) {
    WritePlan plan;
    if (!manifest.ok) {
        plan.error = manifest.parse_error.empty()
                         ? "manifest is not usable"
                         : manifest.parse_error;
        return plan;
    }

    fs::path out = output_dir.lexically_normal();
    for (const auto& f : manifest.files) {
        fs::path rel(f.path);
        if (!is_safe_relative(rel)) {
            plan.error = "manifest file path escapes the output dir: " + f.path;
            return plan;
        }

        WriteAction act;
        act.dest = (out / rel).lexically_normal();
        act.provenance = f.provenance;
        act.file = &f;

        if (f.provenance == Provenance::CopiedUserFile) {
            if (!f.has_copy_from || f.copy_from.empty()) {
                plan.error = "copied-user-file '" + f.path +
                             "' has no copy_from source";
                return plan;
            }
            if (!fs::path(f.copy_from).is_absolute()) {
                plan.error = "copy_from for '" + f.path +
                             "' must be an absolute source path";
                return plan;
            }
            act.is_copy = true;
            act.source = f.copy_from;
        } else {
            // generated / stub: inline content (may be legitimately empty).
            if (!f.has_content) {
                plan.error = "generated/stub file '" + f.path +
                             "' carries no content";
                return plan;
            }
            act.is_copy = false;
        }
        plan.actions.push_back(std::move(act));
    }

    plan.ok = true;
    return plan;
}

}  // namespace pulp::cli::import_emit
