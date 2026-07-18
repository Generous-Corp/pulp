#pragma once

#include <pulp/audio/device.hpp>

namespace pulp::format {

/// Optional capability for processors and graph runtimes which own auxiliary
/// realtime rendering threads.
///
/// This stays separate from Processor so opting in does not widen the node ABI.
class AudioWorkgroupClient {
public:
    /// Publish the borrowed workgroup for subsequent renders, or null to remove
    /// it. This may run on the realtime thread and must not allocate, lock,
    /// block, or call host APIs.
    virtual void set_audio_workgroup(void* workgroup) noexcept = 0;

    /// Off-RT barrier for borrowed-handle teardown. Returns only after every
    /// auxiliary worker has acknowledged the most recently published value.
    /// Never call this from an audio callback or AU render-context observer.
    virtual void wait_for_audio_workgroup_update() noexcept = 0;

protected:
    ~AudioWorkgroupClient() = default;
};

/// Production device-to-renderer handoff shared by standalone and timeline
/// hosts. The device owns the borrowed handle.
inline void bind_audio_device_workgroup(AudioWorkgroupClient& client,
                                        audio::AudioDevice* device) {
    client.set_audio_workgroup(device ? device->callback_workgroup() : nullptr);
    if (device) {
        device->set_workgroup_change_callback(
            [&client](void* workgroup) {
                client.set_audio_workgroup(workgroup);
                if (!workgroup) client.wait_for_audio_workgroup_update();
            });
    }
}

/// Stop-time ownership barrier: release the device-owned handle on every
/// worker before close() can invalidate it. The device callback must already
/// be stopped; this helper is intentionally allowed to block.
inline void close_audio_device_after_workgroup_release(
    AudioWorkgroupClient* client, audio::AudioDevice& device) noexcept {
    // A live device can still be finishing a retarget after an earlier null
    // publication. Disable future changes and drain that in-flight publication
    // before the final explicit removal and close.
    device.quiesce_workgroup_changes();
    if (client) {
        client->set_audio_workgroup(nullptr);
        client->wait_for_audio_workgroup_update();
    }
    device.close();
}

} // namespace pulp::format
