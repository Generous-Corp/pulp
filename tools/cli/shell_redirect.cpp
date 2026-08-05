#include "shell_redirect.hpp"

#include <cerrno>
#include <cstdio>
#include <iostream>

#if defined(_WIN32)
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

const char* stderr_to_null() {
#if defined(_WIN32)
    return " 2>NUL";
#else
    return " 2>/dev/null";
#endif
}

const char* output_to_null() {
#if defined(_WIN32)
    return " >NUL 2>&1";
#else
    return " >/dev/null 2>&1";
#endif
}

ScopedStdoutRedirect::ScopedStdoutRedirect(bool redirect) {
    if (!redirect) return;
    std::cout.flush();
    std::fflush(stdout);
#if defined(_WIN32)
    saved_fd_ = _dup(_fileno(stdout));
    active_ = saved_fd_ >= 0 && _dup2(_fileno(stderr), _fileno(stdout)) == 0;
#else
    saved_fd_ = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 0);
    active_ = saved_fd_ >= 0 && dup2(STDERR_FILENO, STDOUT_FILENO) >= 0;
#endif
    if (!active_ && saved_fd_ >= 0) close_saved_fd();
}

ScopedStdoutRedirect::~ScopedStdoutRedirect() {
    if (!active_) return;
    std::cout.flush();
    std::fflush(stdout);
#if defined(_WIN32)
    (void)_dup2(saved_fd_, _fileno(stdout));
#else
    (void)dup2(saved_fd_, STDOUT_FILENO);
#endif
    close_saved_fd();
}

bool ScopedStdoutRedirect::write_original(const std::string& text) const {
    if (!active_) return false;
    std::size_t written = 0;
    while (written < text.size()) {
#if defined(_WIN32)
        const auto count = _write(
            saved_fd_, text.data() + written,
            static_cast<unsigned int>(text.size() - written));
#else
        const auto count = write(
            saved_fd_, text.data() + written, text.size() - written);
#endif
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        written += static_cast<std::size_t>(count);
    }
    return true;
}

void ScopedStdoutRedirect::close_saved_fd() {
#if defined(_WIN32)
    (void)_close(saved_fd_);
#else
    (void)close(saved_fd_);
#endif
    saved_fd_ = -1;
}
