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

/// True when Pulp was built with the Celemony ARA SDK (PULP_ENABLE_ARA=ON
/// + PULP_ARA_SDK_DIR pointing at a valid checkout). Separate from
/// host_supports_ara(): SDK-compiled-in is a build-time fact; host
/// support is resolved at load time per format adapter.
/// Workstream 06 slice 6.1.
bool ara_sdk_compiled_in();

/// Highest ARA API generation Pulp was compiled against. Returns 0 when
/// PULP_HAS_ARA is not set. When non-zero, the value matches the ARA
/// SDK's `kARAAPIGeneration_*` enum — e.g. `6` for `2_3_Final`.
/// Callers gate feature use by comparing to a known constant.
int ara_sdk_generation();

/// Well-known extension id for the CLAP-ARA companion factory.
/// Matches Celemony's convention for identifying the ARA extension
/// inside a CLAP plugin's `clap_plugin::get_extension` callback.
/// Workstream 06 slice 6.5 — the adapter surfaces this to CLAP hosts
/// that know how to pair a CLAP plugin with an ARA document controller.
constexpr const char* kClapAraFactoryExtension = "com.celemony.ara/clap-factory-v1";

/// Return an opaque pointer to an `ARA::ARAFactory`-compatible struct
/// when the processor is ARA-aware and Pulp was built with PULP_HAS_ARA,
/// otherwise nullptr. Called by format-adapter `get_extension` handlers.
/// The returned pointer is owned by Pulp and lives as long as the
/// processor. `void*` at the public boundary so non-ARA TUs do not need
/// to pull `ARA_API/ARAInterface.h`.
const void* ara_companion_factory_for(class AraDocumentController* controller);

}  // namespace pulp::format
