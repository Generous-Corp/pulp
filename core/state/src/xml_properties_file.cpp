#include <pulp/state/xml_properties_file.hpp>

#include <pulp/runtime/detail/durable_file_replacement.hpp>

#include <pugixml.hpp>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <span>
#include <system_error>

namespace pulp::state {

namespace {

constexpr std::string_view kRoot = "PROPERTIES";
constexpr std::string_view kValue = "VALUE";
constexpr std::string_view kCrlf = "\r\n";

// Decodes one UTF-8 sequence at `at`; an invalid or truncated sequence yields
// U+FFFD and consumes one byte.
std::uint32_t next_code_point(std::string_view s, std::size_t& at) {
    const auto b0 = static_cast<unsigned char>(s[at]);
    auto fail = [&] {
        ++at;
        return 0xFFFDu;
    };
    if (b0 < 0x80) {
        ++at;
        return b0;
    }
    int extra = 0;
    std::uint32_t cp = 0;
    std::uint32_t min = 0;
    if ((b0 & 0xE0) == 0xC0) {
        extra = 1;
        cp = b0 & 0x1F;
        min = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
        extra = 2;
        cp = b0 & 0x0F;
        min = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
        extra = 3;
        cp = b0 & 0x07;
        min = 0x10000;
    } else
        return fail();
    if (at + static_cast<std::size_t>(extra) >= s.size())
        return fail();
    for (int i = 1; i <= extra; ++i) {
        const auto b = static_cast<unsigned char>(s[at + static_cast<std::size_t>(i)]);
        if ((b & 0xC0) != 0x80)
            return fail();
        cp = (cp << 6) | (b & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return fail();
    at += static_cast<std::size_t>(extra) + 1;
    return cp;
}

void append_escaped(std::string& out, std::string_view text) {
    std::size_t at = 0;
    while (at < text.size()) {
        const auto cp = next_code_point(text, at);
        switch (cp) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            if (cp >= 0x20 && cp < 0x7F) {
                out += static_cast<char>(cp);
            } else {
                out += "&#";
                out += std::to_string(cp);
                out += ';';
            }
        }
    }
}

bool read_file(const std::filesystem::path& path, std::string& out, bool& exists) {
    std::error_code ec;
    exists = std::filesystem::exists(path, ec);
    if (ec)
        return false;
    if (!exists)
        return true;
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

// Copies `bytes` to `backup` unless something is already there. The copy is
// written to a sibling first and published with a hard link, which refuses an
// existing name, so an existing backup is never replaced.
bool write_backup_once(const std::filesystem::path& backup, std::string_view bytes) {
    std::error_code ec;
    if (std::filesystem::exists(backup, ec))
        return true;
    auto partial = backup;
    partial += ".partial";
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            out.close();
            std::filesystem::remove(partial, ec);
            return false;
        }
    }
    std::filesystem::create_hard_link(partial, backup, ec);
    if (ec && !std::filesystem::exists(backup)) {
        // A filesystem without hard links: copy, still refusing to overwrite.
        ec.clear();
        std::filesystem::copy_file(partial, backup, std::filesystem::copy_options::skip_existing,
                                   ec);
    }
    std::error_code ignored;
    std::filesystem::remove(partial, ignored);
    return std::filesystem::exists(backup, ignored);
}

void set_error(XmlPropertiesError* error, XmlPropertiesError::Kind kind, std::string message) {
    if (error) {
        error->kind = kind;
        error->message = std::move(message);
        error->backup_path.clear();
    }
}

} // namespace

XmlPropertiesFile::XmlPropertiesFile(std::filesystem::path path) : path_(std::move(path)) {}

std::filesystem::path XmlPropertiesFile::backup_path_for(const std::filesystem::path& path) {
    auto backup = path;
    backup += ".bak";
    return backup;
}

std::optional<std::vector<XmlPropertiesFile::Entry>>
XmlPropertiesFile::parse(std::string_view bytes, XmlPropertiesError* error) {
    // parse_fragment keeps text at the document level as nodes, so text before
    // the root can be refused (without it pugixml drops that text silently).
    // parse_wconv_attribute is off so a literal tab or line feed in a value
    // reads as itself rather than as a space, and parse_eol is off so a
    // literal carriage return (alone or before a line feed) reads as itself
    // rather than being folded into a line feed.
    constexpr unsigned options = (pugi::parse_default | pugi::parse_fragment) &
                                 ~(pugi::parse_wconv_attribute | pugi::parse_eol);
    pugi::xml_document doc;
    const auto result = doc.load_buffer(bytes.data(), bytes.size(), options, pugi::encoding_utf8);
    if (!result) {
        set_error(error, XmlPropertiesError::Kind::parse,
                  std::string("not well-formed XML at byte ") + std::to_string(result.offset) +
                      ": " + result.description());
        return std::nullopt;
    }
    // Text before the first element is an error. Everything after the first
    // element (another root, text, a comment) is ignored.
    pugi::xml_node root;
    for (auto node : doc.children()) {
        if (node.type() == pugi::node_pcdata || node.type() == pugi::node_cdata) {
            set_error(error, XmlPropertiesError::Kind::parse, "text before the root element");
            return std::nullopt;
        }
        if (node.type() == pugi::node_element) {
            root = node;
            break;
        }
    }
    if (!root || std::string_view(root.name()) != kRoot) {
        set_error(error, XmlPropertiesError::Kind::parse,
                  root ? std::string("root element is <") + root.name() + ">, not <PROPERTIES>"
                       : std::string("no root element"));
        return std::nullopt;
    }
    std::vector<Entry> entries;
    for (auto value : root.children()) {
        if (value.type() != pugi::node_element || std::string_view(value.name()) != kValue)
            continue;
        const auto name = value.attribute("name");
        if (!name)
            continue;
        std::string key = name.value();
        std::string val = value.attribute("val").value();
        bool found = false;
        for (auto& entry : entries) {
            if (entry.first == key) {
                entry.second = std::move(val); // first position, last value
                found = true;
                break;
            }
        }
        if (!found)
            entries.emplace_back(std::move(key), std::move(val));
    }
    return entries;
}

std::string XmlPropertiesFile::serialize(const std::vector<Entry>& entries) {
    std::string out;
    out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
    out += kCrlf;
    out += kCrlf;
    out += "<PROPERTIES>";
    out += kCrlf;
    for (const auto& [name, value] : entries) {
        out += "  <VALUE name=\"";
        append_escaped(out, name);
        out += "\" val=\"";
        append_escaped(out, value);
        out += "\"/>";
        out += kCrlf;
    }
    out += "</PROPERTIES>";
    out += kCrlf;
    return out;
}

std::optional<XmlPropertiesFile> XmlPropertiesFile::load(const std::filesystem::path& path,
                                                         XmlPropertiesError* error) {
    std::string bytes;
    bool exists = false;
    if (!read_file(path, bytes, exists)) {
        set_error(error, XmlPropertiesError::Kind::io, "cannot read " + path.string());
        return std::nullopt;
    }
    XmlPropertiesFile file(path);
    if (!exists)
        return file;
    auto entries = parse(bytes, error);
    if (!entries) {
        const auto backup = backup_path_for(path);
        if (write_backup_once(backup, bytes) && error)
            error->backup_path = backup;
        return std::nullopt;
    }
    file.entries_ = std::move(*entries);
    file.saved_ = file.entries_;
    return file;
}

std::optional<std::string> XmlPropertiesFile::get(std::string_view name) const {
    for (const auto& [key, value] : entries_)
        if (key == name)
            return value;
    return std::nullopt;
}

bool XmlPropertiesFile::contains(std::string_view name) const {
    return get(name).has_value();
}

void XmlPropertiesFile::set(std::string_view name, std::string_view value) {
    for (auto& [key, existing] : entries_) {
        if (key == name) {
            existing = std::string(value);
            return;
        }
    }
    entries_.emplace_back(std::string(name), std::string(value));
}

bool XmlPropertiesFile::remove(std::string_view name) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->first == name) {
            entries_.erase(it);
            return true;
        }
    }
    return false;
}

bool XmlPropertiesFile::save(XmlPropertiesError* error) {
    if (!dirty())
        return true;
    const auto bytes = serialize(entries_);
    auto replacement = runtime::detail::DurableFileReplacement::create(path_);
    if (!replacement || !replacement->valid()) {
        set_error(error, XmlPropertiesError::Kind::io,
                  "cannot create a temporary file beside " + path_.string());
        return false;
    }
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    if (!replacement->write_all(std::span<const std::uint8_t>(data, bytes.size()))) {
        replacement->cancel();
        set_error(error, XmlPropertiesError::Kind::io, "cannot write " + path_.string());
        return false;
    }
    if (replacement->commit() == runtime::detail::DurableFileCommitOutcome::NotReplaced) {
        set_error(error, XmlPropertiesError::Kind::io, "cannot replace " + path_.string());
        return false;
    }
    saved_ = entries_;
    return true;
}

} // namespace pulp::state
