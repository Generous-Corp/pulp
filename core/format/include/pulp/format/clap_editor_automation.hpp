#pragma once

// Opt-in CLAP extension that lets a host process capture the open editor's
// current frame from a shipped plug-in binary.
//
// The editor-host contract already has `EditorAutomation::capture_png()`, but
// a built plug-in exports only `clap_entry`, so an out-of-process-linked host
// cannot reach the session that owns it. This extension forwards to it. It is
// a test-automation seam, not a product feature: it exists only in plug-ins
// configured with PULP_EDITOR_AUTOMATION_EXTENSION=ON (default OFF), and with
// the option off none of this code is compiled into the plug-in.
//
// Both calls are main-thread, like every clap_plugin_gui call. Neither runs on
// or touches the audio thread.

#include <pulp/format/clap_contract_gui.hpp>
#include <pulp/format/contract_editor_session.hpp>
#include <pulp/format/editor_host.hpp>

#include <clap/clap.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#define PULP_CLAP_EXT_EDITOR_AUTOMATION "pulp.editor-automation/1"

typedef struct pulp_clap_plugin_editor_automation {
    // Encodes the open editor's current frame as PNG. On success returns true
    // and sets `*data` / `*size` to a buffer the caller releases with
    // free_png(). Returns false, leaving the outputs untouched, when no editor
    // is open or it cannot capture.
    bool (*capture_png)(const clap_plugin_t* plugin, uint8_t** data, size_t* size);
    // Releases a buffer returned by capture_png().
    void (*free_png)(const clap_plugin_t* plugin, uint8_t* data);
} pulp_clap_plugin_editor_automation_t;

#if defined(PULP_EDITOR_AUTOMATION_EXTENSION)

namespace pulp::format::clap_adapter {

inline bool editor_automation_capture_png(const clap_plugin_t* plugin, uint8_t** data,
                                          size_t* size) {
    if (plugin == nullptr || data == nullptr || size == nullptr)
        return false;
    auto* session = contract_gui_session(plugin);
    auto* editor = session != nullptr ? session->editor() : nullptr;
    auto* automation = editor != nullptr ? editor->automation() : nullptr;
    if (automation == nullptr)
        return false;
    std::vector<std::uint8_t> png;
    if (!automation->capture_png(png) || png.empty())
        return false;
    auto* out = new std::uint8_t[png.size()];
    std::memcpy(out, png.data(), png.size());
    *data = out;
    *size = png.size();
    return true;
}

inline void editor_automation_free_png(const clap_plugin_t*, uint8_t* data) {
    delete[] data;
}

inline const pulp_clap_plugin_editor_automation_t* editor_automation_extension() {
    static const pulp_clap_plugin_editor_automation_t ext = {
        .capture_png = editor_automation_capture_png,
        .free_png = editor_automation_free_png,
    };
    return &ext;
}

} // namespace pulp::format::clap_adapter

#endif // PULP_EDITOR_AUTOMATION_EXTENSION
