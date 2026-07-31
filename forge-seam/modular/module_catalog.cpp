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

std::vector<MentionCandidate> search_modules(const std::string& query,
                                             std::size_t limit) {
    std::vector<MentionCandidate> ready, rest;
    for (const auto& e : all()) {
        if (!contains_fold(e.name, query) && !contains_fold(e.slug, query) &&
            !contains_fold(e.brand, query))
            continue;
        MentionCandidate c;
        c.brand = e.brand;
        c.name = e.name;
        c.slug = e.slug;
        c.state = e.installed ? MentionCandidate::Availability::ready
                              : MentionCandidate::Availability::available;
        (e.installed ? ready : rest).push_back(std::move(c));
        if (ready.size() >= limit) break;
    }
    // Installed first: only those can be wired into a patch that will sound.
    for (auto& c : rest) {
        if (ready.size() >= limit) break;
        ready.push_back(std::move(c));
    }
    return ready;
}

CatalogCounts catalog_counts() {
    CatalogCounts n;
    for (const auto& e : all()) (e.installed ? n.installed : n.catalogued)++;
    return n;
}

}  // namespace forge_modular
