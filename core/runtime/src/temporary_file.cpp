#include <pulp/runtime/temporary_file.hpp>
#include <random>
#include <sstream>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pulp::runtime {

namespace {

std::string normalize_extension(std::string_view extension) {
    std::string normalized;
    normalized.reserve(extension.size());
    for (char c : extension) {
        normalized += (c == '/' || c == '\\') ? '_' : c;
    }
    return normalized;
}

void append_extension(std::ostringstream& stream, std::string_view extension) {
    if (extension.empty())
        return;

    if (extension[0] != '.')
        stream << '.';
    stream << normalize_extension(extension);
}

}  // namespace

TemporaryFile::TemporaryFile(std::string_view extension) {
    auto tmp_dir = std::filesystem::temp_directory_path();

    // Generate a unique filename
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;

    for (int attempt = 0; attempt < 100; ++attempt) {
        std::ostringstream ss;
        ss << "pulp_" << std::hex << dist(gen);
        append_extension(ss, extension);

        auto candidate = tmp_dir / ss.str();
        // Exclusive creation reserves the path atomically: a name that
        // appears between a probe and the create is reported as taken
        // instead of being truncated.
        if (create_exclusive(candidate)) {
            path_ = candidate;
            return;
        }
    }

    // Fallback: one more random name. The path is recorded only when this
    // object created it, so the destructor never removes a file it does not
    // own; an empty path() reports the failure.
    std::ostringstream ss;
    ss << "pulp_tmp_" << std::hex << std::random_device{}();
    append_extension(ss, extension);
    auto candidate = tmp_dir / ss.str();
    if (create_exclusive(candidate))
        path_ = candidate;
}

bool TemporaryFile::create_exclusive(const std::filesystem::path& path) {
#ifdef _WIN32
    int fd = -1;
    if (_wsopen_s(&fd, path.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _SH_DENYNO,
                  _S_IREAD | _S_IWRITE) != 0 ||
        fd < 0) {
        return false;
    }
    _close(fd);
    return true;
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    ::close(fd);
    return true;
#endif
}

TemporaryFile::~TemporaryFile() {
    if (active_ && !path_.empty()) {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
}

TemporaryFile::TemporaryFile(TemporaryFile&& other) noexcept
    : path_(std::move(other.path_)), active_(other.active_) {
    other.active_ = false;
}

TemporaryFile& TemporaryFile::operator=(TemporaryFile&& other) noexcept {
    if (this != &other) {
        if (active_ && !path_.empty()) {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
        }
        path_ = std::move(other.path_);
        active_ = other.active_;
        other.active_ = false;
    }
    return *this;
}

}  // namespace pulp::runtime
