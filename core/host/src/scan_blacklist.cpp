#include <pulp/host/scan_blacklist.hpp>

#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace pulp::host {

namespace fs = std::filesystem;

namespace {

struct FileStamp {
    int64_t mtime = 0;
    int64_t size = 0;
    bool ok = false;
};

FileStamp stamp_of(const std::string& path) {
    FileStamp out;
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) return out;
    auto mtime = fs::last_write_time(path, ec);
    if (ec) return out;
    out.mtime = std::chrono::duration_cast<std::chrono::seconds>(
        mtime.time_since_epoch()).count();
    out.size = static_cast<int64_t>(fs::file_size(path, ec));
    if (ec) return out;
    out.ok = true;
    return out;
}

// Replace `|` and newlines in a stored field with percent-escapes so
// our pipe-separated format can round-trip arbitrary paths / reasons.
std::string escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '|')       out += "%7C";
        else if (c == '\n') out += "%0A";
        else if (c == '%')  out += "%25";
        else                out += c;
    }
    return out;
}

std::string unescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = s.substr(i + 1, 2);
            if      (hex == "7C") { out += '|';  i += 2; }
            else if (hex == "0A") { out += '\n'; i += 2; }
            else if (hex == "25") { out += '%';  i += 2; }
            else                  { out += s[i]; }
        } else {
            out += s[i];
        }
    }
    return out;
}

bool parse_int64_field(const std::string& text, int64_t& out) {
    try {
        std::size_t pos = 0;
        const auto value = std::stoll(text, &pos);
        while (pos < text.size()) {
            if (!std::isspace(static_cast<unsigned char>(text[pos]))) return false;
            ++pos;
        }
        out = value;
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

std::optional<BlacklistEntry> ScanBlacklist::get(const std::string& path) const {
    auto it = entries_.find(path);
    if (it == entries_.end()) return std::nullopt;
    auto s = stamp_of(path);
    if (!s.ok) return it->second;  // file gone — keep the entry
    if (s.mtime != it->second.mtime || s.size != it->second.size) {
        return std::nullopt;  // file changed → not blacklisted anymore
    }
    return it->second;
}

void ScanBlacklist::blacklist(const std::string& path, std::string reason) {
    auto s = stamp_of(path);
    BlacklistEntry e;
    e.mtime = s.ok ? s.mtime : 0;
    e.size  = s.ok ? s.size  : 0;
    e.reason = std::move(reason);
    entries_[path] = std::move(e);
}

std::string ScanBlacklist::to_text() const {
    std::ostringstream out;
    out << "# pulp scan blacklist v1 — fields: path|mtime|size|reason\n";
    for (const auto& [path, e] : entries_) {
        out << escape(path) << '|'
            << e.mtime << '|'
            << e.size  << '|'
            << escape(e.reason) << '\n';
    }
    return out.str();
}

bool ScanBlacklist::from_text(const std::string& text) {
    entries_.clear();
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> fields;
        fields.reserve(4);
        std::size_t start = 0;
        for (std::size_t i = 0; i <= line.size(); ++i) {
            if (i == line.size() || line[i] == '|') {
                fields.push_back(line.substr(start, i - start));
                start = i + 1;
            }
        }
        if (fields.size() != 4) continue;  // malformed — skip
        BlacklistEntry e;
        if (!parse_int64_field(fields[1], e.mtime) ||
            !parse_int64_field(fields[2], e.size)) continue;
        e.reason = unescape(fields[3]);
        entries_[unescape(fields[0])] = std::move(e);
    }
    return true;
}

bool ScanBlacklist::save_to(const std::string& file_path) const {
    std::error_code ec;
    auto parent = fs::path(file_path).parent_path();
    if (!parent.empty()) fs::create_directories(parent, ec);
    std::ofstream f(file_path, std::ios::binary);
    if (!f) return false;
    f << to_text();
    return static_cast<bool>(f);
}

bool ScanBlacklist::load_from(const std::string& file_path) {
    std::ifstream f(file_path, std::ios::binary);
    if (!f) return false;
    std::ostringstream buf;
    buf << f.rdbuf();
    return from_text(buf.str());
}

} // namespace pulp::host
