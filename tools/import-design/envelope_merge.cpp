#include "envelope_merge.hpp"

#include <choc/text/choc_JSON.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_set>

namespace pulp::import_design {

namespace fs = std::filesystem;

namespace {

bool read_json(const fs::path& p, choc::value::Value& v) {
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
}

// Union `src` into `dst`, skipping entries whose `id_key` is already present.
// A null id_key keeps every entry (unkeyed observations). Returns an owning
// array — choc's operator[] hands back a non-owning ValueView, so the result has
// to be built up and set back by value.
choc::value::Value union_by(const choc::value::ValueView& dst,
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
                continue;   // same id → already merged
            out.addArrayElement(e);
        }
    };
    take(dst);
    take(src);
    return out;
}

}  // namespace

// Merging asset manifests is collision-safe by construction: every producer of
// this envelope keys an asset by an id that is unique-per-content within the
// design (the .fig decoder and the Figma plugin key by content hash; the REST
// export keys by Figma node id and names the file by content hash), so an id
// that repeats across states names the same asset and dedupes, while distinct
// content never shares an id. That is what lets every state's assets flatten
// into one `assets/` directory next to the merged envelope without rewriting a
// single local_path.
std::optional<int> merge_frame_envelopes(const std::vector<fs::path>& envelopes,
                                         const fs::path& scratch,
                                         const fs::path& out_path) {
    if (envelopes.empty()) {
        std::cerr << "Error: no envelopes to merge\n";
        return 3;
    }

    choc::value::Value merged;
    if (!read_json(envelopes.front(), merged)) {
        std::cerr << "Error: could not read design envelope " << envelopes.front() << "\n";
        return 3;
    }
    if (!merged.hasObjectMember("root")) {
        std::cerr << "Error: design envelope " << envelopes.front() << " has no root frame\n";
        return 3;
    }

    auto alternates = choc::value::createEmptyArray();
    for (std::size_t i = 1; i < envelopes.size(); ++i) {
        choc::value::Value env;
        if (!read_json(envelopes[i], env)) {
            std::cerr << "Error: could not read design envelope " << envelopes[i] << "\n";
            return 3;
        }
        if (!env.hasObjectMember("root")) {
            std::cerr << "Error: design envelope " << envelopes[i] << " has no root frame\n";
            return 3;
        }
        alternates.addArrayElement(env["root"]);

        // asset_manifest is an object wrapping the assets array.
        // A member that isn't there reads as an empty array, so a state that
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
        } else if (env.hasObjectMember("asset_manifest") && !merged.hasObjectMember("asset_manifest")) {
            merged.setMember("asset_manifest", choc::value::Value(env["asset_manifest"]));
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

    if (alternates.size() > 0) {
        choc::value::Value root(merged["root"]);
        root.setMember("alternate_frames", alternates);
        merged.setMember("root", root);
    }

    // Flatten every state's assets next to the merged envelope so each entry's
    // `assets/<hash>` local_path resolves from the merged file's directory.
    std::error_code ec;
    for (const auto& env_path : envelopes) {
        const fs::path src_assets = env_path.parent_path() / "assets";
        if (!fs::exists(src_assets, ec)) continue;
        if (fs::equivalent(src_assets, scratch / "assets", ec)) continue;  // already in place
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
    out.flush();
    if (!out) {
        std::cerr << "Error: could not write merged envelope " << out_path << "\n";
        return 3;
    }
    return std::nullopt;
}

fs::path make_scratch_dir(const std::string& tag, const std::string& input_file) {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path()
           / ("pulp-" + tag + "-" + fs::path(input_file).stem().string() + "-" +
              std::to_string(tick));
}

}  // namespace pulp::import_design
