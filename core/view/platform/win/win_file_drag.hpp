#pragma once

// Windows OLE outbound file-drag source — shared by the plugin view host
// (plugin_view_host_win.cpp, which owns a child HWND) and the standalone window
// host path (drag_drop_win.cpp, which serves View::start_file_drag's free
// begin_file_drag backend). DoDragDrop is self-contained — it needs no HWND —
// so this header has NO Skia/render dependency and compiles on every Windows
// build, GPU or not. The CF_HDROP IDataObject + IDropSource below are the OLE
// analogue of macOS's NSDraggingSession and Linux's XDND source.

#include <pulp/view/drag_drop.hpp>

#include <windows.h>
#include <shlobj.h>

#include <cstring>
#include <string>
#include <vector>

namespace pulp::view::win_drag {

inline std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), n);
    return out;
}

// Build a CF_HDROP global: a DROPFILES header followed by the wide file paths,
// each NUL-terminated, with a final extra NUL (double-NUL list terminator).
inline HGLOBAL make_hdrop_global(const std::vector<std::string>& paths) {
    std::wstring buf;
    for (const auto& p : paths) {
        if (p.empty()) continue;
        buf += utf8_to_wide(p);
        buf.push_back(L'\0');
    }
    if (buf.empty()) return nullptr;
    buf.push_back(L'\0');  // double-NUL terminate the list

    const SIZE_T bytes = sizeof(DROPFILES) + buf.size() * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GHND, bytes);
    if (!h) return nullptr;
    auto* df = static_cast<DROPFILES*>(GlobalLock(h));
    if (!df) { GlobalFree(h); return nullptr; }
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;  // paths are wide (UTF-16)
    std::memcpy(reinterpret_cast<BYTE*>(df) + sizeof(DROPFILES), buf.data(),
                buf.size() * sizeof(wchar_t));
    GlobalUnlock(h);
    return h;
}

// Minimal IDataObject exposing a single CF_HDROP / TYMED_HGLOBAL payload. Owns
// the HGLOBAL and frees it on release; GetData hands callers a duplicate they
// free via ReleaseStgMedium (standard OLE ownership).
class PulpWinFileDataObject : public IDataObject {
public:
    explicit PulpWinFileDataObject(HGLOBAL hdrop) : hdrop_(hdrop) {
        fmt_ = FORMATETC{static_cast<CLIPFORMAT>(CF_HDROP), nullptr, DVASPECT_CONTENT,
                         -1, TYMED_HGLOBAL};
    }
    ~PulpWinFileDataObject() { if (hdrop_) GlobalFree(hdrop_); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *ppv = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(++ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const long r = --ref_;
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* fmt, STGMEDIUM* med) override {
        if (!fmt || !med) return E_INVALIDARG;
        if (QueryGetData(fmt) != S_OK) return DV_E_FORMATETC;
        HGLOBAL dup = OleDuplicateData(hdrop_, CF_HDROP, GMEM_MOVEABLE);
        if (!dup) return E_OUTOFMEMORY;
        med->tymed = TYMED_HGLOBAL;
        med->hGlobal = dup;
        med->pUnkForRelease = nullptr;  // caller frees via ReleaseStgMedium
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* fmt) override {
        if (!fmt) return E_INVALIDARG;
        return (fmt->cfFormat == CF_HDROP && (fmt->tymed & TYMED_HGLOBAL) &&
                fmt->dwAspect == DVASPECT_CONTENT)
                   ? S_OK : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dir, IEnumFORMATETC** out) override {
        if (dir != DATADIR_GET || !out) return E_NOTIMPL;
        return SHCreateStdEnumFmtEtc(1, &fmt_, out);
    }
    // Unsupported optional surface (a drag-source data object needs none of it).
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* o) override {
        if (o) o->ptd = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    HGLOBAL hdrop_ = nullptr;
    FORMATETC fmt_{};
    long ref_ = 1;
};

// Standard left-button file drag source: commit on button-up, cancel on Esc.
class PulpWinDropSource : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *ppv = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(++ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const long r = --ref_;
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD key) override {
        if (escape) return DRAGDROP_S_CANCEL;
        if (!(key & MK_LBUTTON)) return DRAGDROP_S_DROP;  // button released → drop
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    long ref_ = 1;
};

// Run an outbound file drag on the calling (UI) thread. Returns true if the OS
// completed a copy/link drop. Blocks until the drag ends (standard DoDragDrop).
// Self-contained: DoDragDrop tracks the live mouse itself, so no HWND is needed.
inline bool win_run_file_drag(const FileDragRequest& request) {
    if (request.file_paths.empty()) return false;
    HGLOBAL hdrop = make_hdrop_global(request.file_paths);
    if (!hdrop) return false;

    auto* data = new PulpWinFileDataObject(hdrop);  // takes ownership of hdrop
    auto* source = new PulpWinDropSource();
    DWORD effect = 0;
    const HRESULT hr = DoDragDrop(data, source,
                                  DROPEFFECT_COPY | DROPEFFECT_LINK, &effect);
    data->Release();
    source->Release();
    return hr == DRAGDROP_S_DROP && effect != DROPEFFECT_NONE;
}

}  // namespace pulp::view::win_drag
