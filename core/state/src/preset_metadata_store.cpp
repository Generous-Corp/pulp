#include <pulp/state/preset_metadata_store.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace pulp::state {

namespace {

constexpr int kSchema = 1;
constexpr std::int64_t kTombstoneKeepMs = 30LL * 24 * 60 * 60 * 1000;

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string lower_ascii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool has_tag(const std::vector<std::string>& tags, const std::string& tag) {
    const auto want = lower_ascii(tag);
    return std::any_of(tags.begin(), tags.end(),
                       [&](const std::string& t) { return lower_ascii(t) == want; });
}

void drop_tag(std::vector<std::string>& tags, const std::string& tag) {
    const auto want = lower_ascii(tag);
    tags.erase(std::remove_if(tags.begin(), tags.end(),
                              [&](const std::string& t) { return lower_ascii(t) == want; }),
               tags.end());
}

// A directory is the one lock primitive that is atomic on every platform: the
// creation either succeeds for exactly one caller or reports it already exists.
class DirLock {
  public:
    explicit DirLock(fs::path dir) : dir_(std::move(dir)) {}
    ~DirLock() {
        if (held_) {
            std::error_code ec;
            fs::remove(dir_, ec);
        }
    }
    bool acquire(int timeout_ms = 4000) {
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            std::error_code ec;
            if (fs::create_directory(dir_, ec) && !ec) {
                held_ = true;
                return true;
            }
            // A holder that died leaves the directory behind; break a stale one.
            const auto mtime = fs::last_write_time(dir_, ec);
            if (!ec && fs::file_time_type::clock::now() - mtime > std::chrono::seconds(15)) {
                fs::remove(dir_, ec);
                continue;
            }
            if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(timeout_ms))
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    }

  private:
    fs::path dir_;
    bool held_ = false;
};

std::string join_strings(const std::vector<std::string>& values) {
    std::ostringstream out;
    out << "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0)
            out << ", ";
        out << choc::json::getEscapedQuotedString(values[i]);
    }
    out << "]";
    return out.str();
}

std::vector<std::string> string_array(const choc::value::ValueView& doc, const char* key) {
    std::vector<std::string> out;
    if (!doc.hasObjectMember(key))
        return out;
    auto arr = doc[key];
    if (!arr.isArray())
        return out;
    for (std::uint32_t i = 0; i < arr.size(); ++i)
        if (arr[i].isString())
            out.emplace_back(arr[i].getString());
    return out;
}

bool is_int(const choc::value::ValueView& v) {
    return v.isInt32() || v.isInt64();
}
std::int64_t as_int(const choc::value::ValueView& v) {
    return v.isInt32() ? v.getInt32() : v.getInt64();
}

// Newest edit wins; on a tie the tombstone loses so a re-add is not undone.
void merge_into(std::map<std::string, PresetRecord>& into,
                const std::map<std::string, PresetRecord>& from) {
    for (const auto& [key, rec] : from) {
        auto it = into.find(key);
        if (it == into.end()) {
            into.emplace(key, rec);
        } else if (rec.updated_ms > it->second.updated_ms ||
                   (rec.updated_ms == it->second.updated_ms && it->second.deleted &&
                    !rec.deleted)) {
            it->second = rec;
        }
    }
}

} // namespace

PresetMetadataStore::PresetMetadataStore(fs::path file) : file_(std::move(file)) {}

bool PresetMetadataStore::read_file(Disk& disk) const {
    auto& out = disk.records;
    disk = {};
    if (file_.empty())
        return true;
    std::error_code ec;
    if (!fs::exists(file_, ec))
        return true;
    std::ifstream in(file_);
    if (!in.is_open())
        return false;
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    try {
        auto doc = choc::json::parse(body);
        if (!doc.isObject() || !doc.hasObjectMember("records"))
            return false;
        auto records = doc["records"];
        if (!records.isArray())
            return false;
        if (doc.hasObjectMember("program_map") && doc["program_map"].isObject()) {
            disk.program_map_json = choc::json::toString(doc["program_map"]);
            if (doc.hasObjectMember("program_map_updated") && is_int(doc["program_map_updated"]))
                disk.program_map_updated_ms = as_int(doc["program_map_updated"]);
        }
        for (std::uint32_t i = 0; i < records.size(); ++i) {
            auto r = records[i];
            if (!r.isObject() || !r.hasObjectMember("key") || !r["key"].isString())
                continue;
            PresetRecord rec;
            rec.key = std::string(r["key"].getString());
            if (r.hasObjectMember("digest") && r["digest"].isString())
                rec.digest = std::string(r["digest"].getString());
            if (r.hasObjectMember("favourite") && r["favourite"].isBool())
                rec.favourite = r["favourite"].getBool();
            if (r.hasObjectMember("display_name") && r["display_name"].isString())
                rec.display_name = std::string(r["display_name"].getString());
            rec.tags_add = string_array(r, "tags_add");
            rec.tags_remove = string_array(r, "tags_remove");
            if (r.hasObjectMember("updated") && is_int(r["updated"]))
                rec.updated_ms = as_int(r["updated"]);
            if (r.hasObjectMember("deleted") && r["deleted"].isBool())
                rec.deleted = r["deleted"].getBool();
            out[rec.key] = std::move(rec);
        }
    } catch (...) {
        disk = {};
        return false;
    }
    return true;
}

void PresetMetadataStore::merge_disk(const Disk& disk) {
    merge_into(records_, disk.records);
    if (disk.program_map_updated_ms > program_map_updated_ms_) {
        program_map_json_ = disk.program_map_json;
        program_map_updated_ms_ = disk.program_map_updated_ms;
    }
}

bool PresetMetadataStore::write_file() {
    const auto& records = records_;
    if (file_.empty())
        return true;
    std::error_code ec;
    if (file_.has_parent_path())
        fs::create_directories(file_.parent_path(), ec);

    std::ostringstream out;
    out << "{\n  \"schema\": " << kSchema << ",\n  \"revision\": " << (revision_ + 1)
        << ",\n  \"records\": [";
    bool first = true;
    const auto cutoff = now_ms() - kTombstoneKeepMs;
    for (const auto& [key, rec] : records) {
        if (rec.deleted && rec.updated_ms < cutoff)
            continue; // compact old tombstones
        out << (first ? "\n" : ",\n");
        first = false;
        out << "    {\"key\": " << choc::json::getEscapedQuotedString(rec.key);
        if (!rec.digest.empty())
            out << ", \"digest\": " << choc::json::getEscapedQuotedString(rec.digest);
        if (rec.favourite)
            out << ", \"favourite\": true";
        if (!rec.display_name.empty())
            out << ", \"display_name\": " << choc::json::getEscapedQuotedString(rec.display_name);
        if (!rec.tags_add.empty())
            out << ", \"tags_add\": " << join_strings(rec.tags_add);
        if (!rec.tags_remove.empty())
            out << ", \"tags_remove\": " << join_strings(rec.tags_remove);
        out << ", \"updated\": " << rec.updated_ms;
        if (rec.deleted)
            out << ", \"deleted\": true";
        out << "}";
    }
    out << "\n  ]";
    if (!program_map_json_.empty())
        out << ",\n  \"program_map\": " << program_map_json_
            << ",\n  \"program_map_updated\": " << program_map_updated_ms_;
    out << "\n}\n";

    fs::path tmp = file_;
    tmp += ".tmp";
    {
        std::ofstream f(tmp);
        if (!f.is_open())
            return false;
        f << out.str();
        f.flush();
        if (!f.good()) {
            f.close();
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, file_, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    ++revision_;
    seen_file_ = true;
    seen_mtime_ = fs::last_write_time(file_, ec);
    seen_size_ = fs::file_size(file_, ec);
    return true;
}

bool PresetMetadataStore::load() {
    Disk disk;
    const bool ok = read_file(disk);
    records_ = std::move(disk.records);
    program_map_json_ = std::move(disk.program_map_json);
    program_map_updated_ms_ = disk.program_map_updated_ms;
    std::error_code ec;
    seen_file_ = !file_.empty() && fs::exists(file_, ec);
    if (seen_file_) {
        seen_mtime_ = fs::last_write_time(file_, ec);
        seen_size_ = fs::file_size(file_, ec);
    }
    return ok;
}

bool PresetMetadataStore::reload_if_changed() {
    if (file_.empty())
        return false;
    std::error_code ec;
    const bool exists = fs::exists(file_, ec);
    if (!exists && !seen_file_)
        return false;
    if (exists && seen_file_ && fs::last_write_time(file_, ec) == seen_mtime_ &&
        fs::file_size(file_, ec) == seen_size_)
        return false;
    Disk disk;
    if (!read_file(disk))
        return false;
    // Keep edits made here that have not reached the file.
    auto merged = std::move(disk.records);
    merge_into(merged, records_);
    const bool changed = merged.size() != records_.size() ||
                         !std::equal(merged.begin(), merged.end(), records_.begin(),
                                     [](const auto& a, const auto& b) {
                                         return a.first == b.first &&
                                                a.second.updated_ms == b.second.updated_ms &&
                                                a.second.deleted == b.second.deleted;
                                     }) ||
                         disk.program_map_updated_ms > program_map_updated_ms_;
    records_ = std::move(merged);
    if (disk.program_map_updated_ms > program_map_updated_ms_) {
        program_map_json_ = std::move(disk.program_map_json);
        program_map_updated_ms_ = disk.program_map_updated_ms;
    }
    seen_file_ = exists;
    if (exists) {
        seen_mtime_ = fs::last_write_time(file_, ec);
        seen_size_ = fs::file_size(file_, ec);
    }
    return changed;
}

const PresetRecord* PresetMetadataStore::find(const std::string& key) const {
    auto it = records_.find(key);
    return (it == records_.end() || it->second.deleted) ? nullptr : &it->second;
}

std::vector<PresetRecord> PresetMetadataStore::records() const {
    std::vector<PresetRecord> out;
    for (const auto& [key, rec] : records_)
        if (!rec.deleted)
            out.push_back(rec);
    return out;
}

bool PresetMetadataStore::commit() {
    if (file_.empty())
        return true;
    std::error_code ec;
    if (file_.has_parent_path()) {
        fs::create_directories(file_.parent_path(), ec);
        if (!fs::is_directory(file_.parent_path(), ec))
            return false;
    }
    fs::path lock_path = file_;
    lock_path += ".lock";
    DirLock lock(lock_path);
    if (!lock.acquire())
        return false;

    Disk disk;
    if (!read_file(disk))
        disk = {}; // a corrupt file is replaced by the merged state
    merge_disk(disk);
    return write_file();
}

template <typename Edit>
bool PresetMetadataStore::edit(const std::string& key, const std::string& digest, Edit&& apply) {
    auto& rec = records_[key];
    rec.key = key;
    rec.deleted = false;
    apply(rec);
    if (!digest.empty())
        rec.digest = digest;
    rec.updated_ms = std::max(now_ms(), rec.updated_ms + 1);
    if (rec.empty())
        rec.deleted = true;
    return commit();
}

bool PresetMetadataStore::set_favourite(const std::string& key, bool favourite,
                                        const std::string& digest) {
    return edit(key, digest, [&](PresetRecord& r) { r.favourite = favourite; });
}

bool PresetMetadataStore::set_display_name(const std::string& key, const std::string& name,
                                           const std::string& digest) {
    return edit(key, digest, [&](PresetRecord& r) { r.display_name = name; });
}

bool PresetMetadataStore::add_tag(const std::string& key, const std::string& tag,
                                  const std::string& digest) {
    return edit(key, digest, [&](PresetRecord& r) {
        drop_tag(r.tags_remove, tag);
        if (!has_tag(r.tags_add, tag))
            r.tags_add.push_back(tag);
    });
}

bool PresetMetadataStore::remove_tag(const std::string& key, const std::string& tag,
                                     const std::string& digest) {
    return edit(key, digest, [&](PresetRecord& r) {
        drop_tag(r.tags_add, tag);
        if (!has_tag(r.tags_remove, tag))
            r.tags_remove.push_back(tag);
    });
}

bool PresetMetadataStore::set_program_map_json(const std::string& json) {
    program_map_json_ = json;
    program_map_updated_ms_ = std::max(now_ms(), program_map_updated_ms_ + 1);
    return commit();
}

bool PresetMetadataStore::rekey(const std::string& old_key, const std::string& new_key) {
    auto it = records_.find(old_key);
    if (it == records_.end() || it->second.deleted || old_key == new_key)
        return false;
    PresetRecord moved = it->second;
    moved.key = new_key;
    moved.updated_ms = std::max(now_ms(), moved.updated_ms + 1);
    it->second.deleted = true;
    it->second.updated_ms = moved.updated_ms;
    records_[new_key] = std::move(moved);
    return commit();
}

bool PresetMetadataStore::erase(const std::string& key) {
    auto it = records_.find(key);
    if (it == records_.end() || it->second.deleted)
        return false;
    it->second.deleted = true;
    it->second.updated_ms = std::max(now_ms(), it->second.updated_ms + 1);
    return commit();
}

} // namespace pulp::state
