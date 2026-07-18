#pragma once

#include <pulp/audio/device.hpp>

namespace pulp::format {

/// Optional capability for processors and graph runtimes which own auxiliary
/// realtime rendering threads.
///
/// This stays separate from Processor so opting in does not widen the node ABI.
class AudioWorkgroupClient {
public:
    /// Publish the host/device-owned workgroup for subsequent renders, or null
    /// to remove it. The publisher for one client must be serialized (AU calls
    /// its observer on the render thread; CoreAudio serializes device changes).
    /// This may run on the realtime thread and must not allocate, lock, block,
    /// retain/release Objective-C/os objects, or call host APIs.
    ///
    /// AU lifetime follows Apple's render-context protocol: the context struct
    /// is callback-scoped, while its workgroup is the host-owned current render
    /// context that auxiliary realtime threads are instructed to join and leave
    /// when the next observer publication arrives. Standalone devices hold a
    /// caller-owned query reference until the explicit teardown barrier below.
    virtual void set_audio_workgroup(void* workgroup) noexcept = 0;

    /// Publish an AU render-context workgroup. Apple does not document
    /// os_retain/os_release as RT-safe and supplies no completion callback by
    /// which a host can know a late asynchronous join no longer references the
    /// previous context. Pulp therefore adopts it in a synchronous observer
    /// barrier: every worker, including cold sleepers, completes leave of the
    /// old context and acknowledges its new join result before the observer
    /// returns.
    virtual void set_audio_workgroup_from_render_context(
        void* workgroup) noexcept {
        set_audio_workgroup(workgroup);
    }

    /// RT-safe render-cycle preparation. Every worker completes the transition;
    /// false means at least one new join failed and the caller must render
    /// inline for this publication.
    virtual bool prepare_audio_workgroup_for_render() noexcept { return true; }

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
    if (device) {
        device->set_workgroup_change_callback(
            [&client](void* workgroup) {
                client.set_audio_workgroup(workgroup);
                if (!workgroup) client.wait_for_audio_workgroup_update();
            });
    } else {
        client.set_audio_workgroup(nullptr);
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
