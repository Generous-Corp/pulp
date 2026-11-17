// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <type_traits>

// Platforms with a worker transport. Every other platform refuses before spawning.
#if defined(__APPLE__) && defined(__aarch64__)
#define PULP_ISOLATED_CLAP_POSIX_TRANSPORT 1
#elif defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
#define PULP_ISOLATED_CLAP_WINDOWS_TRANSPORT 1
#endif

namespace pulp::host::isolation {

inline constexpr std::uint32_t kAbi = 1;
inline constexpr std::uint32_t kMagic = 0x4739434c;
inline constexpr std::size_t kMappingBytes = 8192;
inline constexpr std::size_t kCheckpointBytes = 1 << 20;
inline constexpr std::uint32_t kSampleRate = 48000;
inline constexpr std::uint32_t kFrames = 64;
inline constexpr std::uint32_t kChannels = 2;
// The single supported profile (stereo, 64 frames, 48 kHz), named on the worker
// command line. The name is shared by every transport.
inline constexpr const char* kProfileName = "MacClapStereo64V1";
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

// Local ABI only: both ends are built from the same sources for one platform
// (Apple arm64 or Windows x64). The lifecycle channel is private and inherited;
// records are fixed width and carry no addresses, except the Windows section
// handle value, which is meaningful only inside the named worker process.
// Payloads are independently capped.
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

/// The hello contract on the parent side, shared by every transport: the reply
/// must echo every binding field the parent sent, with the worker's own process
/// id and a mapping of exactly kMappingBytes.
inline bool hello_reply_matches(const ControlRecord& sent, const ControlRecord& reply,
                                std::uint64_t process_id) noexcept {
    return reply.command == Command::hello && reply.result == 1 &&
           reply.generation == sent.generation && reply.process_id == process_id &&
           reply.payload_bytes == kMappingBytes && reply.payload_bytes == sent.payload_bytes &&
           reply.volume == sent.volume && reply.file == sent.file &&
           reply.file_generation == sent.file_generation &&
           reply.plugin_sha256 == sent.plugin_sha256 && reply.worker_sha256 == sent.worker_sha256 &&
           reply.build == sent.build;
}

/// A fixed record field as text, ending at its first NUL or at its full width.
inline std::string_view record_text(const std::array<char, 65>& field) noexcept {
    std::size_t size = 0;
    while (size < field.size() && field[size] != '\0')
        ++size;
    return {field.data(), size};
}

/// The hello contract on the worker side, shared by every transport: the header
/// fields a worker checks before it touches the shared mapping.
inline bool hello_header_ok(const ControlRecord& hello) noexcept {
    return hello.command == Command::hello && hello.generation != 0 &&
           hello.payload_bytes == kMappingBytes;
}

/// The identity a worker proves before any vendor code runs: its own executable
/// hash, the plugin hash and the build fingerprint equal what the parent bound.
inline bool hello_identity_matches(const ControlRecord& hello, std::string_view worker_sha256,
                                   std::string_view plugin_sha256,
                                   std::string_view build) noexcept {
    return record_text(hello.worker_sha256) == worker_sha256 &&
           record_text(hello.plugin_sha256) == plugin_sha256 && record_text(hello.build) == build;
}

/// The file whose bytes identify a CLAP selection. A macOS bundle is hashed by
/// its executable while the bundle root is still what CLAP init receives; a
/// plain module file (every Windows .clap) is hashed as itself.
inline std::filesystem::path clap_hash_path(const std::filesystem::path& selection) {
    std::error_code ec;
    if (std::filesystem::is_directory(selection, ec))
        return selection / "Contents" / "MacOS" / selection.stem();
    return selection;
}

} // namespace pulp::host::isolation
