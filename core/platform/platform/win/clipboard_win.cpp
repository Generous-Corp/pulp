// Windows clipboard — real OpenClipboard/SetClipboardData integration.
//
// The previous implementation stored text/data in process-local maps
// (`g_text`/`g_data`), which fake-succeeded: copy/paste looked to work inside
// Pulp but never reached the system clipboard, so other applications saw
// nothing. This implementation talks to the OS clipboard or returns
// false/nullopt so callers can detect the unsupported case (e.g. a session with
// no window station). No in-process shadow storage.
//
// Text is exchanged as CF_UNICODETEXT (UTF-16), converted to/from UTF-8 at the
// boundary. Binary blobs use a per-type registered clipboard format
// (RegisterClipboardFormat), matching the macOS custom-pasteboard-type and
// Linux MIME-target behavior.

#include <pulp/platform/clipboard.hpp>

#include <cstring>
#include <cwchar>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace pulp::platform {

namespace {

// Serializes our own clipboard calls; the OS clipboard is global but a single
// process should not race its own Open/Close pairs.
std::mutex g_mutex;

// Open the clipboard with a few retries — another process may briefly hold it.
bool open_clipboard() {
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (OpenClipboard(nullptr)) return true;
        Sleep(1);
    }
    return false;
}

std::wstring utf8_to_utf16(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string utf16_to_utf8(const wchar_t* w, int wlen) {
    if (!w || wlen <= 0) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, wlen, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, wlen, s.data(), n, nullptr, nullptr);
    return s;
}

// Copy `bytes` into a GMEM_MOVEABLE HGLOBAL for SetClipboardData (which takes
// ownership on success). Returns nullptr on allocation failure.
HGLOBAL alloc_global(const void* bytes, size_t size) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, size ? size : 1);
    if (!h) return nullptr;
    void* dst = GlobalLock(h);
    if (!dst) { GlobalFree(h); return nullptr; }
    if (size) memcpy(dst, bytes, size);
    GlobalUnlock(h);
    return h;
}

}  // namespace

bool Clipboard::set_text(const std::string& text) {
    std::lock_guard lock(g_mutex);
    std::wstring w = utf8_to_utf16(text);
    // CF_UNICODETEXT requires a NUL-terminated buffer including the terminator.
    const size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    if (!open_clipboard()) return false;
    bool ok = false;
    if (EmptyClipboard()) {
        HGLOBAL h = alloc_global(w.c_str(), bytes);
        if (h) {
            if (SetClipboardData(CF_UNICODETEXT, h)) {
                ok = true;       // clipboard now owns h
            } else {
                GlobalFree(h);   // ownership not transferred on failure
            }
        }
    }
    CloseClipboard();
    return ok;
}

std::optional<std::string> Clipboard::get_text() {
    std::lock_guard lock(g_mutex);
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return std::nullopt;
    if (!open_clipboard()) return std::nullopt;
    std::optional<std::string> result;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* w = static_cast<const wchar_t*>(GlobalLock(h))) {
            // The stored buffer is NUL-terminated; convert up to (not incl.)
            // the terminator so the round-trip preserves the exact string.
            int wlen = static_cast<int>(wcslen(w));
            result = utf16_to_utf8(w, wlen);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return result;
}

bool Clipboard::has_text() {
    std::lock_guard lock(g_mutex);
    return IsClipboardFormatAvailable(CF_UNICODETEXT) != 0;
}

bool Clipboard::set_data(const std::string& type, const std::vector<uint8_t>& data) {
    if (type.empty()) return false;
    std::lock_guard lock(g_mutex);
    UINT fmt = RegisterClipboardFormatA(type.c_str());
    if (fmt == 0) return false;
    if (!open_clipboard()) return false;
    bool ok = false;
    if (EmptyClipboard()) {
        HGLOBAL h = alloc_global(data.data(), data.size());
        if (h) {
            if (SetClipboardData(fmt, h)) {
                ok = true;
            } else {
                GlobalFree(h);
            }
        }
    }
    CloseClipboard();
    return ok;
}

std::optional<std::vector<uint8_t>> Clipboard::get_data(const std::string& type) {
    if (type.empty()) return std::nullopt;
    std::lock_guard lock(g_mutex);
    UINT fmt = RegisterClipboardFormatA(type.c_str());
    if (fmt == 0 || !IsClipboardFormatAvailable(fmt)) return std::nullopt;
    if (!open_clipboard()) return std::nullopt;
    std::optional<std::vector<uint8_t>> result;
    if (HANDLE h = GetClipboardData(fmt)) {
        SIZE_T size = GlobalSize(h);
        if (const void* src = GlobalLock(h)) {
            const auto* b = static_cast<const uint8_t*>(src);
            result = std::vector<uint8_t>(b, b + size);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return result;
}

} // namespace pulp::platform
