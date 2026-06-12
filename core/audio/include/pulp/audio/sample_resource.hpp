#pragma once

#include <pulp/audio/audio_file.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace pulp::audio {

enum class SampleResourceStatus : uint8_t {
    Empty,
    Loaded,
    Missing,
    Oversized,
};

struct SampleResourceSnapshot {
    SampleResourceStatus status = SampleResourceStatus::Empty;
    uint64_t generation = 0;
    uint64_t frame_count = 0;
    uint32_t channel_count = 0;
    uint32_t sample_rate = 0;
    uint64_t byte_size = 0;
    uint64_t memory_budget_bytes = 0;
    std::shared_ptr<const AudioFileData> data;

    bool ready() const noexcept {
        return status == SampleResourceStatus::Loaded && data && !data->empty();
    }
};

struct SampleResourceDiagnostics {
    SampleResourceStatus status = SampleResourceStatus::Empty;
    uint64_t generation = 0;
    std::string path;
    std::string reason;
    uint64_t frame_count = 0;
    uint32_t channel_count = 0;
    uint32_t sample_rate = 0;
    uint64_t byte_size = 0;
    uint64_t memory_budget_bytes = 0;
};

class SampleResourceHandle {
public:
    SampleResourceSnapshot snapshot() const noexcept { return snapshot_; }
    SampleResourceDiagnostics diagnostics() const { return diagnostics_; }

    void clear() {
        snapshot_ = {};
        diagnostics_ = {};
    }

    void publish_missing(std::string path, std::string reason) {
        const auto next_generation = snapshot_.generation + 1;
        snapshot_ = {
            .status = SampleResourceStatus::Missing,
            .generation = next_generation,
        };
        diagnostics_ = {
            .status = SampleResourceStatus::Missing,
            .generation = next_generation,
            .path = std::move(path),
            .reason = std::move(reason),
        };
    }

    bool publish_loaded(AudioFileData data,
                        std::string path,
                        uint64_t memory_budget_bytes = 0) {
        const auto byte_size = decoded_byte_size(data);
        const auto next_generation = snapshot_.generation + 1;
        if (memory_budget_bytes != 0 && byte_size > memory_budget_bytes) {
            snapshot_ = {
                .status = SampleResourceStatus::Oversized,
                .generation = next_generation,
                .frame_count = data.num_frames(),
                .channel_count = data.num_channels(),
                .sample_rate = data.sample_rate,
                .byte_size = byte_size,
                .memory_budget_bytes = memory_budget_bytes,
            };
            diagnostics_ = {
                .status = SampleResourceStatus::Oversized,
                .generation = next_generation,
                .path = std::move(path),
                .reason = "decoded sample exceeds memory budget",
                .frame_count = data.num_frames(),
                .channel_count = data.num_channels(),
                .sample_rate = data.sample_rate,
                .byte_size = byte_size,
                .memory_budget_bytes = memory_budget_bytes,
            };
            return false;
        }

        auto shared = std::make_shared<AudioFileData>(std::move(data));
        snapshot_ = {
            .status = SampleResourceStatus::Loaded,
            .generation = next_generation,
            .frame_count = shared->num_frames(),
            .channel_count = shared->num_channels(),
            .sample_rate = shared->sample_rate,
            .byte_size = byte_size,
            .memory_budget_bytes = memory_budget_bytes,
            .data = std::move(shared),
        };
        diagnostics_ = {
            .status = SampleResourceStatus::Loaded,
            .generation = next_generation,
            .path = std::move(path),
            .frame_count = snapshot_.frame_count,
            .channel_count = snapshot_.channel_count,
            .sample_rate = snapshot_.sample_rate,
            .byte_size = snapshot_.byte_size,
            .memory_budget_bytes = memory_budget_bytes,
        };
        return true;
    }

    static uint64_t decoded_byte_size(const AudioFileData& data) noexcept {
        uint64_t total = 0;
        for (const auto& channel : data.channels) {
            total += static_cast<uint64_t>(channel.size() * sizeof(float));
        }
        return total;
    }

private:
    SampleResourceSnapshot snapshot_{};
    SampleResourceDiagnostics diagnostics_{};
};

} // namespace pulp::audio
