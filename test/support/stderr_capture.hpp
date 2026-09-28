// Redirects the process's stderr (where runtime::log_* always writes) into a
// temporary file for the lifetime of the object, so a test can assert on the
// exact log lines a code path emits. POSIX only.
#pragma once

#if !defined(_WIN32)

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace pulp::test {

class StderrCapture {
  public:
    StderrCapture() {
        path_ = (std::filesystem::temp_directory_path() /
                 ("pulp_test_stderr_" + std::to_string(::getpid()) + ".log"))
                    .string();
        std::fflush(stderr);
        saved_ = ::dup(STDERR_FILENO);
        if (std::FILE* file = std::fopen(path_.c_str(), "w")) {
            ::dup2(::fileno(file), STDERR_FILENO);
            std::fclose(file);
        }
    }
    ~StderrCapture() {
        restore();
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    StderrCapture(const StderrCapture&) = delete;
    StderrCapture& operator=(const StderrCapture&) = delete;

    /// Stop capturing and return everything written since construction.
    std::string text() {
        restore();
        std::ifstream in(path_);
        std::stringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

  private:
    void restore() {
        if (saved_ < 0)
            return;
        std::fflush(stderr);
        ::dup2(saved_, STDERR_FILENO);
        ::close(saved_);
        saved_ = -1;
    }

    std::string path_;
    int saved_ = -1;
};

} // namespace pulp::test

#endif // !_WIN32
