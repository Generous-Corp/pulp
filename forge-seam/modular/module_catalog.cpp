#include "forge/module_catalog.hpp"

#include <choc/text/choc_JSON.h>

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
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
/// A module's name with the maker's own name taken off the front, when the
/// query is reaching for the MAKER rather than for this module.
///
/// VCV names very often lead with the maker: "CV funk Blank 4HP". Typing "@CV"
/// therefore prefix-matches that blank panel's NAME, and a blank panel led the
/// list for a query that plainly means the maker. The prefix it matched is not
/// this module's name, it is the brand repeated inside it.
///
/// Only when the query is itself a prefix of the brand. "@CVfunk Blank" is
/// longer than the brand and is genuinely reaching for that module, so it
/// ranks against the whole name.
std::string without_brand(const std::string& text, const std::string& brand,
                          const std::string& q) {
    if (brand.empty() || !starts_fold(text, brand)) return text;
    if (!starts_fold(brand, q)) return text;
    const auto brand_len = fold(brand).size();
    if (fold(q).size() > brand_len) return text;
    std::size_t taken = 0, i = 0;
    for (; i < text.size() && taken < brand_len; ++i)
        if (text[i] != ' ' && text[i] != '-' && text[i] != '_') ++taken;
    auto rest = text.substr(i);
    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '-'))
        rest.erase(rest.begin());
    return rest.empty() ? text : rest;
}

int match_rank(const ModuleEntry& e, const std::string& q) {
    // The SLUG repeats the maker just as often as the name does --
    // "CVfunk/CVfunkBlank4HP" -- so both halves are read with the maker taken
    // off the front, or the alias tier lets the same blank panel back in.
    const auto model = without_brand(model_of(e.slug), e.brand, q);
    const auto name = without_brand(e.name, e.brand, q);
    if (fold(name) == fold(q) || fold(model) == fold(q)) return 0;
    if (starts_fold(name, q)) return 1;
    if (starts_fold(model, q)) return 2;               // alias: "br" -> Braids
    if (contains_fold(name, q)) return 3;
    if (contains_fold(model, q)) return 4;
    if (starts_fold(e.brand, q) || starts_fold(plugin_of(e.slug), q)) return 5;
    return 6;                                          // maker, somewhere in it
}

bool matches(const ModuleEntry& e, const std::string& q) {
    return contains_fold(e.name, q) || contains_fold(e.slug, q) ||
           contains_fold(e.brand, q);
}

/// How well a query names a MAKER, on the same scale the modules are ranked
/// on, or no value when it does not name them at all.
///
/// The tiers are placed against the module tiers above, because the two kinds
/// of row are merged and one ordering decides both. Each is a specific list
/// that has to come out right:
///
///   - A WHOLE MAKER NAME, typed out, is unambiguous: "@Valley", "@CV funk",
///     "@Bogaudio". It sits at 1, under an exact module name and over
///     everything else, so `@Dunes` stays a module and `@Sphinx` stays the
///     module rather than becoming Sphinx Modular.
///   - A PARTIAL maker name sits at 3, under the alias tier, because two
///     letters are weak evidence of naming a vendor: "@br" means Braids, and
///     a maker called Bruer must not take that row. It still beats every
///     module that matched only through its own brand, which is the whole of
///     "@CV": fifty rows that all say CV funk in the first column, and no way
///     to say "that maker" in one gesture.
std::optional<int> brand_rank(const std::string& brand, const std::string& q) {
    if (fold(brand) == fold(q)) return 1;
    if (starts_fold(brand, q)) return 3;
    if (contains_fold(brand, q)) return 4;
    return std::nullopt;
}

/// At most this many maker rows. The list is for choosing a maker, not for
/// browsing every maker whose name contains two letters.
constexpr std::size_t kMaxBrandRows = 3;

/// Shorter than this and a query names nothing in particular: "@" alone opens
/// the whole library, and every maker prefix-matches an empty string.
constexpr std::size_t kMinBrandQuery = 2;

/// Single-quote for /bin/sh. The one path involved has a space in it.
std::string shell_quoted(const std::string& text) {
    std::string out = "'";
    for (const char c : text) out += (c == '\'') ? std::string("'\\''")
                                                 : std::string(1, c);
    return out + "'";
}

}  // namespace

std::string module_label(const std::string& brand, const std::string& name) {
    if (brand.empty()) return name;
    if (name.empty()) return brand;
    // Folded, so "CVfunk Blank 8HP" and "CV funk Blank 8HP" are both already
    // led by the maker: the slug and the panel spell the same name three ways
    // and only one of them matches character for character.
    if (starts_fold(name, brand)) return name;
    return brand + " " + name;
}

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

    // The makers, as rows of their own. A maker with 50 modules could only be
    // reached 50 rows at a time, and the one gesture a person actually wants
    // -- "that maker" -- did not exist.
    if (fold(query).size() >= kMinBrandQuery) {
        struct Maker { std::size_t count; std::size_t first; };
        std::vector<std::pair<std::string, Maker>> makers;
        for (const auto& e : entries) {
            if (e.brand.empty()) continue;
            auto it = std::find_if(makers.begin(), makers.end(),
                                   [&](const auto& kv) { return kv.first == e.brand; });
            if (it == makers.end())
                makers.push_back({e.brand, Maker{1, makers.size()}});
            else
                ++it->second.count;
        }
        std::vector<Scored> brands;
        for (const auto& [brand, maker] : makers) {
            const auto rank = brand_rank(brand, query);
            if (!rank) continue;
            MentionCandidate c;
            c.brand = brand;
            c.name = std::to_string(maker.count) +
                     (maker.count == 1 ? " module" : " modules");
            // What gets inserted is the MAKER'S OWN NAME. It is not a slug and
            // deliberately not a syntax: the prompt reader matches makers by
            // name whether they arrived as a token or as ordinary words, so an
            // inserted "@CV funk" and a typed "modules from CV funk" are one
            // behaviour with one thing to reason about -- and a project
            // reloaded next week still means what it meant.
            c.slug = brand;
            c.state = MentionCandidate::Availability::ready;
            c.kind = MentionCandidate::Kind::brand;
            brands.push_back({std::move(c), *rank, maker.first});
        }
        std::sort(brands.begin(), brands.end(), [](const Scored& a, const Scored& b) {
            if (a.rank != b.rank) return a.rank < b.rank;
            return a.seq < b.seq;
        });
        if (brands.size() > kMaxBrandRows) brands.resize(kMaxBrandRows);
        hits.insert(hits.end(), std::make_move_iterator(brands.begin()),
                    std::make_move_iterator(brands.end()));
    }

    // The FRONT of the list, then how well the query matched, then the order
    // the catalogue holds them, so the list does not reshuffle between
    // keystrokes.
    //
    // Three things earn the front tier, and each is a list that came out
    // wrong without it:
    //
    //   - INSTALLED. Only these can be wired into a patch that will sound, so
    //     they lead. This was the whole rule once.
    //   - AN EXACT NAME. A whole module name typed out is the strongest
    //     evidence there is and it does not get weaker for not being here
    //     yet: "@Sphinx" answered "Sphinx Modular, 2 modules" while the
    //     module actually called Sphinx sat below it, purely because it was
    //     not installed.
    //   - A MAKER. A maker row can always be picked, and burying it under
    //     fifty of that maker's own installed modules is what it was added to
    //     fix. Its own rank still places it: a whole maker name typed out
    //     sits at 1 and a partial one at 3, under the alias tier, so "@br"
    //     stays Braids and does not become Bruer.
    //
    // Then a MODULE WINS A TIE with a maker, so the narrower reading leads.
    auto front = [](const Scored& s) {
        return s.c.kind == MentionCandidate::Kind::brand || s.rank == 0 ||
               s.c.state == MentionCandidate::Availability::ready;
    };
    std::sort(hits.begin(), hits.end(), [&](const Scored& a, const Scored& b) {
        const bool af = front(a);
        const bool bf = front(b);
        if (af != bf) return af;
        if (a.rank != b.rank) return a.rank < b.rank;
        const bool ab = a.c.kind == MentionCandidate::Kind::brand;
        const bool bb = b.c.kind == MentionCandidate::Kind::brand;
        if (ab != bb) return bb;
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

IndexCounts count_index_text(const std::string& text) {
    IndexCounts n;
    if (text.size() < 2) return n;
    try {
        const auto doc = choc::json::parse(text);
        n.plugins = doc.size();
        for (uint32_t i = 0; i < doc.size(); ++i) {
            const auto plug = doc[i];
            if (plug.hasObjectMember("modules")) n.modules += plug["modules"].size();
        }
    } catch (...) {
        // Unparseable counts as nothing, which is the honest answer and the
        // one that provokes a rebuild.
        return IndexCounts{};
    }
    return n;
}

bool index_is_plausible(const IndexCounts& counts) {
    return counts.plugins >= kMinPlausiblePlugins &&
           counts.modules >= kMinPlausibleModules;
}

LibraryIndexState library_index_state() {
    static LibraryIndexState state;
    static std::uintmax_t seen_size = 0;
    static std::time_t seen_time = 0;

    const auto path = library_index_path();
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        state = LibraryIndexState{};
        seen_size = 0;
        seen_time = 0;
        return state;
    }
    if (state.present && st.st_size == static_cast<off_t>(seen_size) &&
        st.st_mtime == seen_time)
        return state;
    state.present = true;
    state.counts = count_index_text(read_file(path));
    state.written = st.st_mtime;
    seen_size = static_cast<std::uintmax_t>(st.st_size);
    seen_time = st.st_mtime;
    return state;
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
    // PLAUSIBILITY, not just age. A 200-plugin file with no CV funk in it was
    // four days old and therefore "fresh", and it is what the whole @ list and
    // every prompt inventory was built from. Size on disk is not the test:
    // count what is actually in it.
    if (!index_is_plausible(count_index_text(read_file(path)))) return true;
    return now - st.st_mtime >
           static_cast<std::time_t>(max_age_days) * 24 * 60 * 60;
}

std::string library_index_status_path() {
    return home_dir() +
           "/Library/Application Support/Forge Modular/runs/library-status";
}

std::string library_index_command(const std::string& tools_dir) {
    // Output kept, not discarded, AND THE EXIT STATUS RECORDED. A background
    // job that fails silently is how this file came to be read by something
    // nothing ever wrote -- and then, once it was written, how a toolchain too
    // old to understand `index` printed its usage into the log and exited 2
    // for four days without a word reaching the app.
    //
    // Wrapped in a subshell so that a caller appending `&` backgrounds the
    // WHOLE sequence. Without the parentheses the `&` would attach to the
    // final command alone and the fetch would run in the foreground of
    // whatever launched it.
    const std::string runs = home_dir() +
        "/Library/Application Support/Forge Modular/runs";
    // The old status goes first, so "no status file" means a build is in
    // flight rather than "the last one is still whatever it was".
    return "( mkdir -p " + shell_quoted(runs) + " && rm -f " +
           shell_quoted(library_index_status_path()) + " && cd " +
           shell_quoted(tools_dir) +
           " && python3 library_catalog.py index >> " +
           shell_quoted(runs + "/library.log") + " 2>&1; printf '%s' \"$?\" > " +
           shell_quoted(library_index_status_path()) + " )";
}

std::optional<int> library_index_last_status() {
    const auto text = read_file(library_index_status_path());
    if (text.empty()) return std::nullopt;
    try {
        return std::stoi(text);
    } catch (...) {
        return std::nullopt;
    }
}

std::string ensure_library_index(const std::string& tools_dir,
                                 const std::function<void(const std::string&)>& run,
                                 std::time_t now) {
    if (!library_index_needs_build(library_index_path(), now)) return {};
    return build_library_index(tools_dir, run);
}

std::string build_library_index(const std::string& tools_dir,
                                const std::function<void(const std::string&)>& run) {
    const auto command = library_index_command(tools_dir);
    if (run) run(command);
    return command;
}

}  // namespace forge_modular
