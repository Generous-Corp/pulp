#pragma once

#if defined(__APPLE__)

#include <AudioToolbox/AudioToolbox.h>

#include <cstddef>
#include <cstring>

namespace pulp::format::detail {

/// Validate the structural portion of a non-interleaved AudioBufferList.
/// Storage is checked separately because AUv3 output callbacks may legally
/// arrive with null/undersized mData and ask the Audio Unit to provide it.
inline bool audio_buffer_list_shape_matches(const AudioBufferList* list,
                                            UInt32 expected_buffers,
                                            UInt32 expected_channels_per_buffer = 1) noexcept {
    if (!list || list->mNumberBuffers != expected_buffers) return false;
    for (UInt32 i = 0; i < expected_buffers; ++i) {
        if (list->mBuffers[i].mNumberChannels != expected_channels_per_buffer)
            return false;
    }
    return true;
}

/// Require complete writable/readable storage for every buffer in an ABL.
inline bool audio_buffer_list_has_storage(const AudioBufferList* list,
                                          UInt32 frames,
                                          UInt32 bytes_per_frame) noexcept {
    if (!list) return false;
    const std::size_t required =
        static_cast<std::size_t>(frames) * bytes_per_frame;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i) {
        if (!list->mBuffers[i].mData ||
            list->mBuffers[i].mDataByteSize < required)
            return false;
    }
    return true;
}

/// Silence every writable byte the host actually supplied. Null buffers are
/// intentionally skipped: fail-closed means no dereference as well as no DSP.
inline void zero_audio_buffer_list(AudioBufferList* list) noexcept {
    if (!list) return;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i) {
        auto& buffer = list->mBuffers[i];
        if (buffer.mData && buffer.mDataByteSize > 0)
            std::memset(buffer.mData, 0, buffer.mDataByteSize);
    }
}

}  // namespace pulp::format::detail

#endif
