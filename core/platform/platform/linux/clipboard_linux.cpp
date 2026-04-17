// Linux clipboard implementation — shells out to system clipboard
// utilities (`wl-copy`/`wl-paste` on Wayland, `xclip`/`xsel` on X11).
//
// #300: the old implementation returned `true` for set_text without
// touching the OS clipboard, which was fake-success — copy/paste
// looked to work inside Pulp but didn't reach other applications on
// the user's desktop. This impl either really writes to the system
// clipboard OR returns false so callers can detect the unsupported
// case. No in-process shadow storage: if the tool is missing we say
// so honestly.
//
// Tooling detection is lazy (first-call stat) and cached. The tool
// chosen at startup is the one used for the lifetime of the process —
// session-type changes mid-run would need a restart, which is fine
// for a plugin host.

#include <pulp/platform/clipboard.hpp>

#include <array>
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
            // #320 Codex P1: wl-paste appends a newline by default.
            // Pass -n so get_text() returns the exact bytes
            // wl-copy received. xclip/xsel preserve bytes without
            // a flag; only Wayland needs this.
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
    FILE* p = ::popen(cmd.c_str(), "w");
    if (!p) return false;
    size_t wrote = std::fwrite(data.data(), 1, data.size(), p);
    const int rc = ::pclose(p);
    return wrote == data.size() && rc == 0;
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
    return capture_command(tools.paste_cmd);
    // #309 Codex P2: do NOT strip a trailing '\n'. xclip/wl-paste
    // preserve the selection bytes as-is — they don't synthesize a
    // newline. Content that legitimately ends with '\n' would lose a
    // byte on round-trip if we stripped. The previous comment in
    // this function was wrong; preserving the byte stream is the
    // correct round-trip for set_text/get_text.
}

bool Clipboard::has_text() {
    // #309 Codex P2: "text present" must include the empty string,
    // not treat it as absent. An application that set "" should
    // observe has_text() == true. Report true iff the paste command
    // succeeded — the pasted value's length is orthogonal to whether
    // a text selection exists. On platforms where paste always
    // succeeds (wl-paste -l / xclip -o return cleanly for empty
    // selections), this tracks the native clipboard API semantics.
    std::lock_guard lock(mutex());
    const auto& tools = resolve_tools();
    if (tools.backend == Backend::none) return false;
    return capture_command(tools.paste_cmd).has_value();
}

bool Clipboard::set_data(const std::string& /*type*/,
                        const std::vector<uint8_t>& /*data*/) {
    // Custom pasteboard types aren't meaningful over the xclip/wl-copy
    // text channel. Return false explicitly so callers know the
    // binary-clipboard path is unsupported on Linux today.
    return false;
}

std::optional<std::vector<uint8_t>> Clipboard::get_data(
    const std::string& /*type*/) {
    return std::nullopt;
}

} // namespace pulp::platform
