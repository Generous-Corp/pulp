#pragma once

// XmlPropertiesFile: a flat name/value settings file stored as XML, written in
// one canonical layout so other programs that use the same format read it
// unchanged, and read as tolerantly as they write.
//
// The format:
//
//   <?xml version="1.0" encoding="UTF-8"?>
//   (blank line)
//   <PROPERTIES>
//     <VALUE name="..." val="..."/>
//   </PROPERTIES>
//
// Every line, the last included, ends in CRLF. One VALUE per entry, indented
// two spaces, in the file's order. Writing escapes & < > " as named entities,
// tab as &#9;, line feed as &#10; and carriage return as &#13;, and writes every
// non-ASCII character as a decimal character reference, so the bytes are ASCII.
// Other control characters are written as decimal references (&#1;), and an
// apostrophe as is. A file with no entries is the root's start and end tags on
// consecutive lines.
//
// Reading accepts single or double quotes, entities, character references and
// literal UTF-8. Leading and trailing spaces in names and values are kept, and
// a literal tab, line feed or other control byte in a value reads as itself.
// Anything after the first root element (another root, text, a comment) is
// ignored; text before it is a parse error.
// Comments, blank lines, extra attributes, a VALUE without a name and any other
// element are read past (and so dropped on the next save). A VALUE without a
// val reads as an empty value. A name that appears twice keeps its first
// position and takes its last value.
//
// Where this deliberately differs from the format's usual handling:
// - A name or value that is not valid UTF-8 (which only a hand-edited file can
//   produce) is written with one U+FFFD reference (&#65533;) per invalid byte,
//   rather than as a control-character reference built from the byte's bits.
// - Fail closed. A file that is not well-formed XML, is empty, or has a root
//   other than PROPERTIES does not load. The bytes are copied once to the
//   backup path (never overwriting an existing backup) and the error is
//   returned, so the file is never wiped by the next save.
// - save() replaces the file atomically and leaves no temporary file behind,
//   whether it succeeds or fails, and reports failure.
// - A save when no entry changed since the load or last save writes nothing:
//   the file's bytes, modification time and inode are untouched.

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulp::state {

struct XmlPropertiesError {
    enum class Kind { io, parse };
    Kind kind = Kind::io;
    std::string message;
    /// For a parse error on load: where the unreadable bytes were copied, or
    /// empty if the copy could not be written.
    std::filesystem::path backup_path;
};

class XmlPropertiesFile {
  public:
    using Entry = std::pair<std::string, std::string>;

    /// An empty file bound to `path`; save() creates it.
    explicit XmlPropertiesFile(std::filesystem::path path);

    /// Reads `path`. A missing file loads empty. A file that does not parse
    /// fails closed: see above.
    static std::optional<XmlPropertiesFile> load(const std::filesystem::path& path,
                                                 XmlPropertiesError* error = nullptr);

    /// Parses `bytes` without touching the disk.
    static std::optional<std::vector<Entry>> parse(std::string_view bytes,
                                                   XmlPropertiesError* error = nullptr);

    /// The canonical bytes for `entries`, in order.
    static std::string serialize(const std::vector<Entry>& entries);

    [[nodiscard]] std::optional<std::string> get(std::string_view name) const;
    [[nodiscard]] bool contains(std::string_view name) const;
    [[nodiscard]] const std::vector<Entry>& entries() const noexcept {
        return entries_;
    }

    /// Sets an existing name in place, or appends a new one at the end.
    void set(std::string_view name, std::string_view value);
    /// Removes `name`; true if it was there.
    bool remove(std::string_view name);

    /// Whether the entries differ from the last load or save.
    [[nodiscard]] bool dirty() const noexcept {
        return entries_ != saved_;
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    /// Where a file that fails to parse is copied: `<path>.bak`.
    [[nodiscard]] static std::filesystem::path backup_path_for(const std::filesystem::path& path);

    /// Writes the canonical bytes atomically if anything changed. True when the
    /// file holds the current entries afterwards.
    bool save(XmlPropertiesError* error = nullptr);

  private:
    std::filesystem::path path_;
    std::vector<Entry> entries_;
    std::vector<Entry> saved_;
};

} // namespace pulp::state
