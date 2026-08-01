#include "forge/module_catalog.hpp"

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace forge_modular {

namespace {

struct Entry {
    std::string brand, name, slug;
    bool installed = false;
};

std::string home_dir() {
    const char* h = std::getenv("HOME");
    return h ? h : ".";
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream f(p);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// Modules whose plugin is installed in Rack. These can be wired.
void load_installed(std::vector<Entry>& out) {
    const std::filesystem::path base =
        home_dir() + "/Library/Application Support/Rack2";
    std::error_code ec;
    for (const auto& dir : std::filesystem::directory_iterator(base, ec)) {
        if (!dir.is_directory() ||
            dir.path().filename().string().rfind("plugins", 0) != 0)
            continue;
        for (const auto& plug : std::filesystem::directory_iterator(dir.path(), ec)) {
            if (!plug.is_directory()) continue;
            const auto text = read_file(plug.path() / "plugin.json");
            if (text.empty()) continue;
            try {
                const auto doc = choc::json::parse(text);
                const auto brand = doc["brand"].getWithDefault<std::string>(
                    doc["name"].getWithDefault<std::string>(""));
                if (!doc.hasObjectMember("modules")) continue;
                const auto ms = doc["modules"];
                for (uint32_t i = 0; i < ms.size(); ++i) {
                    const auto m = ms[i];
                    Entry e;
                    e.brand = brand;
                    e.slug = m["slug"].getWithDefault<std::string>("");
                    e.name = m["name"].getWithDefault<std::string>(e.slug);
                    e.installed = true;
                    if (!e.slug.empty()) out.push_back(std::move(e));
                }
            } catch (...) {
                // One unreadable manifest must not cost the whole list.
            }
        }
    }
}

/// Modules merely published. Cannot be wired, but can be named and looked up.
void load_catalogued(std::vector<Entry>& out) {
    const auto text = read_file(
        home_dir() + "/Library/Application Support/Forge Modular/library/index.json");
    if (text.empty()) return;
    try {
        const auto doc = choc::json::parse(text);
        for (uint32_t i = 0; i < doc.size(); ++i) {
            const auto plug = doc[i];
            const auto brand = plug["brand"].getWithDefault<std::string>("");
            if (!plug.hasObjectMember("modules")) continue;
            const auto ms = plug["modules"];
            for (uint32_t j = 0; j < ms.size(); ++j) {
                const auto m = ms[j];
                Entry e;
                e.brand = brand;
                e.slug = m["slug"].getWithDefault<std::string>("");
                e.name = m["name"].getWithDefault<std::string>(e.slug);
                if (!e.slug.empty()) out.push_back(std::move(e));
            }
        }
    } catch (...) {
    }
}

const std::vector<Entry>& all() {
    static const std::vector<Entry> entries = [] {
        std::vector<Entry> v;
        load_installed(v);
        const auto installed_end = v.size();
        load_catalogued(v);
        // Drop a catalogued copy of something already installed: the same
        // module listed twice, once wireable and once not, is worse than
        // either alone.
        std::vector<Entry> out(v.begin(), v.begin() + installed_end);
        for (std::size_t i = installed_end; i < v.size(); ++i) {
            const auto& c = v[i];
            const bool dup = std::any_of(
                out.begin(), out.begin() + installed_end, [&](const Entry& e) {
                    return e.slug == c.slug && e.brand == c.brand;
                });
            if (!dup) out.push_back(c);
        }
        return out;
    }();
    return entries;
}

bool contains_fold(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    auto lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(c));
        return s;
    };
    return lower(hay).find(lower(needle)) != std::string::npos;
}

}  // namespace

namespace {

/// The model half of "Plugin/Model", which is the name people actually use.
std::string model_of(const std::string& slug) {
    const auto slash = slug.rfind('/');
    return slash == std::string::npos ? slug : slug.substr(slash + 1);
}

/// How well a query matches, lower being better.
///
/// Insertion order alone put "Calibrator" and "Macro Oscillator" above
/// "Breakout" for the query "br" — every one a real match, none of them the
/// one anybody meant. What a person types is nearly always the start of the
/// name they are reaching for, so that ranks first.
int match_rank(const Entry& e, const std::string& q) {
    const auto starts = [&](const std::string& hay) {
        return hay.size() >= q.size() && contains_fold(hay.substr(0, q.size()), q);
    };
    if (starts(e.name)) return 0;
    if (starts(model_of(e.slug))) return 1;
    if (contains_fold(e.name, q)) return 2;
    if (contains_fold(e.slug, q)) return 3;
    return 4;                                   // brand only
}

}  // namespace

std::vector<MentionCandidate> search_modules(const std::string& query,
                                             std::size_t limit) {
    struct Scored { MentionCandidate c; int rank; std::size_t seq; };
    std::vector<Scored> hits;
    std::size_t seq = 0;
    for (const auto& e : all()) {
        if (!contains_fold(e.name, query) && !contains_fold(e.slug, query) &&
            !contains_fold(e.brand, query))
            continue;
        MentionCandidate c;
        c.brand = e.brand;
        c.name = e.name;
        c.slug = e.slug;
        // Say WHY it matched when the reason is invisible. "Macro Oscillator"
        // answering "br" looks wrong until you know it is Braids.
        const auto model = model_of(e.slug);
        if (!query.empty() && !contains_fold(e.name, query) &&
            contains_fold(model, query) && !contains_fold(e.name, model))
            c.alias = model;
        c.state = e.installed ? MentionCandidate::Availability::ready
                              : MentionCandidate::Availability::available;
        hits.push_back({std::move(c), match_rank(e, query), seq++});
    }
    // Installed first — only those can be wired into a patch that will sound —
    // then by how well the query matched, then by the order the catalogue
    // holds them, so the list does not reshuffle between keystrokes.
    std::sort(hits.begin(), hits.end(), [](const Scored& a, const Scored& b) {
        const bool ar = a.c.state == MentionCandidate::Availability::ready;
        const bool br = b.c.state == MentionCandidate::Availability::ready;
        if (ar != br) return ar;
        if (a.rank != b.rank) return a.rank < b.rank;
        return a.seq < b.seq;
    });
    std::vector<MentionCandidate> out;
    for (auto& h : hits) {
        if (out.size() >= limit) break;
        out.push_back(std::move(h.c));
    }
    return out;
}

CatalogCounts catalog_counts() {
    CatalogCounts n;
    for (const auto& e : all()) (e.installed ? n.installed : n.catalogued)++;
    return n;
}

}  // namespace forge_modular
