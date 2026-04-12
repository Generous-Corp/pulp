#pragma once

// Unified Stream interface for bytewise I/O.
//
// A `Stream` presents a common surface across files, memory buffers, pipes,
// sockets, and HTTP bodies. It is synchronous and byte-oriented; async
// behavior is layered on top via `AsyncStream`, which dispatches Stream
// operations on a background worker and delivers completions through the
// `EventLoop`.
//
// The interface is minimal on purpose:
//   - `read(buf, n)`  — non-blocking-style read; may return 0 with
//                        StreamError::WouldBlock when no data is available.
//   - `write(buf, n)` — returns bytes written; may be partial.
//   - `close()`        — idempotent; safe to call on a closed stream.
//   - `is_open()`      — whether the stream is currently usable.
//
// Streams never throw from `read`/`write`. Errors surface as
// `StreamError != Ok` in the returned `StreamResult`.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::runtime {

/// High-level error classification for Stream operations.
enum class StreamError : uint8_t {
    Ok,          ///< No error; `bytes` is authoritative.
    Closed,      ///< End-of-stream / peer closed / stream not open.
    WouldBlock,  ///< No data available now; try again later.
    IoError,     ///< OS-level I/O failure (errno/GetLastError set by the backend).
    Invalid,     ///< Invalid argument or invalid state (e.g., read after close).
};

/// Result of a Stream read/write.
///
/// Consumers should check `ok()` first. On `ok()`:
///   - `bytes == 0` means "end of stream" for read, "no room" for write
///     (backends that do not distinguish will report `Closed` / `WouldBlock`).
///   - `bytes > 0` means that many bytes were transferred.
struct StreamResult {
    std::size_t bytes = 0;
    StreamError error = StreamError::Ok;

    constexpr bool ok() const { return error == StreamError::Ok; }
    constexpr bool would_block() const { return error == StreamError::WouldBlock; }
    constexpr bool closed() const { return error == StreamError::Closed; }

    static constexpr StreamResult make(std::size_t n) { return {n, StreamError::Ok}; }
    static constexpr StreamResult fail(StreamError e) { return {0, e}; }
};

/// Base class for all Pulp byte streams.
///
/// Implementations must be safe to destroy from any thread, but individual
/// instances are not thread-safe: callers coordinate access (typically the
/// owning `AsyncStream` serializes on a worker thread).
class Stream {
public:
    virtual ~Stream() = default;

    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    /// Read up to `size` bytes into `buffer`. Returns how many bytes were
    /// written, or an error. `buffer` may be partially filled on error.
    virtual StreamResult read(std::uint8_t* buffer, std::size_t size) = 0;

    /// Write up to `size` bytes from `buffer`. Returns how many bytes were
    /// consumed. Partial writes are permitted — callers loop until satisfied
    /// or treat the short return as backpressure.
    virtual StreamResult write(const std::uint8_t* buffer, std::size_t size) = 0;

    /// Close the stream. Idempotent.
    virtual void close() = 0;

    /// True while the stream is usable for read/write.
    virtual bool is_open() const = 0;

protected:
    Stream() = default;
};

/// Stream backed by `std::FILE*` (for portability across Windows/POSIX).
///
/// Not async-friendly by itself — use `AsyncStream` to dispatch reads/writes
/// to a worker thread.
class FileStream : public Stream {
public:
    enum class Mode : uint8_t { Read, Write, Append, ReadWrite };

    FileStream() = default;
    explicit FileStream(std::string_view path, Mode mode = Mode::Read);
    ~FileStream() override;

    FileStream(FileStream&& other) noexcept;
    FileStream& operator=(FileStream&& other) noexcept;

    /// Open `path` with the given mode. Returns true on success.
    bool open(std::string_view path, Mode mode = Mode::Read);

    StreamResult read(std::uint8_t* buffer, std::size_t size) override;
    StreamResult write(const std::uint8_t* buffer, std::size_t size) override;
    void close() override;
    bool is_open() const override { return handle_ != nullptr; }

    /// Flush OS-level buffers to the underlying file.
    bool flush();

    /// Current byte position, or (size_t)-1 on error.
    std::size_t position() const;

private:
    void* handle_ = nullptr;  // std::FILE*
};

/// Stream backed by an in-memory byte buffer. Useful for tests and for
/// round-trips where no actual I/O is required.
class MemoryStream : public Stream {
public:
    MemoryStream() = default;

    /// Pre-fill the stream with existing bytes (copied in).
    explicit MemoryStream(std::vector<std::uint8_t> initial);

    StreamResult read(std::uint8_t* buffer, std::size_t size) override;
    StreamResult write(const std::uint8_t* buffer, std::size_t size) override;
    void close() override { open_ = false; }
    bool is_open() const override { return open_; }

    /// Byte content currently in the stream (for inspection in tests).
    const std::vector<std::uint8_t>& buffer() const { return buffer_; }

    /// Reset the read cursor to the start.
    void rewind() { read_pos_ = 0; }

    /// Clear all data and rewind.
    void clear();

    std::size_t size() const { return buffer_.size(); }
    std::size_t read_position() const { return read_pos_; }

private:
    std::vector<std::uint8_t> buffer_;
    std::size_t read_pos_ = 0;
    bool open_ = true;
};

// Forward declaration — full definition requires named_pipe.hpp.
class NamedPipe;

/// Stream wrapper over `NamedPipe`. Takes ownership of an already-connected
/// pipe; callers open the pipe (server/client) before constructing.
class PipeStream : public Stream {
public:
    PipeStream() = default;
    explicit PipeStream(std::unique_ptr<NamedPipe> pipe);
    ~PipeStream() override;

    PipeStream(PipeStream&&) noexcept;
    PipeStream& operator=(PipeStream&&) noexcept;

    StreamResult read(std::uint8_t* buffer, std::size_t size) override;
    StreamResult write(const std::uint8_t* buffer, std::size_t size) override;
    void close() override;
    bool is_open() const override;

    NamedPipe* pipe() { return pipe_.get(); }

private:
    std::unique_ptr<NamedPipe> pipe_;
};

}  // namespace pulp::runtime
