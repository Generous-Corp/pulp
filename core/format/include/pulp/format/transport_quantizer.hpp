#pragma once

#include <pulp/format/processor.hpp>

#include <cstdint>

namespace pulp::format {

enum class TransportQuantizePolicy : std::uint8_t {
    Immediate,
    NextBeat,
    NextBar,
    NextGrid,
    HostLoopStart,
};

enum class TransportQuantizeStatus : std::uint8_t {
    Scheduled,
    OutsideBlock,
    TransportStopped,
    InvalidBlockSize,
    InvalidSampleRate,
    InvalidTempo,
    InvalidTimeSignature,
    InvalidGrid,
    InvalidTimeline,
    LoopUnavailable,
    TransportJumped,
};

struct TransportQuantizeRequest {
    TransportQuantizePolicy policy = TransportQuantizePolicy::Immediate;
    double grid_beats = 1.0;
    bool require_playing = true;
    bool cancel_on_transport_jump = true;
};

struct TransportQuantizerBlock {
    bool transport_jumped = false;
    bool timeline_valid = false;
};

struct TransportQuantizeResult {
    bool scheduled = false;
    // May equal ProcessContext::num_samples to represent the boundary just
    // after the current block. It is not always a sample index.
    std::uint32_t block_offset = 0;
    double target_beats = 0.0;
    bool transport_jumped = false;
    TransportQuantizeStatus status = TransportQuantizeStatus::OutsideBlock;
};

class TransportQuantizer {
public:
    class BlockPlan {
    public:
        TransportQuantizeResult resolve(
            const TransportQuantizeRequest& request) const noexcept;

        const ProcessContext& context() const noexcept { return context_; }
        TransportQuantizerBlock block() const noexcept { return block_; }
        bool transport_jumped() const noexcept { return block_.transport_jumped; }
        bool timeline_valid() const noexcept { return block_.timeline_valid; }

    private:
        friend class TransportQuantizer;

        BlockPlan(const TransportQuantizer& owner,
                  const ProcessContext& context,
                  TransportQuantizerBlock block) noexcept;

        const TransportQuantizer& owner_;
        ProcessContext context_;
        TransportQuantizerBlock block_;
    };

    TransportQuantizer() = default;

    void reset() noexcept;
    TransportQuantizerBlock begin_block(const ProcessContext& context) noexcept;

    // Preferred call shape for new users: captures the exact ProcessContext
    // and block metadata together so resolves cannot accidentally pair a stale
    // TransportQuantizerBlock with a different host block.
    BlockPlan begin_block_plan(const ProcessContext& context) noexcept;

    TransportQuantizeResult resolve(const ProcessContext& context,
                                    const TransportQuantizeRequest& request,
                                    TransportQuantizerBlock block) const noexcept;

    static double beats_per_bar(int numerator, int denominator) noexcept;

private:
    struct LastBlock {
        bool valid = false;
        bool is_playing = false;
        bool is_looping = false;
        double sample_rate = 0.0;
        double tempo_bpm = 0.0;
        double position_beats = 0.0;
        double loop_start_beats = 0.0;
        double loop_end_beats = 0.0;
        int num_samples = 0;
    };

    static bool valid_sample_rate(double sample_rate) noexcept;
    static bool valid_tempo(double tempo_bpm) noexcept;
    static bool valid_grid(double grid_beats) noexcept;
    static bool valid_timeline(const ProcessContext& context) noexcept;
    static bool valid_loop_range(double start_beats, double end_beats) noexcept;
    static bool valid_loop(const ProcessContext& context) noexcept;
    static double frames_to_beats(double frames,
                                  double sample_rate,
                                  double tempo_bpm) noexcept;
    static double beats_to_frames(double beats,
                                  double sample_rate,
                                  double tempo_bpm) noexcept;
    static double next_grid_boundary(double position_beats,
                                     double grid_beats) noexcept;
    static double next_host_loop_start_boundary(const ProcessContext& context) noexcept;
    static double wrap_loop_position(double position_beats,
                                     double loop_start_beats,
                                     double loop_end_beats) noexcept;
    static double timeline_tolerance_beats(const ProcessContext& context) noexcept;

    TransportQuantizeResult offset_for_target(const ProcessContext& context,
                                              double target_beats,
                                              bool transport_jumped) const noexcept;
    bool detect_transport_jump(const ProcessContext& context,
                               bool timeline_valid) const noexcept;
    void remember(const ProcessContext& context, bool timeline_valid) noexcept;

    LastBlock last_;
};

}  // namespace pulp::format
