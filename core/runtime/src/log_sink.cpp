#include <pulp/runtime/log_sink.hpp>

#include <pulp/platform/special_dir.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <iterator>
#include <mutex>
#include <system_error>
#include <utility>

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#define PULP_LOG_SINK_POSIX 1
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#else
#include <fstream>
#endif

namespace pulp::runtime {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxRecordBytes = 8 * 1024;

const char* level_tag(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warning:
        return "warn";
    case LogLevel::Error:
        return "error";
    }
    return "info";
}

// One component, no separators, not "." or "..", printable ASCII only.
bool is_single_component(std::string_view name) {
    if (name.empty() || name.size() > 200 || name == "." || name == "..")
        return false;
    for (unsigned char c : name) {
        if (c < 0x20 || c == 0x7f || c == '/' || c == '\\' || c == ':')
            return false;
    }
    return true;
}

void append_escaped(std::string& out, std::string_view text) {
    static constexpr char kHex[] = "0123456789abcdef";
    for (unsigned char c : text) {
        if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else if (c == '\t')
            out += '\t';
        else if (c < 0x20 || c == 0x7f) {
            out += "\\x";
            out += kHex[c >> 4];
            out += kHex[c & 0xf];
        } else {
            out += static_cast<char>(c);
        }
    }
}

std::string format_record(LogLevel level, std::string_view category, std::string_view message) {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count();
    const std::time_t secs = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &secs);
#else
    gmtime_r(&secs, &tm);
#endif
    char stamp[40];
    std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(ms % 1000));

    std::string line;
    line.reserve(64 + category.size() + message.size());
    line += stamp;
    line += " [";
    line += level_tag(level);
    line += "] ";
    if (!category.empty()) {
        append_escaped(line, category);
        line += ": ";
    }
    append_escaped(line, message);
    if (line.size() > kMaxRecordBytes) {
        line.resize(kMaxRecordBytes - 3);
        line += "...";
    }
    line += '\n';
    return line;
}

void set_error(std::string* error, std::string text) {
    if (error)
        *error = std::move(text);
}

} // namespace

// ═══ FileLogSink ═════════════════════════════════════════════════════════════

class FileLogSink::Impl {
  public:
    FileLogSinkOptions options;
    fs::path dir;

    std::mutex mutex;
    std::uint64_t size = 0;
    std::uint64_t dropped = 0;

    std::string rotated_name(int index) const {
        return options.file_name + "." + std::to_string(index);
    }

    void write(LogLevel level, std::string_view category, std::string_view message) {
        if (level < options.min_level)
            return;
        const std::string line = format_record(level, category, message);
        std::lock_guard<std::mutex> lock(mutex);
        if (size > 0 && size + line.size() > options.max_file_bytes)
            rotate_locked();
        if (!append_locked(line))
            ++dropped;
    }

    std::uint64_t dropped_count() {
        std::lock_guard<std::mutex> lock(mutex);
        return dropped;
    }

    // Platform hooks; defined below.
    bool open_dir(std::string* error);
    bool open_file_locked(std::string* error);
    bool append_locked(const std::string& line);
    void rotate_locked();
    ~Impl();

#if defined(PULP_LOG_SINK_POSIX)
    int dir_fd = -1;
    int fd = -1;
#else
    std::ofstream stream;
#endif
};

#if defined(PULP_LOG_SINK_POSIX)

namespace {

bool unsafe_mode(mode_t mode) {
    return (mode & (S_IWGRP | S_IWOTH)) != 0;
}

} // namespace

FileLogSink::Impl::~Impl() {
    if (fd >= 0)
        ::close(fd);
    if (dir_fd >= 0)
        ::close(dir_fd);
}

bool FileLogSink::Impl::open_dir(std::string* error) {
    // Create missing parents with default permissions, then the leaf owner-only.
    std::error_code ec;
    const fs::path parent = dir.parent_path();
    if (!parent.empty())
        fs::create_directories(parent, ec);
    if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        set_error(error,
                  "cannot create log directory " + dir.string() + ": " + std::strerror(errno));
        return false;
    }
    dir_fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir_fd < 0) {
        set_error(error,
                  errno == ELOOP || errno == ENOTDIR
                      ? "log directory is a symlink or not a directory: " + dir.string()
                      : "cannot open log directory " + dir.string() + ": " + std::strerror(errno));
        return false;
    }
    struct stat st{};
    if (::fstat(dir_fd, &st) != 0 || !S_ISDIR(st.st_mode)) {
        set_error(error, "log directory is not a directory: " + dir.string());
        return false;
    }
    if (st.st_uid != ::geteuid()) {
        set_error(error, "log directory is not owned by the current user: " + dir.string());
        return false;
    }
    if (unsafe_mode(st.st_mode)) {
        set_error(error, "log directory is writable by group or others: " + dir.string());
        return false;
    }
    return true;
}

bool FileLogSink::Impl::open_file_locked(std::string* error) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
    const int opened = ::openat(dir_fd, options.file_name.c_str(),
                                O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (opened < 0) {
        set_error(error, errno == ELOOP
                             ? "log file is a symlink: " + (dir / options.file_name).string()
                             : "cannot open log file " + (dir / options.file_name).string() + ": " +
                                   std::strerror(errno));
        return false;
    }
    struct stat st{};
    if (::fstat(opened, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(opened);
        set_error(error, "log file is not a regular file: " + (dir / options.file_name).string());
        return false;
    }
    if (st.st_uid != ::geteuid()) {
        ::close(opened);
        set_error(error, "log file is not owned by the current user: " +
                             (dir / options.file_name).string());
        return false;
    }
    if (st.st_nlink != 1) {
        ::close(opened);
        set_error(error, "log file has multiple hard links: " + (dir / options.file_name).string());
        return false;
    }
    if (unsafe_mode(st.st_mode)) {
        ::close(opened);
        set_error(error,
                  "log file is writable by group or others: " + (dir / options.file_name).string());
        return false;
    }
    if ((st.st_mode & 0777) != 0600)
        ::fchmod(opened, 0600); // we own it: tighten readers
    fd = opened;
    size = static_cast<std::uint64_t>(st.st_size);
    return true;
}

bool FileLogSink::Impl::append_locked(const std::string& line) {
    if (fd < 0 && !open_file_locked(nullptr))
        return false;
    const char* p = line.data();
    std::size_t left = line.size();
    while (left > 0) {
        const ssize_t n = ::write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ::close(fd);
            fd = -1; // reopen (and re-validate) on the next record
            return false;
        }
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    size += line.size();
    return true;
}

void FileLogSink::Impl::rotate_locked() {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
    const int keep = std::max(1, options.max_files);
    if (keep == 1) {
        ::unlinkat(dir_fd, options.file_name.c_str(), 0);
    } else {
        ::unlinkat(dir_fd, rotated_name(keep - 1).c_str(), 0);
        for (int i = keep - 2; i >= 1; --i) {
            ::renameat(dir_fd, rotated_name(i).c_str(), dir_fd, rotated_name(i + 1).c_str());
        }
        ::renameat(dir_fd, options.file_name.c_str(), dir_fd, rotated_name(1).c_str());
    }
    size = 0;
    open_file_locked(nullptr);
}

#else // Windows and anything without POSIX file descriptors

FileLogSink::Impl::~Impl() = default;

namespace {

bool is_link(const fs::path& p) {
    std::error_code ec;
    const auto status = fs::symlink_status(p, ec);
    if (ec)
        return false;
    if (status.type() == fs::file_type::symlink)
        return true;
#if defined(_WIN32)
    if (status.type() == fs::file_type::junction)
        return true;
#endif
    return false;
}

} // namespace

bool FileLogSink::Impl::open_dir(std::string* error) {
#if defined(__EMSCRIPTEN__)
    set_error(error, "file logging is not available on this platform");
    return false;
#else
    std::error_code ec;
    if (is_link(dir)) {
        set_error(error, "log directory is a symlink: " + dir.string());
        return false;
    }
    fs::create_directories(dir, ec);
    if (ec || !fs::is_directory(dir, ec) || is_link(dir)) {
        set_error(error, "cannot create log directory " + dir.string());
        return false;
    }
    return true;
#endif
}

bool FileLogSink::Impl::open_file_locked(std::string* error) {
    if (stream.is_open())
        stream.close();
    const fs::path file = dir / options.file_name;
    if (is_link(file)) {
        set_error(error, "log file is a symlink: " + file.string());
        return false;
    }
    std::error_code ec;
    const auto existing = fs::file_size(file, ec);
    stream.open(file, std::ios::binary | std::ios::app);
    if (!stream.is_open()) {
        set_error(error, "cannot open log file " + file.string());
        return false;
    }
    size = ec ? 0 : static_cast<std::uint64_t>(existing);
    return true;
}

bool FileLogSink::Impl::append_locked(const std::string& line) {
    if (!stream.is_open() && !open_file_locked(nullptr))
        return false;
    stream.write(line.data(), static_cast<std::streamsize>(line.size()));
    stream.flush();
    if (!stream) {
        stream.close();
        stream.clear();
        return false;
    }
    size += line.size();
    return true;
}

void FileLogSink::Impl::rotate_locked() {
    if (stream.is_open())
        stream.close();
    stream.clear();
    std::error_code ec;
    const int keep = std::max(1, options.max_files);
    if (keep == 1) {
        fs::remove(dir / options.file_name, ec);
    } else {
        fs::remove(dir / rotated_name(keep - 1), ec);
        for (int i = keep - 2; i >= 1; --i)
            fs::rename(dir / rotated_name(i), dir / rotated_name(i + 1), ec);
        fs::rename(dir / options.file_name, dir / rotated_name(1), ec);
    }
    size = 0;
    open_file_locked(nullptr);
}

#endif

std::optional<FileLogSink> FileLogSink::open(FileLogSinkOptions options, std::string* error) {
    if (!is_single_component(options.file_name)) {
        set_error(error, "invalid log file name");
        return std::nullopt;
    }
    options.max_files = std::max(1, options.max_files);
    if (options.max_file_bytes == 0)
        options.max_file_bytes = 1;

    fs::path dir = options.directory;
    if (dir.empty()) {
        if (!is_single_component(options.app_name)) {
            set_error(error, "invalid application name for the log directory");
            return std::nullopt;
        }
        const fs::path base = platform::special_dir(platform::SpecialDir::logs);
        if (base.empty()) {
            set_error(error, "no per-user log directory is available");
            return std::nullopt;
        }
        dir = base / options.app_name;
    }

    // A trailing separator makes open() resolve a final symlink despite
    // O_NOFOLLOW, so name the directory by its last real component.
    while (!dir.has_filename() && dir.has_relative_path())
        dir = dir.parent_path();

    auto impl = std::make_shared<Impl>();
    impl->options = std::move(options);
    impl->dir = std::move(dir);
    if (!impl->open_dir(error))
        return std::nullopt;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        if (!impl->open_file_locked(error))
            return std::nullopt;
    }
    return FileLogSink(std::move(impl));
}

void FileLogSink::write(LogLevel level, std::string_view category, std::string_view message) const {
    impl_->write(level, category, message);
}

LogSink FileLogSink::sink() const {
    return [impl = impl_](LogLevel level, std::string_view category, std::string_view message) {
        impl->write(level, category, message);
    };
}

const fs::path& FileLogSink::directory() const {
    return impl_->dir;
}
fs::path FileLogSink::path() const {
    return impl_->dir / impl_->options.file_name;
}
fs::path FileLogSink::rotated_path(int index) const {
    return impl_->dir / impl_->rotated_name(index);
}
std::uint64_t FileLogSink::dropped_count() const {
    return impl_->dropped_count();
}

// ═══ CaptureLogSink ══════════════════════════════════════════════════════════

class CaptureLogSink::Impl {
  public:
    explicit Impl(std::size_t max) : max_records(std::max<std::size_t>(1, max)) {}
    const std::size_t max_records;
    mutable std::mutex mutex;
    std::deque<LogRecord> records;
    std::uint64_t dropped = 0;

    void push(LogLevel level, std::string_view category, std::string_view message) {
        LogRecord record{level, std::string(category), std::string(message)};
        std::lock_guard<std::mutex> lock(mutex);
        if (records.size() >= max_records) {
            records.pop_front();
            ++dropped;
        }
        records.push_back(std::move(record));
    }
};

CaptureLogSink::CaptureLogSink(std::size_t max_records)
    : impl_(std::make_shared<Impl>(max_records)) {}

void CaptureLogSink::write(LogLevel level, std::string_view category,
                           std::string_view message) const {
    impl_->push(level, category, message);
}

LogSink CaptureLogSink::sink() const {
    return [impl = impl_](LogLevel level, std::string_view category, std::string_view message) {
        impl->push(level, category, message);
    };
}

std::vector<LogRecord> CaptureLogSink::records() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return {impl_->records.begin(), impl_->records.end()};
}

std::vector<LogRecord> CaptureLogSink::records_at_least(LogLevel min_level) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<LogRecord> out;
    for (const auto& r : impl_->records)
        if (r.level >= min_level)
            out.push_back(r);
    return out;
}

std::vector<LogRecord> CaptureLogSink::take() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<LogRecord> out(std::make_move_iterator(impl_->records.begin()),
                               std::make_move_iterator(impl_->records.end()));
    impl_->records.clear();
    return out;
}

void CaptureLogSink::clear() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->records.clear();
}

std::size_t CaptureLogSink::size() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->records.size();
}

std::uint64_t CaptureLogSink::dropped_count() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->dropped;
}

bool CaptureLogSink::contains(std::string_view needle) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& r : impl_->records)
        if (r.message.find(needle) != std::string::npos)
            return true;
    return false;
}

} // namespace pulp::runtime
