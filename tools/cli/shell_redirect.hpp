// shell_redirect.hpp — shell redirection snippets shared by CLI commands.
#pragma once

#include <string>

const char* stderr_to_null();
const char* output_to_null();

class ScopedStdoutRedirect {
public:
    explicit ScopedStdoutRedirect(bool redirect);
    ~ScopedStdoutRedirect();

    bool ready() const { return active_; }
    bool write_original(const std::string& text) const;

    ScopedStdoutRedirect(const ScopedStdoutRedirect&) = delete;
    ScopedStdoutRedirect& operator=(const ScopedStdoutRedirect&) = delete;

private:
    void close_saved_fd();

    int saved_fd_ = -1;
    bool active_ = false;
};
