#include <pulp/platform/special_dir.hpp>

#include <pulp/platform/detect.hpp>

#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <initializer_list>
#include <iterator>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <initguid.h> // defines the FOLDERID_* GUIDs here instead of needing uuid.lib
#include <knownfolders.h>
#include <pulp/platform/win32_sane.hpp>
#include <shlobj.h>
#elif defined(__APPLE__)
#include <dlfcn.h>
#include <mach-o/dyld.h>
#elif !defined(__EMSCRIPTEN__)
#include <dlfcn.h>
#include <unistd.h>
#endif

#if defined(__ANDROID__)
namespace pulp::android {
// Defined in src/android/file_provider.cpp; set from Kotlin at startup.
const std::string& app_data_dir();
} // namespace pulp::android
#endif

namespace pulp::platform {

namespace {

using std::string;
using std::string_view;

char separator(SpecialDirOs os) {
    return os == SpecialDirOs::windows ? '\\' : '/';
}

bool is_sep(char c) {
    return c == '/' || c == '\\';
}

string trim_trailing_seps(string s) {
    while (s.size() > 1 && is_sep(s.back()))
        s.pop_back();
    return s;
}

string join(SpecialDirOs os, const string& base, std::initializer_list<string_view> parts) {
    if (base.empty())
        return {};
    string out = trim_trailing_seps(base);
    for (auto part : parts) {
        if (out.empty() || !is_sep(out.back()))
            out.push_back(separator(os));
        out.append(part);
    }
    return out;
}

// Parent folder of a path written for `os`. Empty if there is none.
string parent_of(const string& path) {
    const auto pos = path.find_last_of("/\\");
    if (pos == string::npos)
        return {};
    if (pos == 0)
        return string(1, path[0]);
    return path.substr(0, pos);
}

bool ends_with_icase(string_view s, string_view suffix) {
    if (s.size() < suffix.size())
        return false;
    for (size_t i = 0; i < suffix.size(); ++i) {
        char a = s[s.size() - suffix.size() + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z')
            a = static_cast<char>(a - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

// Innermost directory above `path` whose name ends in one of `exts`, or "".
// The leaf is skipped: a Windows VST3 binary is itself a file named *.vst3
// inside the bundle directory.
string enclosing_bundle(const string& path, const std::vector<string_view>& exts) {
    string cur = parent_of(path);
    while (!cur.empty()) {
        const auto pos = cur.find_last_of("/\\");
        const string_view name =
            pos == string::npos ? string_view(cur) : string_view(cur).substr(pos + 1);
        for (auto ext : exts) {
            if (name.size() > ext.size() && ends_with_icase(name, ext))
                return cur;
        }
        if (pos == string::npos || pos == 0)
            break;
        cur = cur.substr(0, pos);
    }
    return {};
}

} // namespace

SpecialDirOs host_special_dir_os() noexcept {
#if defined(__EMSCRIPTEN__)
    return SpecialDirOs::web;
#else
    switch (current_os) {
    case OS::macOS:
        return SpecialDirOs::macos;
    case OS::iOS:
        return SpecialDirOs::ios;
    case OS::Windows:
        return SpecialDirOs::windows;
    case OS::Android:
        return SpecialDirOs::android;
    default:
        return SpecialDirOs::linux_desktop;
    }
#endif
}

std::string_view special_dir_name(SpecialDir kind) noexcept {
    switch (kind) {
    case SpecialDir::home:
        return "home";
    case SpecialDir::documents:
        return "documents";
    case SpecialDir::desktop:
        return "desktop";
    case SpecialDir::music:
        return "music";
    case SpecialDir::app_data:
        return "app_data";
    case SpecialDir::app_data_shared:
        return "app_data_shared";
    case SpecialDir::config:
        return "config";
    case SpecialDir::temp:
        return "temp";
    case SpecialDir::cache:
        return "cache";
    case SpecialDir::logs:
        return "logs";
    case SpecialDir::current_executable:
        return "current_executable";
    case SpecialDir::bundle_dir:
        return "bundle_dir";
    case SpecialDir::user_presets:
        return "user_presets";
    }
    return "unknown";
}

std::string special_dir_for(SpecialDirOs os, SpecialDir kind, const SpecialDirInputs& in) {
    const auto env = [&in](string_view name) -> string {
        if (!in.getenv)
            return {};
        auto value = in.getenv(name);
        return value ? *value : string{};
    };
    const auto first = [](std::initializer_list<string> candidates) -> string {
        for (const auto& c : candidates)
            if (!c.empty())
                return c;
        return {};
    };
    // Absolute value only: a relative XDG_* path is ignored, as the XDG spec requires.
    const auto env_abs = [&](string_view name) -> string {
        string v = env(name);
        return (!v.empty() && (v[0] == '/' || v[0] == '\\')) ? v : string{};
    };

    // ── Android: everything lives under the app files directory ──────────────
    if (os == SpecialDirOs::android) {
        const string& files = in.android_files_dir;
        if (files.empty()) {
            return kind == SpecialDir::current_executable ? in.executable : string{};
        }
        const string cache = first({in.android_cache_dir, join(os, files, {"cache"})});
        switch (kind) {
        case SpecialDir::home:
        case SpecialDir::app_data:
            return files;
        case SpecialDir::documents:
            return join(os, files, {"Documents"});
        case SpecialDir::music:
            return join(os, files, {"Music"});
        case SpecialDir::config:
            return join(os, files, {"config"});
        case SpecialDir::temp:
        case SpecialDir::cache:
            return cache;
        case SpecialDir::logs:
            return join(os, files, {"logs"});
        case SpecialDir::user_presets:
            return join(os, files, {"Presets"});
        case SpecialDir::current_executable:
            return in.executable;
        case SpecialDir::desktop:
        case SpecialDir::app_data_shared:
        case SpecialDir::bundle_dir:
            return {};
        }
        return {};
    }

    // ── WebAssembly: an in-memory tree ───────────────────────────────────────
    if (os == SpecialDirOs::web) {
        const string home = "/home/web_user";
        switch (kind) {
        case SpecialDir::home:
            return home;
        case SpecialDir::documents:
            return home + "/Documents";
        case SpecialDir::desktop:
            return home + "/Desktop";
        case SpecialDir::music:
            return home + "/Music";
        case SpecialDir::app_data:
            return home + "/.local/share";
        case SpecialDir::config:
        case SpecialDir::user_presets:
            return home + "/.config";
        case SpecialDir::cache:
            return home + "/.cache";
        case SpecialDir::logs:
            return home + "/.local/state/logs";
        case SpecialDir::temp:
            return "/tmp";
        case SpecialDir::bundle_dir:
            return "/";
        case SpecialDir::app_data_shared:
        case SpecialDir::current_executable:
            return {};
        }
        return {};
    }

    // ── Home ─────────────────────────────────────────────────────────────────
    string home;
    if (os == SpecialDirOs::windows) {
        home = env("USERPROFILE");
        if (home.empty()) {
            const string drive = env("HOMEDRIVE");
            const string path = env("HOMEPATH");
            if (!drive.empty() && !path.empty())
                home = drive + path;
        }
    } else {
        home = env("HOME");
    }

    const bool apple = os == SpecialDirOs::macos || os == SpecialDirOs::ios;

    switch (kind) {
    case SpecialDir::home:
        return home;

    case SpecialDir::documents:
        if (os == SpecialDirOs::windows)
            return first({in.known_documents, join(os, home, {"Documents"})});
        if (os == SpecialDirOs::linux_desktop)
            return first({env_abs("XDG_DOCUMENTS_DIR"), join(os, home, {"Documents"})});
        return join(os, home, {"Documents"});

    case SpecialDir::desktop:
        if (os == SpecialDirOs::ios)
            return {};
        if (os == SpecialDirOs::windows)
            return first({in.known_desktop, join(os, home, {"Desktop"})});
        if (os == SpecialDirOs::linux_desktop)
            return first({env_abs("XDG_DESKTOP_DIR"), join(os, home, {"Desktop"})});
        return join(os, home, {"Desktop"});

    case SpecialDir::music:
        if (os == SpecialDirOs::ios)
            return {};
        if (os == SpecialDirOs::windows)
            return first({in.known_music, join(os, home, {"Music"})});
        if (os == SpecialDirOs::linux_desktop)
            return first({env_abs("XDG_MUSIC_DIR"), join(os, home, {"Music"})});
        return join(os, home, {"Music"});

    case SpecialDir::app_data:
        if (apple)
            return join(os, home, {"Library", "Application Support"});
        if (os == SpecialDirOs::windows)
            return first({env("APPDATA"), join(os, home, {"AppData", "Roaming"})});
        return first({env_abs("XDG_DATA_HOME"), join(os, home, {".local", "share"})});

    case SpecialDir::app_data_shared:
        if (os == SpecialDirOs::macos)
            return "/Library/Application Support";
        if (os == SpecialDirOs::windows)
            return first({env("ProgramData"), string("C:\\ProgramData")});
        if (os == SpecialDirOs::linux_desktop)
            return "/usr/share";
        return {};

    case SpecialDir::config:
        if (apple)
            return join(os, home, {"Library", "Application Support"});
        if (os == SpecialDirOs::windows)
            return first({env("APPDATA"), join(os, home, {"AppData", "Roaming"})});
        return first({env_abs("XDG_CONFIG_HOME"), join(os, home, {".config"})});

    case SpecialDir::user_presets:
        if (apple)
            return join(os, home, {"Library", "Audio", "Presets"});
        if (os == SpecialDirOs::windows)
            return first({env("APPDATA"), join(os, home, {"AppData", "Roaming"})});
        return first({env_abs("XDG_CONFIG_HOME"), join(os, home, {".config"})});

    case SpecialDir::temp:
        if (os == SpecialDirOs::windows)
            return first({env("TMP"), env("TEMP"), join(os, home, {"AppData", "Local", "Temp"})});
        return trim_trailing_seps(first({env("TMPDIR"), string("/tmp")}));

    case SpecialDir::cache:
        if (apple)
            return join(os, home, {"Library", "Caches"});
        if (os == SpecialDirOs::windows)
            return first({env("LOCALAPPDATA"), join(os, home, {"AppData", "Local"})});
        return first({env_abs("XDG_CACHE_HOME"), join(os, home, {".cache"})});

    case SpecialDir::logs:
        if (apple)
            return join(os, home, {"Library", "Logs"});
        if (os == SpecialDirOs::windows) {
            return join(os, first({env("LOCALAPPDATA"), join(os, home, {"AppData", "Local"})}),
                        {"Logs"});
        }
        return join(os, first({env_abs("XDG_STATE_HOME"), join(os, home, {".local", "state"})}),
                    {"logs"});

    case SpecialDir::current_executable:
        return in.executable;

    case SpecialDir::bundle_dir: {
        const string& anchor = in.module.empty() ? in.executable : in.module;
        if (anchor.empty())
            return {};
        if (apple) {
            const string bundle = enclosing_bundle(
                anchor, {".app", ".appex", ".component", ".vst3", ".clap", ".vst", ".framework"});
            if (!bundle.empty()) {
                // iOS bundles are flat; macOS keeps resources under Contents.
                return os == SpecialDirOs::ios ? bundle
                                               : join(os, bundle, {"Contents", "Resources"});
            }
            return parent_of(anchor);
        }
        // Windows and Linux: only the VST3 layout is a bundle.
        const string bundle = enclosing_bundle(anchor, {".vst3"});
        if (!bundle.empty())
            return join(os, bundle, {"Contents", "Resources"});
        return parent_of(anchor);
    }
    }
    return {};
}

// ── Host resolution ─────────────────────────────────────────────────────────

namespace {

#if defined(_WIN32)
string wide_to_utf8(const wchar_t* w, int length) {
    if (!w || length <= 0)
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, length, nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return {};
    string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, length, out.data(), n, nullptr, nullptr);
    return out;
}
#endif

std::optional<string> host_getenv(string_view name) {
#if defined(_WIN32)
    std::wstring wname(name.begin(), name.end()); // variable names are ASCII
    wchar_t* value = nullptr;
    size_t size = 0;
    if (_wdupenv_s(&value, &size, wname.c_str()) != 0 || value == nullptr)
        return std::nullopt;
    string out = wide_to_utf8(value, static_cast<int>(wcslen(value)));
    std::free(value);
    if (out.empty())
        return std::nullopt;
    return out;
#else
    const string key(name);
    if (const char* value = std::getenv(key.c_str()); value && *value)
        return string(value);
    return std::nullopt;
#endif
}

string path_to_utf8(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return string(u8.begin(), u8.end());
}

std::filesystem::path path_from_utf8(const string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

string canonical_or_same(const string& p) {
    if (p.empty())
        return p;
    std::error_code ec;
    auto resolved = std::filesystem::weakly_canonical(path_from_utf8(p), ec);
    return ec ? p : path_to_utf8(resolved);
}

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
void module_anchor() {}

string module_path_posix() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&module_anchor), &info) != 0 && info.dli_fname)
        return canonical_or_same(info.dli_fname);
    return {};
}
#endif

string executable_path() {
#if defined(_WIN32)
    wchar_t buf[32768];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf))
        return {};
    return wide_to_utf8(buf, static_cast<int>(n));
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0)
        return {};
    return canonical_or_same(buf.data());
#elif defined(__EMSCRIPTEN__)
    return {};
#else
    std::vector<char> buf(4096);
    const ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n <= 0)
        return {};
    return string(buf.data(), static_cast<size_t>(n));
#endif
}

string module_path() {
#if defined(_WIN32)
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&host_getenv), &module))
        return {};
    wchar_t buf[32768];
    const DWORD n = GetModuleFileNameW(module, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf))
        return {};
    return wide_to_utf8(buf, static_cast<int>(n));
#elif defined(__EMSCRIPTEN__)
    return {};
#else
    return module_path_posix();
#endif
}

#if defined(_WIN32)
string known_folder(const KNOWNFOLDERID& id) {
    PWSTR w = nullptr;
    string out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &w)) && w)
        out = wide_to_utf8(w, static_cast<int>(wcslen(w)));
    CoTaskMemFree(w);
    return out;
}
#endif

SpecialDirInputs host_inputs() {
    // The executable and module do not change for the life of the process.
    static const string exe = executable_path();
    static const string mod = module_path();

    SpecialDirInputs in;
    in.getenv = host_getenv;
    in.executable = exe;
    in.module = mod;
#if defined(_WIN32)
    in.known_documents = known_folder(FOLDERID_Documents);
    in.known_desktop = known_folder(FOLDERID_Desktop);
    in.known_music = known_folder(FOLDERID_Music);
#elif defined(__ANDROID__)
    in.android_files_dir = pulp::android::app_data_dir();
#endif
    return in;
}

struct OverrideTable {
    std::mutex mutex;
    std::map<SpecialDir, std::filesystem::path> entries;
};

OverrideTable& overrides() {
    static OverrideTable* table = new OverrideTable; // leaked: valid during static teardown
    return *table;
}

} // namespace

void set_special_dir_override(SpecialDir kind, std::filesystem::path path) {
    auto& table = overrides();
    std::lock_guard<std::mutex> lock(table.mutex);
    if (path.empty())
        table.entries.erase(kind);
    else
        table.entries[kind] = std::move(path);
}

void clear_special_dir_overrides() {
    auto& table = overrides();
    std::lock_guard<std::mutex> lock(table.mutex);
    table.entries.clear();
}

std::filesystem::path special_dir(SpecialDir kind) {
    {
        auto& table = overrides();
        std::lock_guard<std::mutex> lock(table.mutex);
        if (auto it = table.entries.find(kind); it != table.entries.end())
            return it->second;
    }
    return path_from_utf8(special_dir_for(host_special_dir_os(), kind, host_inputs()));
}

} // namespace pulp::platform
