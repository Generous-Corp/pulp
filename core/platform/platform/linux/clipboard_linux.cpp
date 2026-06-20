// Linux clipboard implementation — shells out to system clipboard
// utilities (`wl-copy`/`wl-paste` on Wayland, `xclip`/`xsel` on X11).
//
// This implementation either really writes to the system clipboard or returns
// false so callers can detect the unsupported case. No in-process shadow
// storage: if the tool is missing we say so honestly.
//
// Tooling detection is lazy (first-call stat) and cached. The tool
// chosen at startup is the one used for the lifetime of the process —
// session-type changes mid-run would need a restart, which is fine
// for a plugin host.

#include <pulp/platform/clipboard.hpp>

#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace pulp::platform {

namespace {

enum class Backend {
    none,
    wl_clipboard,   // wl-copy + wl-paste (Wayland)
    xclip,          // xclip (X11)
    xsel,           // xsel (X11 alt)
};

struct Tools {
    Backend backend = Backend::none;
    std::string copy_cmd;   // e.g. "wl-copy"
    std::string paste_cmd;  // e.g. "wl-paste"
};

// True if `cmd` resolves on PATH.
bool which_exists(const char* cmd) {
    std::string probe = std::string("command -v ") + cmd + " >/dev/null 2>&1";
    return std::system(probe.c_str()) == 0;
}

const Tools& resolve_tools() {
    static const Tools tools = []() -> Tools {
        // Prefer Wayland when $WAYLAND_DISPLAY is set AND wl-copy exists;
        // fall back to X11 tools otherwise.
        const char* wayland = std::getenv("WAYLAND_DISPLAY");
        if (wayland && *wayland && which_exists("wl-copy") && which_exists("wl-paste")) {
            // wl-paste appends a newline by default. Pass -n so get_text()
            // returns the exact bytes wl-copy received. xclip/xsel preserve
            // bytes without a flag; only Wayland needs this.
            return {Backend::wl_clipboard, "wl-copy", "wl-paste -n"};
        }
        if (which_exists("xclip")) {
            return {Backend::xclip, "xclip -selection clipboard -in",
                                    "xclip -selection clipboard -out"};
        }
        if (which_exists("xsel")) {
            return {Backend::xsel, "xsel --clipboard --input",
                                   "xsel --clipboard --output"};
        }
        return {Backend::none, {}, {}};
    }();
    return tools;
}

// Pipe `data` to `cmd`'s stdin; return true if the command exited 0.
bool pipe_to_command(const std::string& cmd, std::string_view data) {
    // A child that exits before draining stdin (e.g. xclip with no X display,
    // or a large binary payload to a tool that errors early) closes the pipe's
    // read end; the next fwrite would then raise SIGPIPE and kill the whole
    // process. Ignore SIGPIPE for the duration of the write so fwrite instead
    // fails with EPIPE and we report an honest false. Save/restore the prior
    // disposition so we don't change process-global behavior beyond this call
    // (the clipboard mutex already serializes writers).
    struct sigaction ignore{}, prev{};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    ::sigaction(SIGPIPE, &ignore, &prev);

    FILE* p = ::popen(cmd.c_str(), "w");
    bool ok = false;
    if (p) {
        const size_t wrote = std::fwrite(data.data(), 1, data.size(), p);
        const int rc = ::pclose(p);
        ok = (wrote == data.size()) && (rc == 0);
    }

    ::sigaction(SIGPIPE, &prev, nullptr);
    return ok;
}

// Capture stdout of `cmd`. Returns nullopt on failure.
std::optional<std::string> capture_command(const std::string& cmd) {
    FILE* p = ::popen(cmd.c_str(), "r");
    if (!p) return std::nullopt;
    std::string out;
    std::array<char, 4096> buf{};
    while (std::size_t n = std::fread(buf.data(), 1, buf.size(), p)) {
        out.append(buf.data(), n);
    }
    const int rc = ::pclose(p);
    if (rc != 0) return std::nullopt;
    return out;
}

std::mutex& mutex() {
    static std::mutex m;
    return m;
}

} // namespace

bool Clipboard::set_text(const std::string& text) {
    std::lock_guard lock(mutex());
    const auto& tools = resolve_tools();
    if (tools.backend == Backend::none) return false;
    return pipe_to_command(tools.copy_cmd, text);
}

std::optional<std::string> Clipboard::get_text() {
    std::lock_guard lock(mutex());
    const auto& tools = resolve_tools();
    if (tools.backend == Backend::none) return std::nullopt;
    // Do not strip a trailing '\n'. xclip/wl-paste preserve the selection
    // bytes as-is; content that legitimately ends with '\n' would lose a byte
    // on round-trip if we stripped it.
    return capture_command(tools.paste_cmd);
}

bool Clipboard::has_text() {
    // "Text present" includes the empty string. An application that set ""
    // should observe has_text() == true. Report true iff the paste command
    // succeeded; the pasted value's length is orthogonal to whether a text
    // selection exists.
    std::lock_guard lock(mutex());
    const auto& tools = resolve_tools();
    if (tools.backend == Backend::none) return false;
    return capture_command(tools.paste_cmd).has_value();
}

namespace {

// A conservative target/MIME token used as a shell word in the wl-copy /
// xclip command (popen runs `/bin/sh -c`). Allow only characters that appear
// in real MIME types and X11 target atoms — letters, digits, and / . + - _ —
// so the type can never break out of its word (no spaces, quotes, $, ;, |,
// backticks, etc.). Empty or over-long is rejected. Callers that fail this
// get an honest false/nullopt, same as an unsupported backend.
bool valid_data_type(const std::string& t) {
    if (t.empty() || t.size() > 255) return false;
    for (unsigned char c : t) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') ||
                        c == '/' || c == '.' || c == '+' || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

// True if `type` appears as a whole line in a newline-separated list (the
// output of `xclip -t TARGETS -o` / `wl-paste --list-types`). Used to tell
// "type is offered" (read it — even if the payload is empty) from "type is
// absent" (return nullopt) — xclip/wl-paste both yield rc 0 + empty bytes for
// an unavailable target, which would otherwise look like an empty payload.
bool type_listed(const std::string& list, const std::string& type) {
    std::size_t pos = 0;
    while (pos <= list.size()) {
        const std::size_t nl = list.find('\n', pos);
        std::string line = list.substr(pos, (nl == std::string::npos ? list.size() : nl) - pos);
        while (!line.empty() &&
               (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        if (line == type) return true;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return false;
}

}  // namespace

bool Clipboard::set_data(const std::string& type,
                        const std::vector<uint8_t>& data) {
    // Binary clipboard via a custom MIME/target type. wl-copy and xclip both
    // carry arbitrary types byte-for-byte; xsel cannot (it only does the text
    // selection), so it honest-fails like a missing backend.
    if (!valid_data_type(type)) return false;
    std::lock_guard lock(mutex());
    const auto& tools = resolve_tools();
    std::string cmd;
    switch (tools.backend) {
        case Backend::wl_clipboard: cmd = "wl-copy --type " + type; break;
        case Backend::xclip:
            cmd = "xclip -selection clipboard -t " + type + " -in";
            break;
        case Backend::xsel:
        case Backend::none:
            return false;
    }
    return pipe_to_command(
        cmd,
        std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
}

std::optional<std::vector<uint8_t>> Clipboard::get_data(
    const std::string& type) {
    if (!valid_data_type(type)) return std::nullopt;
    std::lock_guard lock(mutex());
    const auto& tools = resolve_tools();
    std::string list_cmd, read_cmd;
    switch (tools.backend) {
        // -n keeps wl-paste from appending a trailing newline (it does so by
        // default); xclip -o emits the raw selection bytes already.
        case Backend::wl_clipboard:
            list_cmd = "wl-paste --list-types";
            read_cmd = "wl-paste --type " + type + " -n";
            break;
        case Backend::xclip:
            list_cmd = "xclip -selection clipboard -t TARGETS -out";
            read_cmd = "xclip -selection clipboard -t " + type + " -out";
            break;
        case Backend::xsel:
        case Backend::none:
            return std::nullopt;
    }
    // Read only if the type is actually offered, so an absent type is nullopt
    // (not an empty vector) while a present-but-empty payload still round-trips.
    auto types = capture_command(list_cmd);
    if (!types || !type_listed(*types, type)) return std::nullopt;
    auto out = capture_command(read_cmd);
    if (!out) return std::nullopt;
    return std::vector<uint8_t>(out->begin(), out->end());
}

} // namespace pulp::platform
