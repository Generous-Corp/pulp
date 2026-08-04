#include "forge/module_catalog.hpp"

#include <choc/text/choc_JSON.h>

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace forge_modular {

namespace {

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
void load_installed(std::vector<ModuleEntry>& out) {
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
                    ModuleEntry e;
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
void load_catalogued(std::vector<ModuleEntry>& out) {
    const auto text = read_file(library_index_path());
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
                ModuleEntry e;
                e.brand = brand;
                e.slug = m["slug"].getWithDefault<std::string>("");
                e.name = m["name"].getWithDefault<std::string>(e.slug);
                if (!e.slug.empty()) out.push_back(std::move(e));
            }
        }
    } catch (...) {
    }
}

std::vector<ModuleEntry> load_all() {
    std::vector<ModuleEntry> v;
    load_installed(v);
    const auto installed_end = v.size();
    load_catalogued(v);
    // Drop a catalogued copy of something already installed: the same
    // module listed twice, once wireable and once not, is worse than
    // either alone.
    std::vector<ModuleEntry> out(v.begin(), v.begin() + installed_end);
    for (std::size_t i = installed_end; i < v.size(); ++i) {
        const auto& c = v[i];
        const bool dup = std::any_of(
            out.begin(), out.begin() + installed_end, [&](const ModuleEntry& e) {
                return e.slug == c.slug && e.brand == c.brand;
            });
        if (!dup) out.push_back(c);
    }
    return out;
}

/// Everything known, reloaded when the library index changes underneath us.
///
/// The index arrives AFTER launch on a machine that has never had one: the app
/// starts, sees no index, asks for one to be built, and a minute later 4,705
/// modules appear in a file this process already decided was empty. A cache
/// that never looked again meant the whole library stayed invisible until the
/// next relaunch -- the wiring being present and useless for the entire session
/// somebody would have spent judging it.
///
/// Keyed on the index's size and write time, which costs one stat per keystroke
/// and no parse at all when nothing has changed.
const std::vector<ModuleEntry>& all() {
    static std::vector<ModuleEntry> entries;
    static bool loaded = false;
    static std::uintmax_t seen_size = 0;
    static std::filesystem::file_time_type seen_time{};

    std::error_code ec;
    const auto path = std::filesystem::path(library_index_path());
    const auto size = std::filesystem::exists(path, ec)
                          ? std::filesystem::file_size(path, ec)
                          : 0;
    const auto time = std::filesystem::exists(path, ec)
                          ? std::filesystem::last_write_time(path, ec)
                          : std::filesystem::file_time_type{};
    if (!loaded || size != seen_size || time != seen_time) {
        entries = load_all();
        seen_size = size;
        seen_time = time;
        loaded = true;
    }
    return entries;
}

/// Case-folded, and with the separators people leave out removed.
///
/// A maker writes their name three ways at once: "CV funk" on the panel,
/// "CVfunk" in the plugin slug, "cv-funk" in a repository. Somebody typing @
/// picks whichever they remember, and all three have to land -- so the
/// comparison happens with the case and the separators taken out of it rather
/// than with an ever-growing table of spellings.
std::string fold(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        if (c == ' ' || c == '-' || c == '_') continue;
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool contains_fold(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    return fold(hay).find(fold(needle)) != std::string::npos;
}

bool starts_fold(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    return fold(hay).rfind(fold(needle), 0) == 0;
}

/// The model half of "Plugin/Model", which is the name people actually use.
std::string model_of(const std::string& slug) {
    const auto slash = slug.rfind('/');
    return slash == std::string::npos ? slug : slug.substr(slash + 1);
}

/// The plugin half of "Plugin/Model" -- the maker, spelled the way a slug does.
std::string plugin_of(const std::string& slug) {
    const auto slash = slug.rfind('/');
    return slash == std::string::npos ? std::string{} : slug.substr(0, slash);
}

/// How well a query matches, lower being better.
///
/// The order is exact, then prefix, then alias, then maker. Each tier exists
/// because of a specific list that came out wrong:
///
///   - MAKER LAST. Matching the brand as well as the name is what makes
///     "@CV funk" mean anything at all, but a maker with 50 modules will
///     otherwise bury the one module actually named in the query.
///   - ALIAS above CONTAINS. VCV's display names and the names people use are
///     often different words: "br" reaches Braids through the slug, and it has
///     to outrank every module with "br" somewhere in the middle of its name.
///   - EXACT above PREFIX. "@Dunes" is a whole name, and a module called
///     "Dunes" losing to one called "Dunestomper" is the query being ignored.
int match_rank(const ModuleEntry& e, const std::string& q) {
    const auto model = model_of(e.slug);
    if (fold(e.name) == fold(q) || fold(model) == fold(q)) return 0;
    if (starts_fold(e.name, q)) return 1;
    if (starts_fold(model, q)) return 2;               // alias: "br" -> Braids
    if (contains_fold(e.name, q)) return 3;
    if (contains_fold(model, q)) return 4;
    if (starts_fold(e.brand, q) || starts_fold(plugin_of(e.slug), q)) return 5;
    return 6;                                          // maker, somewhere in it
}

bool matches(const ModuleEntry& e, const std::string& q) {
    return contains_fold(e.name, q) || contains_fold(e.slug, q) ||
           contains_fold(e.brand, q);
}

/// Single-quote for /bin/sh. The one path involved has a space in it.
std::string shell_quoted(const std::string& text) {
    std::string out = "'";
    for (const char c : text) out += (c == '\'') ? std::string("'\\''")
                                                 : std::string(1, c);
    return out + "'";
}

}  // namespace

std::vector<MentionCandidate> search_entries(const std::vector<ModuleEntry>& entries,
                                             const std::string& query,
                                             std::size_t limit) {
    struct Scored { MentionCandidate c; int rank; std::size_t seq; };
    std::vector<Scored> hits;
    std::size_t seq = 0;
    for (const auto& e : entries) {
        if (!matches(e, query)) continue;
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

std::vector<MentionCandidate> search_modules(const std::string& query,
                                             std::size_t limit) {
    return search_entries(all(), query, limit);
}

CatalogCounts catalog_counts() {
    CatalogCounts n;
    for (const auto& e : all()) (e.installed ? n.installed : n.catalogued)++;
    return n;
}

std::string library_index_path() {
    return home_dir() +
           "/Library/Application Support/Forge Modular/library/index.json";
}

bool library_index_needs_build(const std::string& path, std::time_t now,
                               int max_age_days) {
    // stat rather than std::filesystem::last_write_time: the standard one
    // hands back a file_clock time point, and converting that to a wall clock
    // is not portable across the libc++ this builds against. The age of a file
    // does not need a clock conversion to be correct.
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return true;
    // An index that parsed as nothing is worse than none: it looks current and
    // offers nothing, which is exactly the state this whole path exists to end.
    if (st.st_size < 2) return true;
    return now - st.st_mtime >
           static_cast<std::time_t>(max_age_days) * 24 * 60 * 60;
}

std::string library_index_command(const std::string& tools_dir) {
    // Output kept, not discarded. A background job that fails silently is how
    // this file came to be read by something nothing ever wrote.
    const std::string runs = home_dir() +
        "/Library/Application Support/Forge Modular/runs";
    return "mkdir -p " + shell_quoted(runs) + " && cd " + shell_quoted(tools_dir) +
           " && python3 library_catalog.py index >> " +
           shell_quoted(runs + "/library.log") + " 2>&1";
}

std::string ensure_library_index(const std::string& tools_dir,
                                 const std::function<void(const std::string&)>& run,
                                 std::time_t now) {
    if (!library_index_needs_build(library_index_path(), now)) return {};
    const auto command = library_index_command(tools_dir);
    if (run) run(command);
    return command;
}

}  // namespace forge_modular
