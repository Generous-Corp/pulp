// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace pulp::host::isolation {

inline constexpr std::uint32_t kAbi = 1;
inline constexpr std::uint32_t kMagic = 0x4739434c;
inline constexpr std::size_t kMappingBytes = 8192;
inline constexpr std::size_t kCheckpointBytes = 1 << 20;
inline constexpr std::uint32_t kFrames = 64;
inline constexpr std::uint32_t kChannels = 2;
inline constexpr std::uint32_t kSlots = 3;

enum class BlockResult : std::uint32_t { ok = 1, plugin_error, invalid_audio };

struct Block {
    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;
    std::uint32_t frames = kFrames;
    std::uint32_t channels = kChannels;
    BlockResult result = BlockResult::ok;
    std::uint32_t reserved = 0;
    std::array<float, kFrames * kChannels> audio{};
};
static_assert(sizeof(Block) == 544);
static_assert(std::is_trivially_copyable_v<Block>);

// Each lane has exactly one producer and one consumer. ready=1 transfers
// exclusive payload ownership to the consumer (release/acquire); ready=0
// transfers it back. Neither side touches a payload it does not own.
struct alignas(64) Slot {
    std::atomic<std::uint32_t> ready{0};
    std::array<std::byte, 60> separator{};
    Block block;
};
static_assert(sizeof(Slot) == 640);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

struct alignas(64) Mapping {
    std::uint32_t magic = kMagic;
    std::uint32_t abi = kAbi;
    std::uint64_t generation = 0;
    std::array<std::byte, 48> separator{};
    std::array<Slot, kSlots> requests;
    std::array<Slot, kSlots> completions;
    std::array<std::byte, kMappingBytes - 64 - 6 * sizeof(Slot)> reserved{};
};
static_assert(sizeof(Mapping) == kMappingBytes);
static_assert(offsetof(Mapping, requests) == 64);

enum class Command : std::uint32_t {
    hello = 1,
    prepare,
    stop_processing,
    checkpoint,
    restore,
    destroy
};

// Local arm64 ABI only. The lifecycle channel is private and inherited; records
// are fixed width and carry no addresses. Payloads are independently capped.
struct ControlRecord {
    std::uint32_t magic = kMagic;
    std::uint32_t abi = kAbi;
    Command command = Command::hello;
    std::uint32_t result = 0;
    std::uint64_t generation = 0;
    std::uint64_t payload_bytes = 0;
    std::uint64_t volume = 0;
    std::uint64_t file = 0;
    std::uint64_t file_generation = 0;
    std::uint64_t process_id = 0;
    std::array<char, 65> plugin_sha256{};
    std::array<char, 65> worker_sha256{};
    std::array<char, 65> build{};
};
static_assert(std::is_trivially_copyable_v<ControlRecord>);

inline bool valid_block(const Block& block, std::uint64_t generation,
                        std::uint64_t sequence) noexcept {
    return block.generation == generation && block.sequence == sequence &&
           block.frames == kFrames && block.channels == kChannels &&
           block.result == BlockResult::ok && block.reserved == 0;
}

} // namespace pulp::host::isolation
