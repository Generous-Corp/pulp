// Windows native file dialogs via the Vista+ IFileDialog COM API (#301 / W5).
//
// Windows previously had no file_dialog impl — calls fell through to the
// backend-routed stub and returned "no selection". This provides a real
// backend (IFileOpenDialog / IFileSaveDialog) installed via
// FileDialog::install_native_backend() (the standalone host calls that at
// startup), mirroring the Linux xdg-desktop-portal backend. COM is initialized
// per call (apartment-threaded) and torn down after, so the dialog works
// regardless of the caller's COM state.
//
// Scope (MVP, matching the Linux portal backend): open one/many, save (with a
// suggested name), and choose-folder. File-type filters and a preselected
// folder are a follow-up — the dialog works without them. CLSIDs/IIDs are
// resolved with __uuidof / IID_PPV_ARGS so we don't depend on the named GUID
// constants in uuid.lib; only ole32 is linked.

#include <pulp/platform/file_dialog.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shobjidl.h>   // IFileDialog, IFileOpenDialog, IShellItem(Array)

#include <optional>
#include <string>
#include <vector>

namespace pulp::platform {

namespace {

// Per-call apartment-threaded COM init. RPC_E_CHANGED_MODE means COM was
// already initialized in another mode on this thread — that's fine, we just
// must not CoUninitialize in that case.
struct ComInit {
    HRESULT hr;
    ComInit() {
        hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    }
    ~ComInit() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
    bool usable() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

std::wstring utf8_to_utf16(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string utf16_to_utf8(const wchar_t* w) {
    if (!w) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s(static_cast<size_t>(n - 1), '\0');  // n includes the NUL
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// Filesystem path of a shell item, or nullopt for non-filesystem items.
std::optional<std::string> item_path(IShellItem* item) {
    if (!item) return std::nullopt;
    PWSTR psz = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz)) || !psz) return std::nullopt;
    std::string p = utf16_to_utf8(psz);
    CoTaskMemFree(psz);
    if (p.empty()) return std::nullopt;
    return p;
}

// Single-result open/save/folder dialog. `extra` adds option flags
// (e.g. FOS_PICKFOLDERS). `save` selects the Save coclass.
std::optional<std::string> run_single(bool save,
                                      const std::string& title,
                                      FILEOPENDIALOGOPTIONS extra,
                                      const std::string& default_name) {
    ComInit com;
    if (!com.usable()) return std::nullopt;

    IFileDialog* dlg = nullptr;
    HRESULT hr = save
        ? CoCreateInstance(__uuidof(FileSaveDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))
        : CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
    if (FAILED(hr) || !dlg) return std::nullopt;

    std::optional<std::string> result;
    FILEOPENDIALOGOPTIONS opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | extra | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR);
    if (!title.empty()) dlg->SetTitle(utf8_to_utf16(title).c_str());
    if (!default_name.empty()) dlg->SetFileName(utf8_to_utf16(default_name).c_str());

    if (SUCCEEDED(dlg->Show(nullptr))) {  // S_OK = picked; cancel → non-S_OK
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            result = item_path(item);
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

}  // namespace

FileDialog::Backend make_win_file_dialog_backend() {
    FileDialog::Backend backend;

    backend.open_file = [](const std::string& title,
                           const std::vector<FileFilter>&,
                           const std::string&) -> std::optional<std::string> {
        return run_single(/*save=*/false, title, 0, "");
    };

    backend.open_files = [](const std::string& title,
                            const std::vector<FileFilter>&,
                            const std::string&) -> std::vector<std::string> {
        ComInit com;
        if (!com.usable()) return {};
        IFileOpenDialog* dlg = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dlg))) || !dlg) {
            return {};
        }
        std::vector<std::string> out;
        FILEOPENDIALOGOPTIONS opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR);
        if (!title.empty()) dlg->SetTitle(utf8_to_utf16(title).c_str());
        if (SUCCEEDED(dlg->Show(nullptr))) {
            IShellItemArray* arr = nullptr;
            if (SUCCEEDED(dlg->GetResults(&arr)) && arr) {
                DWORD count = 0;
                arr->GetCount(&count);
                for (DWORD i = 0; i < count; ++i) {
                    IShellItem* item = nullptr;
                    if (SUCCEEDED(arr->GetItemAt(i, &item)) && item) {
                        if (auto p = item_path(item)) out.push_back(*p);
                        item->Release();
                    }
                }
                arr->Release();
            }
        }
        dlg->Release();
        return out;
    };

    backend.save_file = [](const std::string& title,
                           const std::vector<FileFilter>&,
                           const std::string&,
                           const std::string& default_name) -> std::optional<std::string> {
        return run_single(/*save=*/true, title, 0, default_name);
    };

    backend.choose_folder = [](const std::string& title,
                               const std::string&) -> std::optional<std::string> {
        return run_single(/*save=*/false, title, FOS_PICKFOLDERS, "");
    };

    return backend;
}

}  // namespace pulp::platform
