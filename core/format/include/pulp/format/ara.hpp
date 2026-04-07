#pragma once

// ARA (Audio Random Access) — minimal document controller stub.
// Full ARA 2.0 support is a large undertaking; this provides the interface
// structure so plugins can declare ARA capability without a full implementation.
//
// ARA allows DAWs to provide the plugin with direct access to audio content,
// enabling features like spectral editing, chord detection, and time-stretching
// without real-time playback.

#include <string>
#include <string_view>
#include <cstdint>
#include <vector>
#include <functional>

namespace pulp::format {

/// ARA role capabilities
enum class AraRole {
    None = 0,
    PlaybackRenderer = 1,
    EditorRenderer = 2,
    EditorView = 4
};

/// ARA document controller — manages the plugin's view of the DAW's audio content.
/// This is a stub interface. Full implementation requires the ARA SDK.
class AraDocumentController {
public:
    virtual ~AraDocumentController() = default;

    /// Called when the host creates a new document.
    virtual void begin_editing() {}

    /// Called when the host finishes editing the document.
    virtual void end_editing() {}

    /// Called when audio source data is available.
    virtual void notify_audio_source_content_changed(int64_t audio_source_id) {
        (void)audio_source_id;
    }

    /// Get the supported ARA roles for this plugin.
    virtual int supported_roles() const { return 0; }

    /// Whether this plugin supports ARA.
    virtual bool is_ara_supported() const { return false; }

    /// ARA factory name (displayed in the DAW).
    virtual std::string ara_factory_name() const { return ""; }
};

/// Check if the current host supports ARA.
/// Returns false if ARA is not available (most hosts).
bool host_supports_ara();

}  // namespace pulp::format
