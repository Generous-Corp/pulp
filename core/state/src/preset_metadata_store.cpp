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

bool PresetMetadataStore::read_file(std::map<std::string, PresetRecord>& out) const {
    out.clear();
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
        out.clear();
        return false;
    }
    return true;
}

bool PresetMetadataStore::write_file(const std::map<std::string, PresetRecord>& records) {
    if (file_.empty())
        return true;
    std::error_code ec;
    if (file_.has_parent_path())
        fs::create_directories(file_.parent_path(), ec);

    std::ostringstream out;
    out << "{\n  \"schema\": " << kSchema << ",\n  \"revision\": " << (revision_ + 1)
        << ",\n  \"records\": [";
    bool first = true;
    const auto cutoff = clock_now() - kTombstoneKeepMs;
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
    out << "\n  ]\n}\n";

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
    std::map<std::string, PresetRecord> disk;
    const bool ok = read_file(disk);
    records_ = std::move(disk);
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
    std::map<std::string, PresetRecord> disk;
    if (!read_file(disk))
        return false;
    // Keep edits made here that have not reached the file.
    merge_into(disk, records_);
    const bool changed =
        disk.size() != records_.size() ||
        !std::equal(disk.begin(), disk.end(), records_.begin(), [](const auto& a, const auto& b) {
            return a.first == b.first && a.second.updated_ms == b.second.updated_ms &&
                   a.second.deleted == b.second.deleted;
        });
    records_ = std::move(disk);
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

std::int64_t PresetMetadataStore::clock_now() const {
    return clock_ ? clock_() : now_ms();
}

std::int64_t PresetMetadataStore::next_stamp(std::int64_t after) const {
    return std::max(clock_now(), after + 1);
}

template <typename Apply> bool PresetMetadataStore::commit(Apply&& apply) {
    if (file_.empty()) {
        apply();
        return true;
    }
    std::error_code ec;
    if (file_.has_parent_path()) {
        fs::create_directories(file_.parent_path(), ec);
        if (!fs::is_directory(file_.parent_path(), ec)) {
            apply(); // the edit lasts for the session
            return false;
        }
    }
    fs::path lock_path = file_;
    lock_path += ".lock";
    DirLock lock(lock_path);
    if (!lock.acquire()) {
        apply();
        return false;
    }

    // Merge the file first and apply the edit on top, so the edit's stamp is
    // taken after every version it replaces has been seen. Stamps are then a
    // Lamport clock per record: an edit always outranks the state it was made
    // over, even when both share a clock tick or this machine's clock is behind.
    std::map<std::string, PresetRecord> disk;
    if (!read_file(disk))
        disk.clear(); // a corrupt file is replaced by the merged state
    merge_into(records_, disk);
    apply();
    return write_file(records_);
}

template <typename Edit>
bool PresetMetadataStore::edit(const std::string& key, const std::string& digest, Edit&& apply) {
    return commit([&] {
        auto& rec = records_[key];
        rec.key = key;
        rec.deleted = false;
        apply(rec);
        if (!digest.empty())
            rec.digest = digest;
        rec.updated_ms = next_stamp(rec.updated_ms);
        if (rec.empty())
            rec.deleted = true;
    });
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

bool PresetMetadataStore::rekey(const std::string& old_key, const std::string& new_key) {
    auto it = records_.find(old_key);
    if (it == records_.end() || it->second.deleted || old_key == new_key)
        return false;
    return commit([&] {
        auto from = records_.find(old_key);
        if (from == records_.end() || from->second.deleted)
            return; // another instance removed it meanwhile
        auto to = records_.find(new_key);
        const std::int64_t seen =
            std::max(from->second.updated_ms,
                     to == records_.end() ? std::int64_t{0} : to->second.updated_ms);
        PresetRecord moved = from->second;
        moved.key = new_key;
        moved.updated_ms = next_stamp(seen);
        from->second.deleted = true;
        from->second.updated_ms = moved.updated_ms;
        records_[new_key] = std::move(moved);
    });
}

bool PresetMetadataStore::erase(const std::string& key) {
    auto it = records_.find(key);
    if (it == records_.end() || it->second.deleted)
        return false;
    return commit([&] {
        auto& rec = records_[key];
        rec.key = key;
        rec.updated_ms = next_stamp(rec.updated_ms);
        rec.deleted = true;
    });
}

} // namespace pulp::state
