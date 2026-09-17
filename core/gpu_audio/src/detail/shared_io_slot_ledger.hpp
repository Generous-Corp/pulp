#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace pulp::gpu_audio::detail {

// Ownership ledger for fixed CPU/GPU-shared input/output slots. prepare() is
// the only allocating operation; all other methods are allocation-free. The
// dispatcher serializes calls into this type. It deliberately owns no clock,
// callback, fallback, transport, or plugin-delay policy.
class SharedIoSlotLedger {
  public:
    SharedIoSlotLedger() = default;
    SharedIoSlotLedger(const SharedIoSlotLedger&) = delete;
    SharedIoSlotLedger& operator=(const SharedIoSlotLedger&) = delete;
    SharedIoSlotLedger(SharedIoSlotLedger&&) = delete;
    SharedIoSlotLedger& operator=(SharedIoSlotLedger&&) = delete;

    struct SlotToken {
        std::uint32_t slot = 0;
        std::uint64_t preparation_epoch = 0;
        std::uint64_t stream_sequence = 0;
        std::uint64_t slot_generation = 0;

        friend bool operator==(const SlotToken&, const SlotToken&) = default;
    };

    enum class GpuCompletion { Success, Failed };

    // Host/quiescent only. Allocates the fixed slot table and starts a new
    // preparation epoch. Re-preparation refuses live slots.
    bool prepare(std::uint32_t capacity) {
        if (capacity == 0 || (prepared_ && !quiescent()) || !advance_epoch())
            return false;

        slots_.assign(capacity, Slot{});
        prepared_ = true;
        retiring_ = false;
        return true;
    }

    bool prepared() const noexcept {
        return prepared_;
    }
    bool retiring() const noexcept {
        return retiring_;
    }
    std::uint32_t capacity() const noexcept {
        return static_cast<std::uint32_t>(slots_.size());
    }
    std::uint64_t preparation_epoch() const noexcept {
        return preparation_epoch_;
    }

    // Returns capacity that acquire() can reserve now. Retirement exposes no
    // available capacity, and a free slot whose generation is exhausted stays
    // unavailable until a new preparation epoch resets its generation.
    std::size_t available_slots() const noexcept {
        if (!prepared_ || retiring_)
            return 0;
        std::size_t count = 0;
        for (const auto& slot : slots_) {
            if (slot.state == SlotState::Free &&
                slot.last_generation != std::numeric_limits<std::uint64_t>::max()) {
                ++count;
            }
        }
        return count;
    }

    // Reserves one free physical slot for an absolute stream sequence. A second
    // live slot for the same sequence is rejected so exact-sequence lookup is
    // unambiguous.
    std::optional<SlotToken> acquire(std::uint64_t stream_sequence) noexcept {
        if (!prepared_ || retiring_ || has_live_sequence(stream_sequence))
            return std::nullopt;

        for (std::uint32_t index = 0; index < slots_.size(); ++index) {
            Slot& slot = slots_[index];
            if (slot.state != SlotState::Free || !advance_generation(slot))
                continue;

            slot.token = {
                .slot = index,
                .preparation_epoch = preparation_epoch_,
                .stream_sequence = stream_sequence,
                .slot_generation = slot.last_generation,
            };
            slot.state = SlotState::Acquired;
            return slot.token;
        }
        return std::nullopt;
    }

    // Publishes CPU-written input to the serialized dispatcher.
    bool publish(const SlotToken& token) noexcept {
        return !retiring_ && transition(token, SlotState::Acquired, SlotState::Published);
    }

    // Claims the exact published token for GPU submission. A delayed request
    // cannot claim a reused slot or a sequence from another preparation. The
    // transition establishes active GPU ownership immediately; after it succeeds,
    // only complete_gpu() can retire that ownership, even if delivery expires first.
    std::optional<SlotToken> claim_submission(const SlotToken& token) noexcept {
        if (retiring_ || !transition(token, SlotState::Published, SlotState::Submitted))
            return std::nullopt;
        return token;
    }

    // Marks the timeline delivery as expired without retiring GPU ownership.
    // An output that was already complete becomes discardable; an in-flight GPU
    // slot remains pinned until complete_gpu() arrives for the exact token.
    bool expire_delivery(const SlotToken& token) noexcept {
        Slot* slot = matching_slot(token);
        if (slot == nullptr)
            return false;
        if (slot->state == SlotState::Submitted) {
            slot->state = SlotState::ExpiredAwaitingCompletion;
            return true;
        }
        if (slot->state == SlotState::OutputReady) {
            slot->state = SlotState::Discardable;
            return true;
        }
        return false;
    }

    // Records actual GPU retirement. Success makes an on-time result available;
    // failure, or either outcome after expiry, requires explicit discard(). No
    // completion path makes a physical slot reusable by itself.
    bool complete_gpu(const SlotToken& token, GpuCompletion completion) noexcept {
        Slot* slot = matching_slot(token);
        if (slot == nullptr)
            return false;
        if (slot->state == SlotState::Submitted) {
            slot->state = completion == GpuCompletion::Success ? SlotState::OutputReady
                                                               : SlotState::Discardable;
            return true;
        }
        if (slot->state == SlotState::ExpiredAwaitingCompletion) {
            slot->state = SlotState::Discardable;
            return true;
        }
        return false;
    }

    // Claims only an output from the caller's preparation and exact expected
    // absolute sequence. Wrong-epoch requests and sequence holes leave ready
    // results untouched; outputs for other sequences remain available.
    std::optional<SlotToken> acquire_output(std::uint64_t expected_preparation_epoch,
                                            std::uint64_t expected_sequence) noexcept {
        if (!prepared_ || expected_preparation_epoch != preparation_epoch_)
            return std::nullopt;
        for (auto& slot : slots_) {
            if (slot.state == SlotState::OutputReady &&
                slot.token.stream_sequence == expected_sequence) {
                slot.state = SlotState::OutputClaimed;
                return slot.token;
            }
        }
        return std::nullopt;
    }

    bool release(const SlotToken& token) noexcept {
        return free_matching(token, SlotState::OutputClaimed);
    }

    // Discards storage only while no GPU access is active. Submitted and
    // expired-in-flight slots remain pinned until complete_gpu().
    bool discard(const SlotToken& token) noexcept {
        Slot* slot = matching_slot(token);
        if (slot == nullptr)
            return false;
        switch (slot->state) {
        case SlotState::Acquired:
        case SlotState::Published:
        case SlotState::OutputReady:
        case SlotState::Discardable:
            make_free(*slot);
            return true;
        case SlotState::Free:
        case SlotState::Submitted:
        case SlotState::ExpiredAwaitingCompletion:
        case SlotState::OutputClaimed:
            return false;
        }
        return false;
    }

    void begin_retirement() noexcept {
        if (prepared_)
            retiring_ = true;
    }

    // Retirement owner inspection. This exposes identity, not mutable state, so
    // the arena can ask every live generation to relinquish CPU-owned storage
    // while leaving submitted work pinned for its real completion.
    std::optional<SlotToken> active_token(std::uint32_t slot_index) const noexcept {
        if (!prepared_ || slot_index >= slots_.size() ||
            slots_[slot_index].state == SlotState::Free) {
            return std::nullopt;
        }
        return slots_[slot_index].token;
    }

    // Host/quiescent teardown only. Once retirement has stopped all intake, CPU
    // ownership may be abandoned even if output had been claimed: destruction
    // invalidates that span. Active GPU ownership is still never released here.
    bool discard_after_cpu_quiescence(const SlotToken& token) noexcept {
        if (!retiring_)
            return false;
        Slot* slot = matching_slot(token);
        if (slot == nullptr)
            return false;
        switch (slot->state) {
        case SlotState::Acquired:
        case SlotState::Published:
        case SlotState::OutputReady:
        case SlotState::OutputClaimed:
        case SlotState::Discardable:
            make_free(*slot);
            return true;
        case SlotState::Free:
        case SlotState::Submitted:
        case SlotState::ExpiredAwaitingCompletion:
            return false;
        }
        return false;
    }

    bool quiescent() const noexcept {
        if (!prepared_)
            return false;
        for (const auto& slot : slots_) {
            if (slot.state != SlotState::Free)
                return false;
        }
        return true;
    }

    // Starts a fresh epoch without allocation after retirement has drained all
    // slots. Epoch identity makes callbacks from the previous preparation stale.
    bool reset_when_quiescent() noexcept {
        if (!prepared_ || !retiring_ || !quiescent() || !advance_epoch())
            return false;
        for (auto& slot : slots_)
            slot = Slot{};
        retiring_ = false;
        return true;
    }

  private:
    enum class SlotState {
        Free,
        Acquired,
        Published,
        Submitted,
        ExpiredAwaitingCompletion,
        OutputReady,
        OutputClaimed,
        Discardable,
    };

    struct Slot {
        SlotState state = SlotState::Free;
        SlotToken token;
        std::uint64_t last_generation = 0;
    };

    bool advance_epoch() noexcept {
        if (preparation_epoch_ == std::numeric_limits<std::uint64_t>::max())
            return false;
        ++preparation_epoch_;
        return true;
    }

    static bool advance_generation(Slot& slot) noexcept {
        if (slot.last_generation == std::numeric_limits<std::uint64_t>::max())
            return false;
        ++slot.last_generation;
        return true;
    }

    bool has_live_sequence(std::uint64_t sequence) const noexcept {
        for (const auto& slot : slots_) {
            if (slot.state != SlotState::Free && slot.token.stream_sequence == sequence)
                return true;
        }
        return false;
    }

    Slot* matching_slot(const SlotToken& token) noexcept {
        if (!prepared_ || token.slot >= slots_.size())
            return nullptr;
        Slot& slot = slots_[token.slot];
        return slot.state != SlotState::Free && slot.token == token ? &slot : nullptr;
    }

    bool transition(const SlotToken& token, SlotState from, SlotState to) noexcept {
        Slot* slot = matching_slot(token);
        if (slot == nullptr || slot->state != from)
            return false;
        slot->state = to;
        return true;
    }

    bool free_matching(const SlotToken& token, SlotState expected) noexcept {
        Slot* slot = matching_slot(token);
        if (slot == nullptr || slot->state != expected)
            return false;
        make_free(*slot);
        return true;
    }

    static void make_free(Slot& slot) noexcept {
        slot.state = SlotState::Free;
        slot.token = {};
    }

    std::vector<Slot> slots_;
    std::uint64_t preparation_epoch_ = 0;
    bool prepared_ = false;
    bool retiring_ = false;
};

} // namespace pulp::gpu_audio::detail
