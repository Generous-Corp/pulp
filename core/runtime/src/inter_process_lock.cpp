#include <filesystem>
#include <pulp/runtime/inter_process_lock.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pulp::runtime {

InterProcessLock::InterProcessLock(std::string_view name) {
    auto tmp = std::filesystem::temp_directory_path();
#ifndef _WIN32
    const auto owner = static_cast<std::uint64_t>(::geteuid());
    const auto directory = tmp / ("pulp-locks-" + std::to_string(owner));
    struct stat status {};
    if (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST)
        return;
    if (::lstat(directory.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) ||
        status.st_uid != ::geteuid() || (status.st_mode & 0077) != 0)
        return;
    tmp = directory;
#endif
    lock_path_ = (tmp / ("pulp_lock_" + std::string(name))).string();
}

InterProcessLock::~InterProcessLock() {
    unlock();
}

#ifdef _WIN32

bool InterProcessLock::try_lock() {
    if (mode_ == Mode::Exclusive)
        return true;
    if (mode_ == Mode::Shared)
        return false;

    handle_ = CreateFileA(lock_path_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                          FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        handle_ = nullptr;
        return false;
    }
    mode_ = Mode::Exclusive;
    return true;
}

bool InterProcessLock::try_lock_shared() {
    if (mode_ != Mode::Unlocked)
        return true;
    handle_ = CreateFileA(lock_path_.c_str(), GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                          FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        handle_ = nullptr;
        return false;
    }
    mode_ = Mode::Shared;
    return true;
}

void InterProcessLock::unlock() {
    if (handle_) {
        CloseHandle(handle_);
        handle_ = nullptr;
    }
    mode_ = Mode::Unlocked;
}

#else // POSIX

namespace {

int open_lock_file(const std::string& path) noexcept {
    if (path.empty())
        return -1;
    const int descriptor =
        ::open(path.c_str(), O_CREAT | O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (descriptor < 0)
        return -1;
    struct stat status {};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_uid != ::geteuid()) {
        ::close(descriptor);
        return -1;
    }
    return descriptor;
}

} // namespace

bool InterProcessLock::try_lock() {
    if (mode_ == Mode::Exclusive)
        return true;
    if (mode_ == Mode::Shared)
        return false;

    fd_ = open_lock_file(lock_path_);
    if (fd_ < 0)
        return false;

    if (flock(fd_, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    mode_ = Mode::Exclusive;
    return true;
}

bool InterProcessLock::try_lock_shared() {
    if (mode_ != Mode::Unlocked)
        return true;
    fd_ = open_lock_file(lock_path_);
    if (fd_ < 0)
        return false;
    if (flock(fd_, LOCK_SH | LOCK_NB) != 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    mode_ = Mode::Shared;
    return true;
}

void InterProcessLock::unlock() {
    if (fd_ >= 0) {
        flock(fd_, LOCK_UN);
        ::close(fd_);
        fd_ = -1;
        // Keep the inode stable. Unlinking here allows a waiter on the old
        // inodeanda newcomerona recreated pathname to hold two locks.
    }
    mode_ = Mode::Unlocked;
}

#endif

} // namespace pulp::runtime
