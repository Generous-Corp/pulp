#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pulp::gpu_audio::detail {

// Dawn-free chronological collector. The transport, not this reducer, owns the
// continuously primed CPU fallback. Callback code may only publish a watermark
// and claim/release a ready block; it never touches terminal or OLA state.
class SharedIoConvolutionExecutor {
  public:
    enum class Terminal : std::uint8_t { Success, Failed, DeviceLost };
    struct Config {
        std::uint32_t capacity = 0;
        std::uint32_t channels = 0;
        std::uint32_t block = 0;
        std::uint32_t fft_size = 0;
        std::uint32_t ir_length = 0;
    };

    // One-shot allocation. Use fence_and_reprime() for a later stream epoch so
    // prepare can never invalidate a callback-held span by reallocating it.
    bool prepare(const Config&, std::uint64_t epoch, std::uint64_t first_sequence);
    bool record_terminal(std::uint64_t epoch, std::uint64_t sequence, Terminal,
                         std::span<const float> interleaved_time,
                         Terminal* accepted_terminal = nullptr) noexcept;
    std::size_t collect() noexcept;
    void advance_callback_watermark(std::uint64_t sequence) noexcept;
    // A claimed span remains stable until release_ready(). Callers copy it
    // immediately and must release before this ring position can be reused.
    std::span<const float> take_ready(std::uint64_t sequence) noexcept;
    bool release_ready(std::uint64_t sequence) noexcept;
    // Non-RT only. PRECONDITION: the owner has stopped and joined callback
    // invocation before entering. The fence rejects calls that begin afterward;
    // it cannot synchronize a callback that already passed its fence load.
    // Reprime also refuses to invalidate a callback-held output lease.
    bool fence_and_reprime(std::uint64_t new_epoch, std::uint64_t first_sequence) noexcept;
    bool fenced() const noexcept {
        return fenced_.load(std::memory_order_acquire);
    }
    std::uint64_t valid_from_sequence() const noexcept {
        return valid_from_;
    }
    std::uint64_t next_sequence() const noexcept {
        return next_;
    }

  private:
    struct Entry {
        std::uint64_t epoch = 0;
        std::uint64_t sequence = 0;
        Terminal terminal = Terminal::Failed;
        bool present = false;
    };
    static constexpr std::uint64_t kEmpty = ~std::uint64_t{0};
    static constexpr std::uint64_t kClaimed = std::uint64_t{1} << 63;

    Config c_{};
    std::vector<Entry> entries_;
    std::vector<float> terminal_, carry_, ready_;
    // unique_ptr avoids vector<atomic> relocation while retaining fixed storage.
    std::unique_ptr<std::atomic<std::uint64_t>[]> ready_state_;
    std::uint64_t epoch_ = 0;
    std::uint64_t next_ = 0;
    std::uint64_t valid_from_ = 0;
    std::atomic<std::uint64_t> watermark_{0};
    std::atomic<bool> watermark_set_{false};
    std::atomic<bool> fenced_{false};
    bool prepared_ = false;
};
} // namespace pulp::gpu_audio::detail
