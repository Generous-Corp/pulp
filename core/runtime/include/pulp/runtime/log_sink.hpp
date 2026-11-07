#pragma once

// Log sinks for <pulp/runtime/log.hpp>.
//
//   FileLogSink     - size-rotated log file in a private directory.
//   CaptureLogSink  - bounded in-memory record list for tests and tooling.
//   ScopedLogSink   - installs a sink for a scope and restores the previous one.
//
// Logging is not realtime-safe, and neither are these sinks: they allocate,
// take a mutex and (for files) perform I/O. Never log from the audio thread.
//
// Every sink here is a cheap shared handle. Copies refer to the same file or
// record list, so a copy can be installed with `set_log_sink` while the original
// stays available for inspection.

#include <pulp/runtime/log.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::runtime {

struct FileLogSinkOptions {
    /// Directory that holds the log. Empty means
    /// `platform::special_dir(SpecialDir::logs) / app_name`. A missing final
    /// directory is created owner-only.
    std::filesystem::path directory;

    /// Names the subdirectory under the platform log directory when
    /// `directory` is empty. A single path component.
    std::string app_name = "pulp";

    /// Log file name inside the directory. A single path component.
    std::string file_name = "pulp.log";

    /// A record that would grow the file past this rotates it first.
    std::uint64_t max_file_bytes = 1024 * 1024;

    /// Live file plus rotated copies kept (`pulp.log`, `pulp.log.1`, ...,
    /// newest rotation first). 1 keeps only the live file and discards it on
    /// rotation. Values below 1 are treated as 1.
    int max_files = 5;

    /// Records below this level are not written.
    LogLevel min_level = LogLevel::Info;
};

/// Appends records to a rotating file.
///
/// Safety: `open` refuses a target it cannot trust, and reports why.
///  - The log directory must be a real directory (not a symlink), owned by the
///    current user, and not writable by group or others.
///  - The log file is opened without following links, must be a regular file
///    owned by the current user with a single hard link, and must not be
///    writable by group or others. Files are created 0600 and tightened to
///    0600 if they were more permissive.
///  - On POSIX the directory is held open and every file operation is relative
///    to it, so replacing the directory with a link after `open` has no effect.
///
/// Windows has no owner or mode bits to check: the sink refuses symlinks and
/// junctions and relies on the inherited ACL of the per-user logs folder.
/// Unavailable under WebAssembly, where `open` fails.
///
/// Records are written one per line:
/// `2026-11-06T17:02:03.123Z [info] category: message`. Line breaks and other
/// control characters inside a record are escaped, so a message cannot forge a
/// second record. Records longer than 8 KiB are truncated.
///
/// Thread-safe; writes are serialised. A failed write is counted and dropped.
class FileLogSink {
  public:
    /// Open (creating if needed) the log. Returns nullopt and fills `error`
    /// when the location cannot be trusted or created.
    static std::optional<FileLogSink> open(FileLogSinkOptions options,
                                           std::string* error = nullptr);

    void write(LogLevel level, std::string_view category, std::string_view message) const;

    /// The sink function for `set_log_sink`.
    LogSink sink() const;
    operator LogSink() const {
        return sink();
    } // NOLINT: intentional conversion

    /// Resolved directory and live file path.
    const std::filesystem::path& directory() const;
    std::filesystem::path path() const;

    /// Path of rotated copy `index` (1 is the newest).
    std::filesystem::path rotated_path(int index) const;

    /// Records that could not be written.
    std::uint64_t dropped_count() const;

    class Impl;

  private:
    explicit FileLogSink(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
    std::shared_ptr<Impl> impl_;
};

struct LogRecord {
    LogLevel level = LogLevel::Info;
    std::string category;
    std::string message;
};

/// Keeps the most recent records in memory. Meant for tests that assert on what
/// was logged and for tooling (an MCP endpoint, an in-app console) that serves
/// recent log lines. When full, the oldest record is discarded and counted.
class CaptureLogSink {
  public:
    explicit CaptureLogSink(std::size_t max_records = 4096);

    void write(LogLevel level, std::string_view category, std::string_view message) const;

    LogSink sink() const;
    operator LogSink() const {
        return sink();
    } // NOLINT: intentional conversion

    /// Snapshot, oldest first.
    std::vector<LogRecord> records() const;
    /// Snapshot of records at or above `min_level`.
    std::vector<LogRecord> records_at_least(LogLevel min_level) const;
    /// Return the records and empty the list.
    std::vector<LogRecord> take();
    void clear();

    std::size_t size() const;
    /// Records discarded because the list was full.
    std::uint64_t dropped_count() const;
    /// True if any retained message contains `needle`.
    bool contains(std::string_view needle) const;

    class Impl;

  private:
    std::shared_ptr<Impl> impl_;
};

/// Installs `sink` for the lifetime of the object and restores the previous
/// sink after. Not nestable across threads: another thread installing a sink in
/// between is overwritten on destruction.
class ScopedLogSink {
  public:
    explicit ScopedLogSink(LogSink sink) : previous_(set_log_sink(std::move(sink))) {}
    ~ScopedLogSink() {
        set_log_sink(std::move(previous_));
    }

    ScopedLogSink(const ScopedLogSink&) = delete;
    ScopedLogSink& operator=(const ScopedLogSink&) = delete;

  private:
    LogSink previous_;
};

} // namespace pulp::runtime
