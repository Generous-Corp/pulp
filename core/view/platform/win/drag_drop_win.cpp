// Windows standalone outbound file-drag backend.
//
// View::start_file_drag() reaches a plugin view host via the host's
// start_file_drag() virtual, but a standalone app's tree is owned by a
// WindowHost — there start_file_drag() falls through to the free function
// begin_file_drag(native_view, request) declared in drag_drop.hpp. On macOS
// that free function is the NSDraggingSession backend (drag_drop_mac.mm); this
// file is its Windows peer, so a standalone Pulp app can drag a file out to
// Explorer / another app, not just a plugin editor.
//
// Compiled on EVERY Windows build (OLE/DoDragDrop has no Skia/GPU dependency),
// so the `#if !defined(__APPLE__) && !defined(_WIN32)` no-op in drag_drop.cpp
// drops out for _WIN32 and this definition wins. The OLE source itself is the
// shared header win_file_drag.hpp, identical to the plugin-host path.

#include "win_file_drag.hpp"

#include <pulp/view/drag_drop.hpp>

#include <objbase.h>  // OleInitialize / OleUninitialize

namespace pulp::view {

bool begin_file_drag(void* native_view, const FileDragRequest& request) {
    if (native_view == nullptr) return false;
    if (request.file_paths.empty()) return false;
    // DoDragDrop requires the calling thread to be an OLE-initialized STA. A
    // standalone app's UI thread usually already is (SDL initializes OLE for its
    // own drag-drop), but OleInitialize is ref-counted: S_FALSE when already
    // initialized, and the paired OleUninitialize merely decrements, so this is
    // safe whether or not the host set it up. After the null-handle contract is
    // satisfied, native_view (the HWND) is unused: DoDragDrop tracks the live
    // mouse itself and needs no source window.
    const HRESULT oi = OleInitialize(nullptr);
    const bool did_init = (oi == S_OK || oi == S_FALSE);
    const bool ok = win_drag::win_run_file_drag(request);
    if (did_init) OleUninitialize();
    return ok;
}

}  // namespace pulp::view
