#include <pulp/state/preset_library.hpp>

#include <pulp/platform/special_dir.hpp>
#include <pulp/runtime/crypto.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

namespace fs = std::filesystem;

namespace pulp::state {

namespace {

std::string lower_ascii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool is_word_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           static_cast<unsigned char>(c) >= 0x80;
}

std::vector<std::string> split_words(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!cur.empty())
                out.push_back(std::move(cur)), cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty())
        out.push_back(std::move(cur));
    return out;
}

bool contains_tag(const std::vector<std::string>& tags, const std::string& tag) {
    const auto want = lower_ascii(tag);
    return std::any_of(tags.begin(), tags.end(),
                       [&](const std::string& t) { return lower_ascii(t) == want; });
}

// Relative path of `p` below `base` ("" when p is not below base).
bool relative_below(const fs::path& base, const fs::path& p, fs::path& rel) {
    if (base.empty())
        return false;
    const auto b = base.lexically_normal();
    const auto n = p.lexically_normal();
    auto r = n.lexically_relative(b);
    if (r.empty() || *r.begin() == "..")
        return false;
    rel = std::move(r);
    return true;
}

std::string without_extension(const fs::path& rel) {
    fs::path r = rel;
    r.replace_extension();
    return r.generic_string();
}

// <data root>/Content/<plugin>/<pack>/<version>/presets/<relative path>.
// The version is left out of the key so an updated pack keeps its edits.
bool parse_content_path(const fs::path& p, std::string& pack, fs::path& rel) {
    std::vector<std::string> parts;
    for (const auto& c : p.lexically_normal())
        parts.push_back(c.string());
    for (std::size_t i = 0; i + 5 < parts.size(); ++i) {
        if (parts[i] == "Content" && parts[i + 4] == "presets") {
            pack = parts[i + 2];
            fs::path r;
            for (std::size_t k = i + 5; k < parts.size(); ++k)
                r /= parts[k];
            rel = std::move(r);
            return true;
        }
    }
    return false;
}

// Everything before the preset's own path: "factory:", "user:" or
// "content:<pack>:".
std::string key_scope(const std::string& key) {
    const auto first = key.find(':');
    if (first == std::string::npos)
        return {};
    if (key.compare(0, first, "content") == 0) {
        const auto second = key.find(':', first + 1);
        return second == std::string::npos ? key.substr(0, first + 1) : key.substr(0, second + 1);
    }
    return key.substr(0, first + 1);
}

std::string sanitize_component(std::string value) {
    for (char& c : value)
        if (c == '/' || c == '\\' || c == ':' || c == '\0' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|')
            c = '_';
    return value.empty() ? "_" : value;
}

int source_rank(PresetSource s) {
    switch (s) {
    case PresetSource::factory:
        return 0;
    case PresetSource::content:
        return 1;
    case PresetSource::user:
        return 2;
    }
    return 3;
}

} // namespace

std::string make_preset_key(PresetSource source, std::string_view relative_path,
                            std::string_view pack_id) {
    std::string key;
    switch (source) {
    case PresetSource::factory:
        key = "factory:";
        break;
    case PresetSource::user:
        key = "user:";
        break;
    case PresetSource::content:
        key = "content:";
        key += pack_id;
        key += ":";
        break;
    }
    key += relative_path;
    return key;
}

std::string preset_digest(const fs::path& file) {
    std::ifstream in(file);
    if (!in.is_open())
        return {};
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    try {
        auto doc = choc::json::parse(body);
        if (doc.isObject() && doc.hasObjectMember("parameters") && doc["parameters"].isObject()) {
            auto params = doc["parameters"];
            std::vector<std::pair<std::string, double>> values;
            for (std::uint32_t i = 0; i < params.size(); ++i) {
                auto m = params.getObjectMemberAt(i);
                double v = 0.0;
                if (m.value.isInt32())
                    v = m.value.getInt32();
                else if (m.value.isInt64())
                    v = static_cast<double>(m.value.getInt64());
                else if (m.value.isFloat32())
                    v = m.value.getFloat32();
                else if (m.value.isFloat64())
                    v = m.value.getFloat64();
                else
                    continue;
                values.emplace_back(std::string(m.name), v);
            }
            std::sort(values.begin(), values.end());
            std::string canon;
            char buf[64];
            for (const auto& [name, v] : values) {
                std::snprintf(buf, sizeof buf, "%.9g", v);
                canon += name;
                canon += '=';
                canon += buf;
                canon += '\n';
            }
            return "sha256:" + runtime::sha256_hex(canon);
        }
    } catch (...) {
    }
    return "raw:" + runtime::sha256_hex(body);
}

PresetLibrary::PresetLibrary(PresetManager& manager, fs::path overlay_file)
    : manager_(manager), store_(std::move(overlay_file)) {
    store_.load();
    std::lock_guard<std::mutex> lock(mutex_);
    rebuild_locked();
    load_program_map_locked();
    relink_locked();
}

fs::path PresetLibrary::default_overlay_path(const std::string& manufacturer,
                                             const std::string& plugin_name) {
    auto root = platform::special_dir(platform::SpecialDir::app_data);
    if (root.empty())
        return {};
    return root / sanitize_component(manufacturer) / sanitize_component(plugin_name) / "library" /
           "library.json";
}

void PresetLibrary::rebuild_locked() {
    entries_.clear();
    by_key_.clear();
    const auto factory_dir = manager_.factory_presets_dir();
    const auto user_dir = manager_.user_presets_dir();
    for (const auto& info : manager_.all_presets()) {
        Entry e;
        e.info = info;
        fs::path rel;
        std::string pack;
        if (relative_below(factory_dir, info.path, rel)) {
            e.source = PresetSource::factory;
            e.key = make_preset_key(e.source, without_extension(rel));
        } else if (relative_below(user_dir, info.path, rel)) {
            e.source = PresetSource::user;
            e.key = make_preset_key(e.source, without_extension(rel));
        } else if (parse_content_path(info.path, pack, rel)) {
            e.source = PresetSource::content;
            e.key = make_preset_key(e.source, without_extension(rel), pack);
        } else {
            // A preset outside every known root still needs a key; the file
            // name within its folder is the most stable thing left.
            e.source = info.is_factory ? PresetSource::factory : PresetSource::content;
            e.key = make_preset_key(e.source, info.path.stem().generic_string(), "other");
        }
        // Two installed versions of one pack share a key; keep both listed.
        const std::string base_key = e.key;
        for (int n = 2; by_key_.count(e.key); ++n)
            e.key = base_key + "#" + std::to_string(n);
        by_key_[e.key] = entries_.size();
        entries_.push_back(std::move(e));
    }
    ++generation_;
}

const PresetLibrary::Entry* PresetLibrary::find_entry_locked(const std::string& key) const {
    auto it = by_key_.find(key);
    return it == by_key_.end() ? nullptr : &entries_[it->second];
}

std::string PresetLibrary::digest_of_locked(const Entry& entry) const {
    return preset_digest(entry.info.path);
}

void PresetLibrary::relink_locked() {
    std::vector<PresetRecord> orphans;
    for (auto& rec : store_.records())
        if (!by_key_.count(rec.key) && !rec.digest.empty())
            orphans.push_back(std::move(rec));
    if (orphans.empty())
        return;

    // Digest of every indexed preset that has no edits of its own yet.
    std::unordered_map<std::string, std::vector<const Entry*>> by_digest;
    for (const auto& e : entries_) {
        if (store_.find(e.key))
            continue;
        auto d = digest_of_locked(e);
        if (!d.empty())
            by_digest[d].push_back(&e);
    }
    for (const auto& orphan : orphans) {
        auto it = by_digest.find(orphan.digest);
        if (it == by_digest.end())
            continue;
        const Entry* match = nullptr;
        int count = 0;
        for (const Entry* e : it->second)
            if (key_scope(e->key) == key_scope(orphan.key) && !store_.find(e->key)) {
                match = e;
                ++count;
            }
        if (count == 1) {
            store_.rekey(orphan.key, match->key);
            if (program_map_.rekey(orphan.key, match->key))
                save_program_map_locked();
        }
    }
}

void PresetLibrary::refresh() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        store_.reload_if_changed();
        rebuild_locked();
        load_program_map_locked();
        relink_locked();
        if (program_map_enabled_)
            pin_program_defaults_locked();
    }
    notify(true);
}

PresetSummary PresetLibrary::summarize_locked(const Entry& entry) const {
    PresetSummary s;
    s.key = entry.key;
    s.name = entry.info.name;
    s.source = entry.source;
    s.folder = entry.info.folder;
    s.author = entry.info.author;
    s.is_factory = entry.info.is_factory;
    s.path = entry.info.path;
    s.display_name = entry.info.name;
    s.tags = entry.info.tags;
    if (!program_map_.empty())
        s.program = program_map_.position_of(entry.key);
    if (const auto* rec = store_.find(entry.key)) {
        s.favourite = rec->favourite;
        if (!rec->display_name.empty())
            s.display_name = rec->display_name;
        for (const auto& t : rec->tags_add)
            if (!contains_tag(s.tags, t))
                s.tags.push_back(t);
        for (const auto& t : rec->tags_remove) {
            const auto want = lower_ascii(t);
            s.tags.erase(
                std::remove_if(s.tags.begin(), s.tags.end(),
                               [&](const std::string& x) { return lower_ascii(x) == want; }),
                s.tags.end());
        }
    }
    return s;
}

PresetPage PresetLibrary::query(const PresetQuery& q) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto words = [&] {
        auto w = split_words(q.text);
        for (auto& x : w)
            x = lower_ascii(std::move(x));
        return w;
    }();

    struct Hit {
        PresetSummary summary;
        int score = 0;
    };
    std::vector<Hit> hits;
    for (const auto& entry : entries_) {
        PresetSummary s = summarize_locked(entry);
        if (q.favourite && s.favourite != *q.favourite)
            continue;
        if (q.source && s.source != *q.source)
            continue;
        if (q.bank && !(s.program && s.program->bank == *q.bank))
            continue;
        bool tags_ok = true;
        for (const auto& t : q.tags_all)
            if (!contains_tag(s.tags, t))
                tags_ok = false;
        if (!tags_ok)
            continue;

        int score = 0;
        bool all_match = true;
        if (!words.empty()) {
            const auto display = lower_ascii(s.display_name);
            const auto stem = lower_ascii(s.name);
            std::string tags_joined;
            for (const auto& t : s.tags)
                tags_joined += lower_ascii(t) + "\n";
            const auto folder = lower_ascii(s.folder);
            const auto author = lower_ascii(s.author);
            const auto description = lower_ascii(entry.info.description);
            for (const auto& w : words) {
                int best = 0;
                for (const std::string* name : {&display, &stem}) {
                    for (auto p = name->find(w); p != std::string::npos; p = name->find(w, p + 1)) {
                        const int at = p == 0 ? 400 : (!is_word_char((*name)[p - 1]) ? 300 : 200);
                        best = std::max(best, at);
                    }
                }
                if (best == 0) {
                    if (tags_joined.find(w) != std::string::npos)
                        best = 150;
                    else if (folder.find(w) != std::string::npos)
                        best = 100;
                    else if (author.find(w) != std::string::npos ||
                             description.find(w) != std::string::npos)
                        best = 50;
                }
                if (best == 0) {
                    all_match = false;
                    break;
                }
                score += best;
            }
        }
        if (!all_match)
            continue;
        hits.push_back({std::move(s), score});
    }

    auto name_less = [](const PresetSummary& a, const PresetSummary& b) {
        const auto la = lower_ascii(a.display_name), lb = lower_ascii(b.display_name);
        if (la != lb)
            return la < lb;
        return a.key < b.key;
    };
    std::sort(hits.begin(), hits.end(), [&](const Hit& a, const Hit& b) {
        if (!words.empty() && a.score != b.score)
            return a.score > b.score;
        switch (q.sort) {
        case PresetSort::favourites_first:
            if (a.summary.favourite != b.summary.favourite)
                return a.summary.favourite;
            break;
        case PresetSort::source:
            if (a.summary.source != b.summary.source)
                return source_rank(a.summary.source) < source_rank(b.summary.source);
            break;
        case PresetSort::program:
            // Presets with a slot first, in (bank, program) order.
            if (a.summary.program.has_value() != b.summary.program.has_value())
                return a.summary.program.has_value();
            if (a.summary.program && b.summary.program &&
                !(*a.summary.program == *b.summary.program))
                return *a.summary.program < *b.summary.program;
            break;
        case PresetSort::name:
            break;
        }
        return name_less(a.summary, b.summary);
    });

    PresetPage page;
    page.total = hits.size();
    page.generation = generation_;
    const std::size_t begin = std::min(q.offset, hits.size());
    const std::size_t end = q.limit == 0 ? hits.size() : std::min(hits.size(), begin + q.limit);
    for (std::size_t i = begin; i < end; ++i)
        page.items.push_back(std::move(hits[i].summary));
    return page;
}

std::optional<PresetSummary> PresetLibrary::find(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Entry* e = find_entry_locked(key);
    if (!e)
        return std::nullopt;
    return summarize_locked(*e);
}

std::string PresetLibrary::key_for(const fs::path& preset_file) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto want = preset_file.lexically_normal();
    for (const auto& e : entries_)
        if (e.info.path.lexically_normal() == want)
            return e.key;
    return {};
}

bool PresetLibrary::edit_locked(const std::string& key,
                                const std::function<bool(const std::string&)>& apply) {
    const Entry* e = find_entry_locked(key);
    if (!e)
        return false;
    const bool ok = apply(digest_of_locked(*e));
    if (!ok)
        persist_ok_ = false;
    ++generation_;
    return ok;
}

void PresetLibrary::notify(bool changed) {
    if (changed && on_changed)
        on_changed();
}

bool PresetLibrary::set_favourite(const std::string& key, bool favourite) {
    bool ok, found;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        found = find_entry_locked(key) != nullptr;
        ok = edit_locked(
            key, [&](const std::string& d) { return store_.set_favourite(key, favourite, d); });
    }
    notify(found);
    return ok;
}

bool PresetLibrary::set_display_name(const std::string& key, const std::string& name) {
    bool ok, found;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        found = find_entry_locked(key) != nullptr;
        ok = edit_locked(
            key, [&](const std::string& d) { return store_.set_display_name(key, name, d); });
    }
    notify(found);
    return ok;
}

bool PresetLibrary::add_tag(const std::string& key, const std::string& tag) {
    bool ok, found;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        found = find_entry_locked(key) != nullptr;
        ok = edit_locked(key, [&](const std::string& d) { return store_.add_tag(key, tag, d); });
    }
    notify(found);
    return ok;
}

bool PresetLibrary::remove_tag(const std::string& key, const std::string& tag) {
    bool ok, found;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        found = find_entry_locked(key) != nullptr;
        ok = edit_locked(key, [&](const std::string& d) { return store_.remove_tag(key, tag, d); });
    }
    notify(found);
    return ok;
}

std::size_t PresetLibrary::orphan_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t n = 0;
    for (const auto& rec : store_.records())
        if (!by_key_.count(rec.key))
            ++n;
    return n;
}

void PresetLibrary::clear_orphans() {
    bool any = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& rec : store_.records())
            if (!by_key_.count(rec.key))
                any = store_.erase(rec.key) || any;
        if (any)
            ++generation_;
    }
    notify(any);
}

void PresetLibrary::load_program_map_locked() {
    if (store_.program_map_json().empty()) {
        program_map_ = {};
        return;
    }
    if (auto parsed = ProgramMap::from_json(store_.program_map_json()))
        program_map_ = std::move(*parsed);
}

bool PresetLibrary::save_program_map_locked() {
    const bool ok = store_.set_program_map_json(program_map_.to_json());
    if (!ok)
        persist_ok_ = false;
    ++generation_;
    return ok;
}

bool PresetLibrary::pin_program_defaults_locked() {
    // New presets of each source, in a stable order (file name, then key) that
    // does not move when a preset is renamed in the overlay.
    std::map<std::string, std::vector<const Entry*>> by_scope;
    for (const auto& e : entries_)
        by_scope[key_scope(e.key)].push_back(&e);
    std::vector<ProgramPinGroup> groups;
    auto make_group = [&](const std::string& scope, const std::string& label) {
        auto it = by_scope.find(scope);
        if (it == by_scope.end())
            return;
        auto entries = it->second;
        std::sort(entries.begin(), entries.end(), [](const Entry* a, const Entry* b) {
            const auto la = lower_ascii(a->info.name), lb = lower_ascii(b->info.name);
            return la != lb ? la < lb : a->key < b->key;
        });
        ProgramPinGroup g;
        g.scope = scope;
        g.label = label;
        for (const auto* e : entries)
            g.keys.push_back(e->key);
        groups.push_back(std::move(g));
        by_scope.erase(it);
    };
    make_group("factory:", "Factory");
    make_group("user:", "User");
    // Whatever is left is a content pack, in scope order: "content:<pack>:".
    std::vector<std::string> rest;
    for (const auto& [scope, v] : by_scope)
        rest.push_back(scope);
    for (const auto& scope : rest) {
        std::string label = scope;
        if (scope.rfind("content:", 0) == 0)
            label = scope.substr(8, scope.size() - 9);
        make_group(scope, label);
    }
    if (!program_map_.pin_defaults(groups))
        return false;
    save_program_map_locked();
    return true;
}

void PresetLibrary::enable_program_map() {
    bool changed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool was_enabled = program_map_enabled_;
        program_map_enabled_ = true;
        changed = pin_program_defaults_locked() || !was_enabled;
        if (changed)
            ++generation_;
    }
    notify(changed);
}

bool PresetLibrary::program_map_enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return program_map_enabled_;
}

ProgramMap PresetLibrary::program_map() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return program_map_;
}

std::optional<PresetSummary> PresetLibrary::preset_at(BankProgram position) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto key = program_map_.key_at(position);
    if (!key)
        return std::nullopt;
    const Entry* e = find_entry_locked(*key);
    if (!e)
        return std::nullopt;
    return summarize_locked(*e);
}

std::optional<BankProgram> PresetLibrary::program_of(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return program_map_.position_of(key);
}

bool PresetLibrary::assign_program(BankProgram position, const std::string& key) {
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (find_entry_locked(key) && program_map_.assign(position, key)) {
            save_program_map_locked();
            ok = true;
        }
    }
    notify(ok);
    return ok;
}

bool PresetLibrary::clear_program(BankProgram position) {
    bool ok;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ok = program_map_.clear(position);
        if (ok)
            save_program_map_locked();
    }
    notify(ok);
    return ok;
}

bool PresetLibrary::set_bank_name(std::uint16_t bank, const std::string& name) {
    bool ok;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ok = program_map_.set_bank_name(bank, name);
        if (ok)
            save_program_map_locked();
    }
    notify(ok);
    return ok;
}

bool PresetLibrary::renumber_bank(std::uint16_t from, std::uint16_t to) {
    bool ok;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ok = program_map_.renumber_bank(from, to);
        if (ok && from != to)
            save_program_map_locked();
    }
    notify(ok);
    return ok;
}

std::uint64_t PresetLibrary::generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

PresetLibraryCapabilities PresetLibrary::capabilities() const {
    std::lock_guard<std::mutex> lock(mutex_);
    PresetLibraryCapabilities c;
    c.persistence = store_.persistent() && persist_ok_;
    c.program_map = program_map_enabled_;
    return c;
}

} // namespace pulp::state
