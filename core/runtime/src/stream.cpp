#include <pulp/runtime/stream.hpp>
#include <pulp/runtime/named_pipe.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>

namespace pulp::runtime {

// ─── FileStream ───────────────────────────────────────────────────────────

namespace {
const char* mode_string(FileStream::Mode mode) {
    switch (mode) {
        case FileStream::Mode::Read:      return "rb";
        case FileStream::Mode::Write:     return "wb";
        case FileStream::Mode::Append:    return "ab";
        case FileStream::Mode::ReadWrite: return "w+b";
    }
    return "rb";
}
}  // namespace

FileStream::FileStream(std::string_view path, Mode mode) {
    open(path, mode);
}

FileStream::~FileStream() {
    close();
}

FileStream::FileStream(FileStream&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
}

FileStream& FileStream::operator=(FileStream&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

bool FileStream::open(std::string_view path, Mode mode) {
    close();
    std::string path_str(path);
    handle_ = std::fopen(path_str.c_str(), mode_string(mode));
    return handle_ != nullptr;
}

StreamResult FileStream::read(std::uint8_t* buffer, std::size_t size) {
    if (!handle_) return StreamResult::fail(StreamError::Closed);
    if (size == 0) return StreamResult::make(0);
    if (buffer == nullptr) return StreamResult::fail(StreamError::Invalid);

    auto* fp = static_cast<std::FILE*>(handle_);
    std::size_t n = std::fread(buffer, 1, size, fp);
    if (n == 0) {
        if (std::feof(fp)) return StreamResult::fail(StreamError::Closed);
        if (std::ferror(fp)) return StreamResult::fail(StreamError::IoError);
    }
    return StreamResult::make(n);
}

StreamResult FileStream::write(const std::uint8_t* buffer, std::size_t size) {
    if (!handle_) return StreamResult::fail(StreamError::Closed);
    if (size == 0) return StreamResult::make(0);
    if (buffer == nullptr) return StreamResult::fail(StreamError::Invalid);

    auto* fp = static_cast<std::FILE*>(handle_);
    std::size_t n = std::fwrite(buffer, 1, size, fp);
    if (n < size) {
        return n > 0 ? StreamResult::make(n) : StreamResult::fail(StreamError::IoError);
    }
    return StreamResult::make(n);
}

void FileStream::close() {
    if (handle_) {
        std::fclose(static_cast<std::FILE*>(handle_));
        handle_ = nullptr;
    }
}

bool FileStream::flush() {
    if (!handle_) return false;
    return std::fflush(static_cast<std::FILE*>(handle_)) == 0;
}

std::size_t FileStream::position() const {
    if (!handle_) return static_cast<std::size_t>(-1);
    auto pos = std::ftell(static_cast<std::FILE*>(handle_));
    if (pos < 0) return static_cast<std::size_t>(-1);
    return static_cast<std::size_t>(pos);
}

// ─── MemoryStream ─────────────────────────────────────────────────────────

MemoryStream::MemoryStream(std::vector<std::uint8_t> initial)
    : buffer_(std::move(initial)) {}

StreamResult MemoryStream::read(std::uint8_t* buffer, std::size_t size) {
    if (!open_) return StreamResult::fail(StreamError::Closed);
    if (size == 0) return StreamResult::make(0);
    if (buffer == nullptr) return StreamResult::fail(StreamError::Invalid);
    if (read_pos_ >= buffer_.size()) return StreamResult::fail(StreamError::Closed);

    std::size_t available = buffer_.size() - read_pos_;
    std::size_t n = std::min(size, available);
    std::memcpy(buffer, buffer_.data() + read_pos_, n);
    read_pos_ += n;
    return StreamResult::make(n);
}

StreamResult MemoryStream::write(const std::uint8_t* buffer, std::size_t size) {
    if (!open_) return StreamResult::fail(StreamError::Closed);
    if (size == 0) return StreamResult::make(0);
    if (buffer == nullptr) return StreamResult::fail(StreamError::Invalid);
    buffer_.insert(buffer_.end(), buffer, buffer + size);
    return StreamResult::make(size);
}

void MemoryStream::clear() {
    buffer_.clear();
    read_pos_ = 0;
}

// ─── PipeStream ───────────────────────────────────────────────────────────

PipeStream::PipeStream(std::unique_ptr<NamedPipe> pipe) : pipe_(std::move(pipe)) {}

PipeStream::~PipeStream() = default;

PipeStream::PipeStream(PipeStream&& other) noexcept : pipe_(std::move(other.pipe_)) {}
PipeStream& PipeStream::operator=(PipeStream&& other) noexcept {
    if (this != &other) pipe_ = std::move(other.pipe_);
    return *this;
}

StreamResult PipeStream::read(std::uint8_t* buffer, std::size_t size) {
    if (!pipe_ || !pipe_->is_open()) return StreamResult::fail(StreamError::Closed);
    if (size == 0) return StreamResult::make(0);
    if (buffer == nullptr) return StreamResult::fail(StreamError::Invalid);

    int n = pipe_->read(buffer, size);
    if (n < 0) return StreamResult::fail(StreamError::IoError);
    if (n == 0) return StreamResult::fail(StreamError::Closed);
    return StreamResult::make(static_cast<std::size_t>(n));
}

StreamResult PipeStream::write(const std::uint8_t* buffer, std::size_t size) {
    if (!pipe_ || !pipe_->is_open()) return StreamResult::fail(StreamError::Closed);
    if (size == 0) return StreamResult::make(0);
    if (buffer == nullptr) return StreamResult::fail(StreamError::Invalid);

    int n = pipe_->write(buffer, size);
    if (n < 0) return StreamResult::fail(StreamError::IoError);
    return StreamResult::make(static_cast<std::size_t>(n));
}

void PipeStream::close() {
    if (pipe_) pipe_->close();
}

bool PipeStream::is_open() const {
    return pipe_ && pipe_->is_open();
}

}  // namespace pulp::runtime
