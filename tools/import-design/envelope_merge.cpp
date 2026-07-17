#include "envelope_merge.hpp"

#include <choc/text/choc_JSON.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>

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
// to be built up and set back by value. Returns nullopt (and sets `err`) on an
// id collision that dedupe would resolve WRONGLY; see below.
//
// Dedupe-on-id is only safe while an id names its content. The `.fig` decoder
// and the Figma plugin key an asset BY its content hash, so it always does
// there. The REST export keys by Figma node id and carries the content hash as
// a separate `content_hash` field — so re-exporting one node after a design
// edit yields two entries sharing an asset_id with different bytes, and
// keeping the first would silently render the earlier state's artwork in the
// later state's frame. A wrong pixel with no diagnostic is the worst outcome
// available here, and the evidence to prevent it is already in the entry: when
// both sides carry a content_hash and the hashes disagree, fail.
//
// Absent hashes fall through to dedupe rather than erroring. That is not
// laxity — an entry without a content_hash is the hash-keyed lane, where the
// asset_id IS the hash and a repeat is provably the same content.
std::optional<choc::value::Value> union_by(const choc::value::ValueView& dst,
                                           const choc::value::ValueView& src,
                                           const char* id_key,
                                           const char* what,
                                           std::string& err) {
    auto out = choc::value::createEmptyArray();
    std::unordered_map<std::string, std::string> seen;  // id -> content_hash ("" = absent)
    auto content_hash_of = [](const choc::value::ValueView& e) -> std::string {
        return (e.isObject() && e.hasObjectMember("content_hash"))
                   ? e["content_hash"].toString()
                   : std::string{};
    };
    bool failed = false;
    auto take = [&](const choc::value::ValueView& arr) {
        if (failed || !arr.isArray()) return;
        for (uint32_t j = 0; j < arr.size(); ++j) {
            const auto e = arr[static_cast<int>(j)];
            if (id_key && e.isObject() && e.hasObjectMember(id_key)) {
                const auto id = e[id_key].toString();
                const auto hash = content_hash_of(e);
                const auto [it, fresh] = seen.emplace(id, hash);
                if (!fresh) {
                    if (!hash.empty() && !it->second.empty() && hash != it->second) {
                        err = std::string("two states carry a different ") + what +
                              " under the same " + id_key + " \"" + id +
                              "\" (content_hash " + it->second.substr(0, 12) + "... vs " +
                              hash.substr(0, 12) +
                              "...). These states cannot share one flattened assets/ "
                              "directory: merging would keep the first and silently render "
                              "it in every state that reuses the id. This is what a "
                              "node-id-keyed export does when a node's content changes "
                              "between captures — re-export the states from one design "
                              "revision, or capture them from a content-hash-keyed source.";
                        failed = true;
                        return;
                    }
                    continue;   // same id, same (or unknowable) content → already merged
                }
            }
            out.addArrayElement(e);
        }
    };
    take(dst);
    take(src);
    if (failed) return std::nullopt;
    return out;
}

}  // namespace

// Merging asset manifests flattens every state's assets into one `assets/`
// directory next to the merged envelope, without rewriting a single
// local_path. That works only while an asset_id names its content, which is
// true of the hash-keyed producers (the .fig decoder and the Figma plugin key
// by content hash) but NOT of the REST export, which keys by Figma node id and
// carries the hash separately. So collision-safety is checked, not assumed:
// union_by fails on a repeated id whose content_hash disagrees rather than
// dedupe-ing two different pictures into one.
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

        // An id collision that dedupe would resolve wrongly names the offending
        // envelope, not just the id — with N states merging, "which capture do I
        // re-export?" is the whole question.
        std::string err;
        auto fail = [&](const fs::path& p) {
            std::cerr << "Error: cannot merge design envelope " << p << ": " << err << "\n";
            return 3;
        };

        if (env.hasObjectMember("asset_manifest") && env["asset_manifest"].isObject() &&
            merged.hasObjectMember("asset_manifest") && merged["asset_manifest"].isObject()) {
            choc::value::Value manifest(merged["asset_manifest"]);
            auto assets = union_by(array_member(manifest, "assets"),
                                   array_member(env["asset_manifest"], "assets"),
                                   "asset_id", "asset", err);
            if (!assets) return fail(envelopes[i]);
            manifest.setMember("assets", *assets);
            merged.setMember("asset_manifest", manifest);
        } else if (env.hasObjectMember("asset_manifest") && !merged.hasObjectMember("asset_manifest")) {
            merged.setMember("asset_manifest", choc::value::Value(env["asset_manifest"]));
        }
        if (env.hasObjectMember("font_family_assets")) {
            auto fonts = union_by(array_member(merged, "font_family_assets"),
                                  array_member(env, "font_family_assets"),
                                  "asset_id", "font asset", err);
            if (!fonts) return fail(envelopes[i]);
            merged.setMember("font_family_assets", *fonts);
        }
        // Diagnostics are observations, not keyed records — keep every one.
        if (env.hasObjectMember("diagnostics")) {
            auto diags = union_by(array_member(merged, "diagnostics"),
                                  array_member(env, "diagnostics"), nullptr, "diagnostic", err);
            if (!diags) return fail(envelopes[i]);   // unkeyed: cannot collide
            merged.setMember("diagnostics", *diags);
        }
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
