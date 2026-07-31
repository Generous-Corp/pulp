#pragma once

// win_plugin_drop_target.hpp — the OLE inbound-drop half of the Windows
// plug-in editor host.
//
// Extracted verbatim from plugin_view_host_win.cpp (WAH-6), which had grown to
// ~1,430 lines carrying HWND lifecycle, OLE drop handling, pointer/capture,
// keyboard/text/focus, DPI/viewport conversion, rendering/readback, and WndProc
// dispatch in one class. OLE is the most self-contained of those: a
// COM-refcounted object implementing a Microsoft interface, with lifetime rules
// that have nothing to do with the rest of the host.
//
// HEADER-ONLY, and the definitions are unchanged from where they lived before.
// That is deliberate: this code only compiles under `_WIN32`, so on a macOS
// development machine there is no compiler to catch a transcription slip. A
// textual move cannot introduce one; splitting the members into out-of-line
// definitions could, and the first place that would surface is the Windows CI
// gate — or a user's DAW.
//
// The host keeps ownership of the HWND and of registration/revocation. This
// module owns IDataObject decoding and the IDropTarget protocol, routing
// results into the SHARED cross-platform dispatch core (drag_drop.cpp) — the
// same core the SDL and macOS producers use, so a drop behaves identically
// everywhere.

#include <pulp/view/drag_drop.hpp>
#include <pulp/view/geometry.hpp>
#include <pulp/view/pointer_dispatch.hpp>
#include <pulp/view/view.hpp>
#include <pulp/runtime/log.hpp>

#include <functional>
#include <string>

// win32_sane.hpp, never raw <windows.h>: it pre-sets WIN32_LEAN_AND_MEAN and
// NOMINMAX so the min/max macros cannot collide with std::min/std::max or with
// Skia's headers (issue #384). A header that pulls windows.h directly drags
// that collision into whatever translation unit includes it FIRST, which is a
// failure that only shows up on the Windows lane.
#include <pulp/platform/win32_sane.hpp>

#include <ole2.h>
#include <shellapi.h>

namespace pulp::view::win_drop {

// UTF-16 → UTF-8 for dropped text / paths.
std::string wide_to_utf8(const wchar_t* w, int wlen) {
    if (!w || wlen <= 0) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, wlen, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, wlen, out.data(), n, nullptr, nullptr);
    return out;
}

// Pull a DropData out of an OLE IDataObject (files take priority over text,
// matching the SDL + macOS producers).
DropData extract_idata_drop(IDataObject* data) {
    DropData out;
    if (!data) return out;

    // CF_HDROP — a list of file paths.
    FORMATETC fmt_files{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM stg{};
    if (data->GetData(&fmt_files, &stg) == S_OK) {
        if (auto hdrop = static_cast<HDROP>(GlobalLock(stg.hGlobal))) {
            const UINT count = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
            out.type = DropData::Type::files;
            for (UINT i = 0; i < count; ++i) {
                const UINT len = DragQueryFileW(hdrop, i, nullptr, 0);
                std::wstring buf(len + 1, L'\0');
                const UINT got = DragQueryFileW(hdrop, i, buf.data(),
                                                static_cast<UINT>(buf.size()));
                if (got > 0) out.file_paths.push_back(wide_to_utf8(buf.data(),
                                                                   static_cast<int>(got)));
            }
            GlobalUnlock(stg.hGlobal);
        }
        ReleaseStgMedium(&stg);
        if (!out.file_paths.empty()) return out;
    }

    // CF_UNICODETEXT — a single text payload.
    FORMATETC fmt_text{CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM stg_text{};
    if (data->GetData(&fmt_text, &stg_text) == S_OK) {
        if (auto* w = static_cast<const wchar_t*>(GlobalLock(stg_text.hGlobal))) {
            out.type = DropData::Type::text;
            out.text = wide_to_utf8(w, static_cast<int>(wcslen(w)));
            GlobalUnlock(stg_text.hGlobal);
        }
        ReleaseStgMedium(&stg_text);
    }
    return out;
}

// Minimal IDropTarget that routes OLE drops on the child HWND into the shared
// cross-platform view-tree dispatch core (drag_drop.cpp) — the same core the SDL
// and macOS producers use. Owned by WinPluginViewHost; registered via
// RegisterDragDrop on the child HWND.
class PulpWinDropTarget : public IDropTarget {
public:
    PulpWinDropTarget(View& root, HWND hwnd, std::function<Point(Point)> to_root)
        : root_(root), hwnd_(hwnd), to_root_(std::move(to_root)) {}

    // Screen POINTL → client → root coordinates (mirrors the mouse path).
    Point to_root_point(POINTL pt) const {
        POINT p{pt.x, pt.y};
        ScreenToClient(hwnd_, &p);
        Point client{static_cast<float>(p.x), static_cast<float>(p.y)};
        return to_root_ ? to_root_(client) : client;
    }

    // ── IUnknown ──────────────────────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(++ref_);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const long r = --ref_;
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }

    // ── IDropTarget ───────────────────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL pt,
                                        DWORD* effect) override {
        pending_ = extract_idata_drop(data);
        const bool ok = dispatch_drag_enter(root_, session_, pending_,
                                            to_root_point(pt));
        *effect = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* effect) override {
        const bool ok = dispatch_drag_enter(root_, session_, pending_,
                                            to_root_point(pt));
        *effect = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        dispatch_drag_exit(root_, session_);
        pending_ = {};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL pt,
                                   DWORD* effect) override {
        DropData d = extract_idata_drop(data);
        const bool ok = dispatch_drop(root_, session_, d, to_root_point(pt));
        *effect = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        pending_ = {};
        return S_OK;
    }

private:
    View& root_;
    HWND hwnd_;
    std::function<Point(Point)> to_root_;
    DragSession session_;
    DropData pending_;  // payload cached between DragEnter and Drop
    long ref_ = 1;
};

/// RAII owner of a window's inbound drag-drop registration.
///
/// Lives here rather than in the host because the sequence is a property of the
/// TARGET, not of whoever happens to register it, and it is easy to get subtly
/// wrong in three places at once:
///
///   * `OleInitialize` is per-thread and reference-counted, so only a host that
///     actually brought COM up may call `OleUninitialize`. An unconditional
///     uninitialize tears down the apartment out from under the DAW.
///   * `RPC_E_CHANGED_MODE` means the host thread is already MTA. Drag-drop
///     needs an STA, so the honest response is to skip it — not to fight the
///     apartment model or to treat the failure as fatal.
///   * `RevokeDragDrop` must precede the final `Release`, and both must be
///     skipped entirely if registration never succeeded.
///
/// Move-disabled: the destructor is the only correct unregistration point.
class DropTargetRegistration {
public:
    DropTargetRegistration() = default;
    DropTargetRegistration(const DropTargetRegistration&) = delete;
    DropTargetRegistration& operator=(const DropTargetRegistration&) = delete;
    ~DropTargetRegistration() { reset(); }

    /// Register `hwnd` for inbound drops, mapping client PHYSICAL pixels to
    /// root coordinates through `to_root`. Returns whether drops are now live;
    /// a false return is a supported degradation, not an error.
    bool register_for(View& root, HWND hwnd, std::function<Point(Point)> to_root) {
        reset();
        if (!hwnd) return false;
        const HRESULT hr = OleInitialize(nullptr);
        if (hr != S_OK && hr != S_FALSE) {
            runtime::log_warn("DropTargetRegistration: OleInitialize failed "
                              "(0x{:08x}); drag-drop disabled",
                              static_cast<unsigned>(hr));
            return false;
        }
        ole_initialized_ = true;
        hwnd_ = hwnd;
        target_ = new PulpWinDropTarget(root, hwnd, std::move(to_root));
        if (RegisterDragDrop(hwnd, target_) != S_OK) {
            runtime::log_warn("DropTargetRegistration: RegisterDragDrop failed");
            target_->Release();
            target_ = nullptr;
            return false;
        }
        return true;
    }

    /// Unregister and balance COM. Idempotent, so the host may call it from
    /// both an explicit teardown and its destructor.
    void reset() {
        if (target_) {
            if (hwnd_) RevokeDragDrop(hwnd_);
            target_->Release();
            target_ = nullptr;
        }
        hwnd_ = nullptr;
        if (ole_initialized_) {
            OleUninitialize();
            ole_initialized_ = false;
        }
    }

    bool registered() const noexcept { return target_ != nullptr; }

private:
    bool ole_initialized_ = false;
    HWND hwnd_ = nullptr;
    PulpWinDropTarget* target_ = nullptr;  // ref-counted by OLE; we hold one
};

}  // namespace pulp::view::win_drop
