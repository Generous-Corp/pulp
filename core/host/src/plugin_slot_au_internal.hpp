// plugin_slot_au_internal.hpp — testable internals for the AU host slot.
//
// Exposes the AudioBufferList construction used by AuSlot::process so the
// audio-thread no-allocation invariant can be exercised directly by
// test/test_plugin_slot_au.mm without loading a real AudioUnit (which is not
// reliably possible in headless CI). Apple-only; AudioBufferList lives in
// CoreAudioTypes. Same isolation contract as cmd_version_internal.hpp.
#pragma once

#include <pulp/audio/buffer.hpp>

#include <CoreAudioTypes/CoreAudioTypes.h>

#include <cstdint>
#include <vector>

namespace pulp::host::au_internal {

// Byte size of an AudioBufferList holding `channels` (>= 1) AudioBuffers.
// AudioBufferList carries one inline AudioBuffer, so a list of N buffers needs
// N-1 trailing AudioBuffers past the struct.
inline std::size_t audio_buffer_list_bytes(int channels) {
    const std::size_t n = channels > 0 ? static_cast<std::size_t>(channels) : 1;
    return sizeof(AudioBufferList) + sizeof(AudioBuffer) * (n - 1);
}

// Ensure `storage` can back an AudioBufferList for `channels`. Call once from
// prepare() so the per-block refill on the audio thread never reallocates.
// Returns true iff it grew the buffer (only the cold/prepare path); false means
// the existing allocation was reused — the steady-state, RT-safe outcome.
inline bool reserve_audio_buffer_list(std::vector<std::uint8_t>& storage,
                                      int channels) {
    const std::size_t need = audio_buffer_list_bytes(channels);
    if (storage.size() >= need) return false;
    storage.assign(need, std::uint8_t{0});
    return true;
}

// Fill a (pre-sized) `storage` with an AudioBufferList of `channels`
// non-interleaved buffers, each `frame_bytes` long, pointing at the current
// frame of each `output` channel. When `storage` was reserved in prepare()
// for at least `channels`, this performs ZERO heap allocation — the whole
// point of A2 (it ran inside the audio callback). The reserve call here is a
// belt-and-suspenders no-op on the RT path; it only ever grows if a caller
// skipped prepare() sizing.
inline AudioBufferList* fill_output_audio_buffer_list(
        std::vector<std::uint8_t>& storage,
        pulp::audio::BufferView<float>& output,
        int channels,
        std::uint32_t frame_bytes) {
    reserve_audio_buffer_list(storage, channels);
    auto* abl = reinterpret_cast<AudioBufferList*>(storage.data());
    abl->mNumberBuffers = static_cast<UInt32>(channels);
    for (int c = 0; c < channels; ++c) {
        abl->mBuffers[c].mNumberChannels = 1;
        abl->mBuffers[c].mDataByteSize = frame_bytes;
        abl->mBuffers[c].mData = output.channel_ptr(static_cast<std::size_t>(c));
    }
    return abl;
}

}  // namespace pulp::host::au_internal
