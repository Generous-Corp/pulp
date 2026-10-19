#pragma once

#include <pulp/audio/buffer.hpp>

#include <cstddef>

namespace pulp::format {

/// A host-owned, realtime-safe view provider for declared processor input
/// buses. The callback must be installed before prepare() and remain immutable
/// while the processor is running. It returns borrowed audio storage for the
/// current block; the host owns the storage and its lifetime.
///
/// Route selection belongs to the control-thread host/catalog layer. The audio
/// callback receives only this pre-bound POD handle, so it never enumerates
/// tracks, allocates, locks, or resolves stable IDs.
struct AudioInputBusProvider {
    using ReadFn = bool (*)(void* context,
                            std::size_t bus_index,
                            std::size_t frame_count,
                            audio::BufferView<const float>& output) noexcept;

    void* context = nullptr;
    ReadFn read = nullptr;

    explicit operator bool() const noexcept {
        return context != nullptr && read != nullptr;
    }
};

} // namespace pulp::format
