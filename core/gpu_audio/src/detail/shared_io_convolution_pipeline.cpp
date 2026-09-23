#include "shared_io_convolution_pipeline.hpp"

namespace pulp::gpu_audio::detail {

bool SharedIoConvolutionPipeline::prepare(Config config, std::uint64_t epoch,
                                          std::uint64_t first_sequence) {
    if (prepared_ || config.capacity == 0 || config.channels == 0 || config.block_size == 0 ||
        config.ir_length == 0 || config.lead_blocks == 0 || config.capacity <= config.lead_blocks ||
        std::uint64_t(config.block_size) + config.ir_length - 1u > config.fft_size)
        return false;
    if (!bridge_.prepare({config.capacity, config.channels, config.block_size, config.lead_blocks,
                          config.capture_callback_timing},
                         epoch, first_sequence))
        return false;
    if (!executor_.prepare({config.capacity, config.channels, config.block_size, config.fft_size,
                            config.ir_length},
                           epoch, first_sequence))
        return false;
    epoch_ = epoch;
    prepared_ = true;
    return true;
}

SharedIoConvolutionPipeline::Callback
SharedIoConvolutionPipeline::begin_callback(std::span<const float> samples) noexcept {
    return bridge_.begin_callback(samples);
}

SharedIoConvolutionPipeline::Delivery
SharedIoConvolutionPipeline::consume_output(const Callback& callback, std::span<float> output,
                                            bool defer_delivery) noexcept {
    bool finalized = false;
    const auto result = bridge_.consume_output(callback, output, &finalized, defer_delivery);
    // This is the callback's sole interaction with the executor: publishing its
    // atomic watermark. Callback code never reads or mutates terminal, OLA, or
    // ready storage. Sequence q consumes wet q - lead; once that callback has
    // completed, a later terminal for the same record may advance worker-owned
    // OLA but must never become an audible late wet block.
    if (finalized && callback.stamp.sequence >= bridge_.lead_blocks())
        executor_.advance_callback_watermark(callback.stamp.sequence - bridge_.lead_blocks());
    return result;
}

std::optional<SharedIoConvolutionPipeline::Lease>
SharedIoConvolutionPipeline::acquire_input() noexcept {
    return bridge_.acquire_input();
}

bool SharedIoConvolutionPipeline::release_input(const Lease& lease) noexcept {
    return bridge_.release_input(lease);
}

bool SharedIoConvolutionPipeline::record_terminal(Stamp stamp, Terminal terminal,
                                                  std::span<const float> interleaved_time,
                                                  Terminal* accepted_terminal) noexcept {
    return prepared_ && stamp.epoch == epoch_ &&
           executor_.record_terminal(stamp.epoch, stamp.sequence, terminal, interleaved_time,
                                     accepted_terminal);
}

std::size_t SharedIoConvolutionPipeline::drain_terminals() noexcept {
    if (!prepared_)
        return 0;
    const auto first = executor_.next_sequence();
    const auto count = executor_.collect();
    for (std::size_t offset = 0; offset < count; ++offset) {
        const auto sequence = first + offset;
        const auto ready = executor_.take_ready(sequence);
        if (ready.empty())
            continue;
        // Published and dropped-full are both final fixed-record dispositions.
        // A full egress ring is consumed as the existing CPU fallback rather
        // than waiting or overwriting a callback-owned record.
        bridge_.publish_output({epoch_, sequence}, ready);
        executor_.release_ready(sequence);
    }
    if (executor_.fenced())
        bridge_.request_recovery(SharedIoRecoveryReason::ProviderFailure);
    return count;
}

bool SharedIoConvolutionPipeline::fence_and_reprime(std::uint64_t new_epoch) noexcept {
    if (!prepared_ || new_epoch <= epoch_ ||
        bridge_.next_sequence() >= SharedIoStampedBridge::kSequenceLimit)
        return false;
    bridge_.suspend_delivery();
    if (!executor_.fence_and_reprime(new_epoch, bridge_.next_sequence()))
        return false;
    if (!bridge_.activate_epoch(new_epoch))
        return false;
    epoch_ = new_epoch;
    return true;
}

void SharedIoConvolutionPipeline::suspend_delivery() noexcept {
    bridge_.suspend_delivery();
}

} // namespace pulp::gpu_audio::detail
