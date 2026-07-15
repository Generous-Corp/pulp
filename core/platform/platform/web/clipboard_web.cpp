// Web (Emscripten) clipboard — a real bridge to the browser clipboard, not a stub.
//
// The C++ Clipboard API is synchronous, but the browser clipboard is asynchronous and
// permission-gated: `navigator.clipboard.readText()` returns a Promise and cannot be awaited
// from inside a synchronous C++ call, and a browser will not hand a page the system clipboard
// on demand anyway. So a live synchronous READ of the OS clipboard is genuinely impossible in
// a browser — this is a platform limit, not a shortcut.
//
// The honest, idiomatic shape (the same one SDL and Emscripten's own html5 clipboard use):
//   - WRITE goes to the real OS clipboard via navigator.clipboard.writeText (fire-and-forget).
//   - READ returns a JS-side shadow of the clipboard text. The shadow is kept true by a `paste`
//     event listener: when the user pastes (⌘V / Ctrl-V), the browser delivers the real system
//     clipboard in the event, and we capture it. So the common round trip — copy in one app,
//     paste into this one — reads the real OS clipboard; and copy-then-paste within this app
//     round-trips through the shadow the write updated.
//
// Binary clipboard types (set_data/get_data) are not offered: the browser's async clipboard
// item API cannot satisfy the synchronous contract either, and no Pulp web path needs them.

#include <pulp/platform/clipboard.hpp>

#include <optional>
#include <string>
#include <vector>

#if defined(__EMSCRIPTEN__)

#include <cstdlib>
#include <emscripten/em_js.h>

// Install the paste listener once. Idempotent: guarded on the module so repeated calls (or a
// second Clipboard user) do not stack listeners.
EM_JS(void, pulp_clipboard_install, (), {
  if (Module.__pulpClipboardInstalled) return;
  Module.__pulpClipboardInstalled = true;
  if (typeof Module.__pulpClipboardText !== "string") Module.__pulpClipboardText = "";
  // Capture-phase, so the shadow is fresh before any app-level paste handler runs.
  document.addEventListener("paste", function(e) {
    try {
      var dt = e.clipboardData || window.clipboardData;
      var t = dt ? dt.getData("text") : null;
      if (t != null) Module.__pulpClipboardText = t;
    } catch (err) { /* no text data, or blocked by permissions */ }
  }, true);
});

EM_JS(void, pulp_clipboard_write, (const char* text), {
  var s = UTF8ToString(text);
  Module.__pulpClipboardText = s;   // shadow, so an in-app paste sees it immediately
  try {
    if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(s);
  } catch (err) { /* permission-gated or unavailable; the shadow still round-trips in-app */ }
});

// Returns a _malloc'd UTF-8 copy of the shadow (caller frees), or null when empty.
EM_JS(char*, pulp_clipboard_read, (), {
  var s = Module.__pulpClipboardText || "";
  if (!s.length) return 0;
  var len = lengthBytesUTF8(s) + 1;
  var p = _malloc(len);
  stringToUTF8(s, p, len);
  return p;
});

namespace pulp::platform {
namespace {
void ensure_installed() {
    static const bool once = [] { pulp_clipboard_install(); return true; }();
    (void)once;
}
}  // namespace

bool Clipboard::set_text(const std::string& text) {
    ensure_installed();
    pulp_clipboard_write(text.c_str());
    return true;
}

std::optional<std::string> Clipboard::get_text() {
    ensure_installed();
    char* p = pulp_clipboard_read();
    if (!p) return std::nullopt;
    std::string s(p);
    std::free(p);
    return s;
}

bool Clipboard::has_text() { return get_text().has_value(); }

// Binary types: not representable through the synchronous contract in a browser.
bool Clipboard::set_data(const std::string&, const std::vector<uint8_t>&) { return false; }
std::optional<std::vector<uint8_t>> Clipboard::get_data(const std::string&) { return std::nullopt; }

}  // namespace pulp::platform

#endif  // __EMSCRIPTEN__
