#pragma once

#include "dawn/dawn_proc.h"
#include "dawn/native/DawnNative.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <type_traits>

namespace pulp::test {

// Test-only process-global Dawn proc-table interposer. Its lifetime requires
// exclusive ownership of Dawn's process-global proc table: no other thread may
// call Dawn, install a proc table, or have a Dawn call in progress during
// installation or restoration. Construct this before every Dawn object in the
// caller's scope so reverse destruction tears down the instance, devices,
// queues, and resources before this object's destructor runs. No Dawn call may
// still be in progress when destruction begins.
//
// This object installs and later restores the `dawn::native::GetProcs()`
// baseline. It does not discover or preserve an arbitrary table that another
// owner installed before construction. Nested or concurrent interposers fail
// closed.
class DawnTransferCallCounter {
  public:
    enum class InstallMode : std::uint8_t { InstallAndRestore, DeferredSingleInstall };
    // Byte fields are requested frontend API bytes. They do not observe copies
    // performed internally by a Dawn backend, writes made through a buffer mapped
    // at creation, or CPU memcpy into shared/mapped storage.
    struct Snapshot {
        std::uint64_t queue_write_buffer_calls = 0;
        std::uint64_t queue_write_buffer_bytes = 0;
        std::uint64_t copy_buffer_to_buffer_calls = 0;
        std::uint64_t copy_buffer_to_buffer_bytes = 0;
        std::uint64_t buffer_map_async_calls = 0;
        std::uint64_t buffer_map_async_bytes = 0;
        std::uint64_t queue_submit_calls = 0;
        std::uint64_t submitted_command_buffers = 0;
    };

    explicit DawnTransferCallCounter(InstallMode mode = InstallMode::InstallAndRestore)
        : mode_(mode), original_(dawn::native::GetProcs()),
          interposed_(std::make_unique<DawnProcTable>(original_)) {
        DawnTransferCallCounter* expected = nullptr;
        if (!active_.compare_exchange_strong(expected, this, std::memory_order_acq_rel))
            std::abort();

        interposed_->queueWriteBuffer = &count_queue_write_buffer;
        interposed_->commandEncoderCopyBufferToBuffer = &count_copy_buffer_to_buffer;
        interposed_->bufferMapAsync = &count_buffer_map_async;
        interposed_->queueSubmit = &count_queue_submit;
        if (mode_ == InstallMode::InstallAndRestore)
            dawnProcSetProcs(interposed_.get());
    }

    ~DawnTransferCallCounter() {
        if (active_.load(std::memory_order_acquire) != this)
            std::abort();

        // The caller must already have torn down every Dawn object. Restore the
        // entry points before clearing active_ so a new public call cannot enter
        // a wrapper without an owning counter.
        if (mode_ == InstallMode::InstallAndRestore) {
            dawnProcSetProcs(&original_);
        } else {
            // The isolated child exits immediately after this object dies. Keep
            // the installed table's storage valid until process teardown without
            // a second process-global setter mutation.
            interposed_.release();
        }
        active_.store(nullptr, std::memory_order_release);
    }

    DawnTransferCallCounter(const DawnTransferCallCounter&) = delete;
    DawnTransferCallCounter& operator=(const DawnTransferCallCounter&) = delete;

    const DawnProcTable* deferred_proc_table() const noexcept {
        return mode_ == InstallMode::DeferredSingleInstall ? interposed_.get() : nullptr;
    }

    // Acceptance snapshots must be taken at a quiescent boundary. Each field is
    // individually atomic, but the returned struct is not one atomic aggregate.
    Snapshot snapshot() const noexcept {
        return {
            .queue_write_buffer_calls = queue_write_buffer_calls_.load(std::memory_order_relaxed),
            .queue_write_buffer_bytes = queue_write_buffer_bytes_.load(std::memory_order_relaxed),
            .copy_buffer_to_buffer_calls =
                copy_buffer_to_buffer_calls_.load(std::memory_order_relaxed),
            .copy_buffer_to_buffer_bytes =
                copy_buffer_to_buffer_bytes_.load(std::memory_order_relaxed),
            .buffer_map_async_calls = buffer_map_async_calls_.load(std::memory_order_relaxed),
            .buffer_map_async_bytes = buffer_map_async_bytes_.load(std::memory_order_relaxed),
            .queue_submit_calls = queue_submit_calls_.load(std::memory_order_relaxed),
            .submitted_command_buffers = submitted_command_buffers_.load(std::memory_order_relaxed),
        };
    }

    // Reset only at a quiescent boundary. Concurrent increments can land between
    // these stores and be erased or split across measurement intervals.
    void reset() noexcept {
        queue_write_buffer_calls_.store(0, std::memory_order_relaxed);
        queue_write_buffer_bytes_.store(0, std::memory_order_relaxed);
        copy_buffer_to_buffer_calls_.store(0, std::memory_order_relaxed);
        copy_buffer_to_buffer_bytes_.store(0, std::memory_order_relaxed);
        buffer_map_async_calls_.store(0, std::memory_order_relaxed);
        buffer_map_async_bytes_.store(0, std::memory_order_relaxed);
        queue_submit_calls_.store(0, std::memory_order_relaxed);
        submitted_command_buffers_.store(0, std::memory_order_relaxed);
    }

  private:
    static DawnTransferCallCounter& active() noexcept {
        auto* counter = active_.load(std::memory_order_acquire);
        if (counter == nullptr)
            std::abort();
        return *counter;
    }

    static std::uint64_t as_u64(std::size_t value) noexcept {
        if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
            if (value > static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max()))
                return std::numeric_limits<std::uint64_t>::max();
        }
        return static_cast<std::uint64_t>(value);
    }

    static void add_saturating(std::atomic<std::uint64_t>& counter, std::uint64_t amount) noexcept {
        auto current = counter.load(std::memory_order_relaxed);
        for (;;) {
            const auto room = std::numeric_limits<std::uint64_t>::max() - current;
            const auto next =
                amount > room ? std::numeric_limits<std::uint64_t>::max() : current + amount;
            if (counter.compare_exchange_weak(current, next, std::memory_order_relaxed))
                return;
        }
    }

    static std::uint64_t mapped_bytes(DawnTransferCallCounter& counter, WGPUBuffer buffer,
                                      std::size_t offset, std::size_t size) noexcept {
        if (size != WGPU_WHOLE_MAP_SIZE)
            return as_u64(size);

        // Call the saved implementation entry point directly. Calling the
        // process-global wgpuBufferGetSize entry point here would re-enter the
        // installed proc table and make wrapper behavior depend on public API
        // dispatch.
        const std::uint64_t buffer_size = counter.original_.bufferGetSize(buffer);
        const std::uint64_t map_offset = as_u64(offset);
        return map_offset < buffer_size ? buffer_size - map_offset : 0;
    }

    static void count_queue_write_buffer(WGPUQueue queue, WGPUBuffer buffer,
                                         std::uint64_t buffer_offset, const void* data,
                                         std::size_t size) {
        auto& counter = active();
        add_saturating(counter.queue_write_buffer_calls_, 1);
        add_saturating(counter.queue_write_buffer_bytes_, as_u64(size));
        counter.original_.queueWriteBuffer(queue, buffer, buffer_offset, data, size);
    }

    static void count_copy_buffer_to_buffer(WGPUCommandEncoder command_encoder, WGPUBuffer source,
                                            std::uint64_t source_offset, WGPUBuffer destination,
                                            std::uint64_t destination_offset, std::uint64_t size) {
        auto& counter = active();
        add_saturating(counter.copy_buffer_to_buffer_calls_, 1);
        add_saturating(counter.copy_buffer_to_buffer_bytes_, size);
        counter.original_.commandEncoderCopyBufferToBuffer(command_encoder, source, source_offset,
                                                           destination, destination_offset, size);
    }

    static WGPUFuture count_buffer_map_async(WGPUBuffer buffer, WGPUMapMode mode,
                                             std::size_t offset, std::size_t size,
                                             WGPUBufferMapCallbackInfo callback_info) {
        auto& counter = active();
        add_saturating(counter.buffer_map_async_calls_, 1);
        add_saturating(counter.buffer_map_async_bytes_,
                       mapped_bytes(counter, buffer, offset, size));
        return counter.original_.bufferMapAsync(buffer, mode, offset, size, callback_info);
    }

    static void count_queue_submit(WGPUQueue queue, std::size_t command_count,
                                   const WGPUCommandBuffer* commands) {
        auto& counter = active();
        add_saturating(counter.queue_submit_calls_, 1);
        add_saturating(counter.submitted_command_buffers_, as_u64(command_count));
        counter.original_.queueSubmit(queue, command_count, commands);
    }

    static_assert(std::is_same_v<decltype(&count_queue_write_buffer), WGPUProcQueueWriteBuffer>);
    static_assert(std::is_same_v<decltype(&count_copy_buffer_to_buffer),
                                 WGPUProcCommandEncoderCopyBufferToBuffer>);
    static_assert(std::is_same_v<decltype(&count_buffer_map_async), WGPUProcBufferMapAsync>);
    static_assert(std::is_same_v<decltype(&count_queue_submit), WGPUProcQueueSubmit>);

    InstallMode mode_ = InstallMode::InstallAndRestore;
    DawnProcTable original_{};
    std::unique_ptr<DawnProcTable> interposed_;

    std::atomic<std::uint64_t> queue_write_buffer_calls_{0};
    std::atomic<std::uint64_t> queue_write_buffer_bytes_{0};
    std::atomic<std::uint64_t> copy_buffer_to_buffer_calls_{0};
    std::atomic<std::uint64_t> copy_buffer_to_buffer_bytes_{0};
    std::atomic<std::uint64_t> buffer_map_async_calls_{0};
    std::atomic<std::uint64_t> buffer_map_async_bytes_{0};
    std::atomic<std::uint64_t> queue_submit_calls_{0};
    std::atomic<std::uint64_t> submitted_command_buffers_{0};

    inline static std::atomic<DawnTransferCallCounter*> active_{nullptr};
};

} // namespace pulp::test
