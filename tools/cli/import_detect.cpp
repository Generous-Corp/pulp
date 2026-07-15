// SPDX-License-Identifier: MIT
#include "import_detect.hpp"
#include "json_parser.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>

namespace pulp::cli::import_detect {

using pulp::cli::pkg::JsonParser;
using pulp::cli::pkg::JsonValue;

// ── Index parsing ──

KnownFrameworks parse_index(const std::string& json_text) {
    KnownFrameworks out;
    if (json_text.empty()) {
        out.error = "known-frameworks index is empty";
        return out;
    }
    JsonParser parser{json_text};
    JsonValue root = parser.parse();
    if (root.type != JsonValue::Object) {
        out.error = "known-frameworks index is not a JSON object";
        return out;
    }
    const JsonValue* arr = root.get("frameworks");
    if (!arr || arr->type != JsonValue::Array) {
        out.error = "known-frameworks index has no 'frameworks' array";
        return out;
    }
    for (auto& fw : arr->arr()) {
        if (fw.type != JsonValue::Object) continue;
        FrameworkEntry e;
        if (auto v = fw.get("framework_id")) e.framework_id = v->as_string();
        if (auto v = fw.get("display_name")) e.display_name = v->as_string();
        if (auto v = fw.get("importer_tool_id")) e.importer_tool_id = v->as_string();
        if (auto v = fw.get("spi_min")) e.spi_min = v->as_int();
        if (auto v = fw.get("spi_max")) e.spi_max = v->as_int();
        if (auto det = fw.get("detection"); det && det->type == JsonValue::Array) {
            for (auto& m : det->arr()) {
                if (m.type != JsonValue::Object) continue;
                Marker marker;
                std::string ty;
                if (auto v = m.get("type")) ty = v->as_string();
                marker.type = (ty == "content_match") ? MarkerType::ContentMatch
                                                       : MarkerType::FileGlob;
                if (auto v = m.get("pattern")) marker.pattern = v->as_string();
                if (auto v = m.get("in_glob")) marker.in_glob = v->as_string();
                if (auto v = m.get("weight"); v && v->type == JsonValue::Number)
                    marker.weight = v->num_val;
                e.detection.push_back(std::move(marker));
            }
        }
        if (!e.framework_id.empty()) out.frameworks.push_back(std::move(e));
    }
    return out;
}

KnownFrameworks load_index(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        KnownFrameworks out;
        out.error = "cannot read known-frameworks index: " + path.string();
        return out;
    }
    std::string text((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    return parse_index(text);
}

KnownFrameworks merge_installed_indices(const fs::path& tools_root) {
    KnownFrameworks merged;

    std::error_code ec;
    if (!fs::is_directory(tools_root, ec)) return merged;   // nothing installed: nothing known

    // Deterministic order, so two machines with the same tools installed rank
    // candidates identically.
    // An importer installs to ~/.pulp/tools/<tool_id>/ or, when versioned, to
    // ~/.pulp/tools/<tool_id>/<version>/. Look for its index at both depths rather
    // than assuming one, so a versioned install is not silently invisible.
    std::vector<fs::path> indices;
    auto note = [&](const fs::path& dir) {
        auto candidate = dir / "known-frameworks.json";
        if (fs::exists(candidate)) indices.push_back(candidate);
    };
    for (const auto& tool : fs::directory_iterator(tools_root, ec)) {
        if (ec) break;
        if (!tool.is_directory()) continue;
        note(tool.path());
        std::error_code inner;
        for (const auto& ver : fs::directory_iterator(tool.path(), inner)) {
            if (inner) break;
            if (ver.is_directory()) note(ver.path());
        }
    }
    std::sort(indices.begin(), indices.end());

    for (const auto& idx : indices) {
        auto one = load_index(idx);
        if (!one.error.empty()) {
            // A broken index from ONE tool must not blind detection to the others,
            // but it must not pass silently either.
            if (!merged.error.empty()) merged.error += "; ";
            merged.error += idx.string() + ": " + one.error;
            continue;
        }
        for (auto& fw : one.frameworks) merged.frameworks.push_back(std::move(fw));
    }
    return merged;
}

fs::path find_index(const fs::path& start_dir, const fs::path& exe_dir) {
    if (const char* env = std::getenv("PULP_KNOWN_FRAMEWORKS")) {
        if (env[0] != '\0') return fs::path(env);
    }
    const fs::path rel = fs::path("tools") / "import" / "known-frameworks.json";
    auto walk_up = [&](fs::path dir) -> fs::path {
        std::error_code ec;
        dir = fs::weakly_canonical(dir, ec);
        if (ec) dir = start_dir;
        while (true) {
            auto candidate = dir / rel;
            if (fs::exists(candidate)) return candidate;
            if (dir.has_parent_path() && dir.parent_path() != dir)
                dir = dir.parent_path();
            else break;
        }
        return {};
    };
    if (auto p = walk_up(start_dir); !p.empty()) return p;
    if (!exe_dir.empty()) {
        if (auto p = walk_up(exe_dir); !p.empty()) return p;
    }
    return {};
}

// ── Glob matching ──

namespace {

// Recursive glob matcher. Tokens:
//   '**' — any run of characters including '/'. A '**/' prefix also matches
//          zero directories (so '**/x' matches 'x' and 'a/b/x').
//   '*'  — any run of characters NOT crossing '/'.
//   '?'  — exactly one character that is not '/'.
// Anchored to the whole path.
bool glob_rec(const char* g, const char* gend, const char* p, const char* pend) {
    while (g < gend) {
        if (*g == '*') {
            bool dbl = (g + 1 < gend && g[1] == '*');
            const char* gnext = g + (dbl ? 2 : 1);
            if (dbl) {
                // Allow "**/" to also match zero segments: try skipping the
                // optional following '/'.
                if (gnext < gend && *gnext == '/') {
                    if (glob_rec(gnext + 1, gend, p, pend)) return true;
                }
                // '**' consumes any number of chars (including '/').
                for (const char* q = p; q <= pend; ++q) {
                    if (glob_rec(gnext, gend, q, pend)) return true;
                }
                return false;
            }
            // single '*': consume chars up to (not including) next '/'.
            for (const char* q = p; q <= pend; ++q) {
                if (glob_rec(gnext, gend, q, pend)) return true;
                if (q < pend && *q == '/') break;  // can't cross '/'
            }
            return false;
        }
        if (p >= pend) return false;
        if (*g == '?') {
            if (*p == '/') return false;
            ++g; ++p; continue;
        }
        if (*g != *p) return false;
        ++g; ++p;
    }
    return p == pend;
}

}  // namespace

bool glob_match(const std::string& glob, const std::string& path) {
    return glob_rec(glob.data(), glob.data() + glob.size(),
                    path.data(), path.data() + path.size());
}

// ── Scanning ──

namespace {

bool should_skip_dir(const std::string& name) {
    static const char* skip[] = {
        ".git", "build", "Build", "node_modules", ".cache", "external",
        "DerivedData", "Pods", "vendor", ".svn", "dist", "out",
        "cmake-build-debug", "cmake-build-release",
    };
    for (auto* s : skip) if (name == s) return true;
    return false;
}

std::string read_capped(const fs::path& p, size_t cap) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string buf;
    buf.resize(cap);
    f.read(buf.data(), static_cast<std::streamsize>(cap));
    buf.resize(static_cast<size_t>(f.gcount()));
    return buf;
}

}  // namespace

std::vector<Candidate> detect(const fs::path& project_dir,
                              const KnownFrameworks& index) {
    std::vector<Candidate> out;
    std::error_code ec;
    if (!fs::exists(project_dir, ec) || !fs::is_directory(project_dir, ec))
        return out;

    // Collect relative file paths once (bounded), then evaluate markers.
    std::vector<std::string> rel_paths;
    constexpr size_t kMaxFiles = 20000;
    constexpr size_t kReadCap = 256 * 1024;  // 256 KB per file for content match

    fs::path base = fs::weakly_canonical(project_dir, ec);
    if (ec) base = project_dir;

    for (auto it = fs::recursive_directory_iterator(
             base, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const auto& entry = *it;
        if (entry.is_directory(ec)) {
            if (should_skip_dir(entry.path().filename().string()))
                it.disable_recursion_pending();
            continue;
        }
        if (!entry.is_regular_file(ec)) continue;
        auto rel = fs::relative(entry.path(), base, ec);
        if (ec) { ec.clear(); continue; }
        std::string rp = rel.generic_string();
        rel_paths.push_back(rp);
        if (rel_paths.size() >= kMaxFiles) break;
    }

    for (const auto& fw : index.frameworks) {
        double total_weight = 0.0;
        double hit_weight = 0.0;
        std::vector<std::string> evidence;

        for (const auto& m : fw.detection) {
            total_weight += m.weight;
            bool hit = false;
            std::string hit_path;

            if (m.type == MarkerType::FileGlob) {
                for (const auto& rp : rel_paths) {
                    if (glob_match(m.pattern, rp)) { hit = true; hit_path = rp; break; }
                }
                if (hit)
                    evidence.push_back("file matches '" + m.pattern + "' (" + hit_path + ")");
            } else {  // ContentMatch
                for (const auto& rp : rel_paths) {
                    if (!m.in_glob.empty() && !glob_match(m.in_glob, rp)) continue;
                    std::string content = read_capped(base / rp, kReadCap);
                    if (content.find(m.pattern) != std::string::npos) {
                        hit = true; hit_path = rp; break;
                    }
                }
                if (hit)
                    evidence.push_back("'" + m.pattern + "' found in " + hit_path);
            }

            if (hit) hit_weight += m.weight;
        }

        if (hit_weight <= 0.0) continue;
        Candidate c;
        c.framework_id = fw.framework_id;
        c.display_name = fw.display_name;
        c.importer_tool_id = fw.importer_tool_id;
        c.spi_min = fw.spi_min;
        c.spi_max = fw.spi_max;
        c.confidence = total_weight > 0.0
                           ? std::min(1.0, hit_weight / total_weight)
                           : 0.0;
        c.evidence = std::move(evidence);
        out.push_back(std::move(c));
    }

    std::sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) {
        if (a.confidence != b.confidence) return a.confidence > b.confidence;
        return a.framework_id < b.framework_id;
    });
    return out;
}

}  // namespace pulp::cli::import_detect
