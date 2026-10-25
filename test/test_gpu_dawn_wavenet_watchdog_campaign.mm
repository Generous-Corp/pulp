// Real-Dawn campaign for the WaveNet node's ProviderUnresponsive watchdog.
//
// measure (default): the production GpuWaveNetSession behind GpuAudioTransport's
//   own worker, paced at the real block period by a time-constraint callback
//   thread. The node's watchdog is disabled (a window no run can reach) and its
//   ring is far larger than any stall, so stalls are measured, not latched. The
//   channel evaluates the node's own watchdog quantity at every pump that
//   collects nothing while a block is in flight:
//     silent = position - max(position at the last completion, oldest + 1)
//   and the matching wall time since the callback that reached that position.
//   Each accepted block reports the longest silence seen while it was in
//   flight; a window of W blocks would have tripped on every block whose
//   silence reached W.
// --hang: the same pump-side measurement over a physical provider whose
//   completions never arrive (DelayCompletion): the time each candidate window
//   takes to detect a true hang.
// --ordering: the real node over that hung provider at several capacities and
//   windows, reporting which cause latches first and when.
//
// One JSON line per cell on stdout.

#include "detail/dawn_shared_io_provider.hpp"
#include "detail/dawn_shared_io_wavenet_program.hpp"
#include "detail/shared_io_program_session.hpp"
#include "detail/shared_io_wavenet_spec.hpp"
#include "detail/wavenet_realtime_channel.hpp"
#include "support/wavenet_program_session_channel.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/gpu_audio/gpu_audio_program.hpp>
#include <pulp/gpu_audio/gpu_audio_transport.hpp>
#include <pulp/gpu_audio/gpu_wavenet.hpp>
#include <pulp/gpu_audio/gpu_wavenet_realtime_node.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <memory>
#include <optional>
#include <pthread.h>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef PULP_GPU_AUDIO_EXPECTED_DAWN_SHA
#define PULP_GPU_AUDIO_EXPECTED_DAWN_SHA ""
#endif

namespace {

namespace ga = pulp::gpu_audio;
using ga::detail::SharedIoSubmissionTicket;
using ga::detail::WaveNetChannelCompletion;
using ga::detail::WaveNetRealtimeChannel;

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kNever = 1u << 30;
constexpr std::size_t kReach = 513; // silence thresholds 0..512 blocks

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

void sleep_until_ns(std::uint64_t deadline) {
    const auto now = now_ns();
    if (deadline > now)
        std::this_thread::sleep_for(std::chrono::nanoseconds(deadline - now));
}

// The callback thread gets the time-constraint policy CoreAudio gives an IO
// thread: period one block, computation a quarter of it.
bool make_realtime(double period_ns) {
    mach_timebase_info_data_t info{};
    mach_timebase_info(&info);
    const auto to_abs = [&](double ns) {
        return static_cast<std::uint32_t>(ns * info.denom / info.numer);
    };
    thread_time_constraint_policy_data_t policy{};
    policy.period = to_abs(period_ns);
    policy.computation = to_abs(period_ns / 4.0);
    policy.constraint = to_abs(period_ns);
    policy.preemptible = 1;
    return thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                             reinterpret_cast<thread_policy_t>(&policy),
                             THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
}

// Preallocated; each field has one writer.
struct Timeline {
    std::atomic<std::uint64_t> position{0};   // callback: finished callbacks
    std::atomic<std::uint64_t> next_admit{0}; // worker: one past the last admitted block
    std::uint64_t start_ns = 0;
    std::array<std::uint64_t, 129> backlog_hist{}; // callback; 0..127, 128+
    std::vector<std::uint64_t> callback_end;       // callback
    std::vector<std::uint64_t> submitted;          // worker
    std::vector<std::uint64_t> received;           // worker
    std::vector<std::uint32_t> silent_blocks;      // worker, per accepted block
    std::vector<std::uint64_t> silent_ns;          // worker, per accepted block
    std::vector<std::uint64_t> gpu_ns;             // worker, staged timestamps only
    // Worker: when the live episode's silence first reached k blocks.
    std::array<std::uint64_t, kReach> reach_ns{};
    explicit Timeline(std::size_t n)
        : callback_end(n, 0), submitted(n, 0), received(n, 0), silent_blocks(n, 0), silent_ns(n, 0),
          gpu_ns(n, 0) {}
    std::uint64_t time_at_position(std::uint64_t position) const {
        return position == 0 ? start_ns : callback_end[position - 1];
    }
};

class TimingChannel final : public WaveNetRealtimeChannel {
  public:
    TimingChannel(std::unique_ptr<WaveNetRealtimeChannel> inner, Timeline& timeline)
        : inner_(std::move(inner)), timeline_(timeline) {}
    bool submit(std::span<const float> input,
                const SharedIoSubmissionTicket& ticket) noexcept override {
        const bool ok = inner_->submit(input, ticket);
        if (!ok)
            return ok;
        if (ticket.sequence < timeline_.submitted.size())
            timeline_.submitted[ticket.sequence] = now_ns();
        timeline_.next_admit.store(ticket.sequence + 1, std::memory_order_release);
        if (count_ == 0) {
            episode_blocks_ = 0;
            episode_ns_ = 0;
            timeline_.reach_ns.fill(0);
        }
        if (count_ < flight_.size())
            flight_[(head_ + count_++) % flight_.size()] = ticket.sequence;
        return ok;
    }
    void service(std::uint64_t now) noexcept override {
        inner_->service(now);
    }
    void service_until(std::uint64_t now, std::uint64_t deadline) noexcept override {
        inner_->service_until(now, deadline);
    }
    std::optional<WaveNetChannelCompletion> receive(std::span<float> output) noexcept override {
        auto completion = inner_->receive(output);
        const auto position = timeline_.position.load(std::memory_order_acquire);
        if (!completion) {
            // The node calls receive only while a block is pending, so an
            // empty receive is a pump without progress: the watchdog's
            // evaluation point.
            if (count_ != 0) {
                const auto since = std::max(mark_, flight_[head_] + 1);
                const auto silent = position > since ? position - since : 0;
                const auto now = now_ns();
                const auto wall = position > since ? now - timeline_.time_at_position(since) : 0;
                episode_blocks_ = std::max(episode_blocks_, silent);
                episode_ns_ = std::max(episode_ns_, wall);
                for (std::size_t k = 0; k <= std::min<std::uint64_t>(silent, kReach - 1); ++k)
                    if (timeline_.reach_ns[k] == 0)
                        timeline_.reach_ns[k] = now;
            }
            return completion;
        }
        mark_ = position;
        // Completions arrive in order: the oldest in flight completed, and the
        // next oldest starts its own silence from this completion.
        if (count_ != 0) {
            const auto oldest = flight_[head_];
            if (oldest < timeline_.silent_blocks.size()) {
                timeline_.silent_blocks[oldest] = static_cast<std::uint32_t>(episode_blocks_);
                timeline_.silent_ns[oldest] = episode_ns_;
            }
            head_ = (head_ + 1) % flight_.size();
            --count_;
            episode_blocks_ = 0;
            episode_ns_ = 0;
        }
        if (completion->result.sequence < timeline_.received.size())
            timeline_.received[completion->result.sequence] = now_ns();
        return completion;
    }
    bool release() noexcept override {
        return inner_->release();
    }
    bool lost() const noexcept override {
        return inner_->lost();
    }

  private:
    std::unique_ptr<WaveNetRealtimeChannel> inner_;
    Timeline& timeline_;
    // Worker-only watchdog mirror: the blocks in flight, oldest first.
    std::array<std::uint64_t, 8> flight_{};
    std::size_t head_ = 0, count_ = 0;
    std::uint64_t mark_ = 0, episode_blocks_ = 0, episode_ns_ = 0;
};

struct Model {
    std::string name;
    std::vector<std::vector<std::uint32_t>> dilations;
    std::vector<ga::GpuWaveNetLayerDescriptor> layers;
    std::vector<float> weights;
};

// Weight count as validate_wavenet_spec() requires it (ungated, condition 1).
std::size_t weight_count(const std::vector<ga::GpuWaveNetLayerDescriptor>& layers) {
    std::size_t required = 0;
    for (const auto& layer : layers) {
        const std::size_t z = layer.gated ? 2u * layer.channels : layer.channels;
        required += static_cast<std::size_t>(layer.channels) * layer.input_size;
        for (std::size_t d = 0; d < layer.dilations.size(); ++d)
            required += z * layer.channels * layer.kernel + z + z * layer.condition_size +
                        static_cast<std::size_t>(layer.channels) * layer.channels + layer.channels;
        required += static_cast<std::size_t>(layer.head_size) * layer.channels;
        if (layer.head_bias)
            required += layer.head_size;
    }
    return required + 1; // trailing head_scale
}

Model make_model(std::string_view name) {
    Model model;
    model.name = std::string(name);
    if (name == "tiny") {
        model.dilations = {{1}};
        model.layers = {{.input_size = 1,
                         .condition_size = 1,
                         .channels = 1,
                         .kernel = 2,
                         .head_size = 1,
                         .tanh_activation = true}};
    } else {
        // NAM "standard": two arrays of ten dilated layers, 16 then 8 channels.
        const std::vector<std::uint32_t> d{1, 2, 4, 8, 16, 32, 64, 128, 256, 512};
        model.dilations = {d, d};
        model.layers = {{.input_size = 1,
                         .condition_size = 1,
                         .channels = 16,
                         .kernel = 3,
                         .head_size = 8,
                         .tanh_activation = true},
                        {.input_size = 16,
                         .condition_size = 1,
                         .channels = 8,
                         .kernel = 3,
                         .head_size = 1,
                         .head_bias = true,
                         .tanh_activation = true}};
    }
    for (std::size_t i = 0; i < model.layers.size(); ++i)
        model.layers[i].dilations = model.dilations[i];
    model.weights.resize(weight_count(model.layers));
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> dist(-0.05f, 0.05f);
    for (auto& w : model.weights)
        w = dist(rng);
    model.weights.back() = 1.0f; // head_scale
    return model;
}

// The node's channel seam over a physical provider driven directly, so a run
// can inject a fault or read per-block GPU timestamps (which Dawn supports
// only with staged storage). Mirrors pulp::test::ProgramSessionChannel and
// also records each completion's GPU busy time.
class RawProviderChannel final : public WaveNetRealtimeChannel {
  public:
    using Session = ga::detail::SharedIoProgramSession;
    RawProviderChannel(std::unique_ptr<Session> session, Timeline* timeline)
        : session_(std::move(session)), timeline_(timeline) {}
    bool submit(std::span<const float> input,
                const SharedIoSubmissionTicket& ticket) noexcept override {
        auto lease = session_->acquire_input(ticket.sequence, 0);
        if (!lease || lease->bytes.size() != input.size_bytes())
            return false;
        std::copy(input.begin(), input.end(), reinterpret_cast<float*>(lease->bytes.data()));
        const auto token = Session::SubmitToken::admit(lease->token);
        if (session_->submit(token)) {
            tickets_[ticket.sequence % tickets_.size()] = ticket;
            return true;
        }
        (void)session_->cancel(token);
        return false;
    }
    void service(std::uint64_t now) noexcept override {
        session_->service(now);
    }
    void service_until(std::uint64_t now, std::uint64_t deadline) noexcept override {
        session_->service_until(now, deadline);
    }
    std::optional<WaveNetChannelCompletion> receive(std::span<float> output) noexcept override {
        const auto completion = session_->pop_completion();
        if (!completion)
            return std::nullopt;
        const auto sequence = completion->token.slot.stream_sequence;
        ga::GpuWaveNetBlockResult result{.sequence = sequence,
                                         .status = ga::GpuWaveNetBlockStatus::ProviderFailed,
                                         .late = completion->late};
        if (timeline_ != nullptr && completion->gpu_elapsed_available &&
            sequence < timeline_->gpu_ns.size())
            timeline_->gpu_ns[sequence] = completion->gpu_elapsed_ns;
        if (completion->status == ga::detail::SharedIoArena::CompletionStatus::RetiredSuccess) {
            if (auto lease = session_->acquire_output(*completion)) {
                if (lease->bytes.size() >= output.size_bytes())
                    std::copy_n(reinterpret_cast<const float*>(lease->bytes.data()), output.size(),
                                output.begin());
                if (session_->release_output({lease->token}) &&
                    lease->bytes.size() >= output.size_bytes())
                    result.status = ga::GpuWaveNetBlockStatus::GpuDelivered;
            }
        } else {
            (void)session_->discard_completion(*completion);
        }
        WaveNetChannelCompletion out{result, {}};
        auto& entry = tickets_[sequence % tickets_.size()];
        if (entry.accepted() && entry.sequence == sequence) {
            out.ticket = entry;
            entry = {};
        }
        return out;
    }
    bool release() noexcept override {
        return session_->release();
    }
    bool lost() const noexcept override {
        const auto* provider = session_->owned_provider();
        return provider != nullptr && provider->device_lost();
    }

  private:
    std::unique_ptr<Session> session_;
    Timeline* timeline_;
    std::array<SharedIoSubmissionTicket, 64> tickets_{};
};

// A physical provider behind the node's channel seam: hung (DelayCompletion)
// or healthy with staged GPU timestamps.
std::unique_ptr<WaveNetRealtimeChannel> make_provider_channel(std::uint32_t frames,
                                                              const Model& model, bool hang,
                                                              std::uint32_t slots,
                                                              Timeline* timeline) {
    using namespace ga::detail;
    std::vector<WavenetLayerSpec> arrays;
    for (const auto& layer : model.layers)
        arrays.push_back({.input_size = layer.input_size,
                          .condition_size = layer.condition_size,
                          .channels = layer.channels,
                          .kernel = layer.kernel,
                          .head_size = layer.head_size,
                          .gated = layer.gated ? 1u : 0u,
                          .head_bias = layer.head_bias ? 1u : 0u,
                          .dilations = layer.dilations});
    const WavenetProgramSpec spec{.block_size = frames,
                                  .head_scale = model.weights.back(),
                                  .stream_instances = 1,
                                  .arrays = arrays,
                                  .weights = model.weights};
    const auto storage = hang ? SharedIoArenaProvider::StorageKind::ImportedHostPointer
                              : SharedIoArenaProvider::StorageKind::Staged;
    auto created =
        DawnSharedIoProvider::create({.expected_dawn_revision = PULP_GPU_AUDIO_EXPECTED_DAWN_SHA,
                                      .fault = hang ? DawnSharedIoProvider::Fault::DelayCompletion
                                                    : DawnSharedIoProvider::Fault::None,
                                      .storage_kind = storage,
                                      .enable_timestamps = !hang});
    if (!created.provider)
        return nullptr;
    auto program = DawnSharedIoWavenetProgram::create(*created.provider, spec);
    const auto bytes = frames * sizeof(float);
    auto session = std::make_unique<SharedIoProgramSession>();
    if (!program || !session->prepare({std::move(created.provider), std::move(program)},
                                      {.slots = slots,
                                       .input_bytes_per_slot = bytes,
                                       .output_bytes_per_slot = bytes,
                                       .storage_kind = storage}))
        return nullptr;
    return std::make_unique<RawProviderChannel>(std::move(session), timeline);
}

struct Cell {
    std::uint32_t frames = 64;
    std::uint32_t lead = 2;
    bool wake = true;
    std::string model = "standard";
    double seconds = 600.0;
    std::string mode = "measure";
    std::uint32_t capacity = 512;
    std::uint32_t watchdog = kNever;
    std::uint32_t inflight = 1;
    bool staged_timestamps = false;
};

double percentile(std::vector<double> v, double p) {
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    const auto index = static_cast<std::size_t>(std::ceil(p * static_cast<double>(v.size())));
    return v[std::min(index == 0 ? 0 : index - 1, v.size() - 1)];
}

std::string stats(const std::vector<double>& v) {
    std::ostringstream out;
    out << R"({"p50":)" << percentile(v, 0.5) << R"(,"p99":)" << percentile(v, 0.99)
        << R"(,"p999":)" << percentile(v, 0.999) << R"(,"max":)" << percentile(v, 1.0) << "}";
    return out.str();
}

const char* recovery_name(ga::GpuAudioRecoveryReason reason) {
    using R = ga::GpuAudioRecoveryReason;
    switch (reason) {
    case R::None:
        return "none";
    case R::InputSaturated:
        return "input_saturated";
    case R::ProviderUnresponsive:
        return "provider_unresponsive";
    case R::ProviderFailure:
        return "provider_failure";
    case R::ProviderLost:
        return "provider_lost";
    default:
        return "other";
    }
}

std::string run_cell(const Cell& cell, const std::string& tag) {
    std::ostringstream out;
    const double period_ns = 1.0e9 * cell.frames / kSampleRate;
    out << R"({"schema":"pulp.gpu-audio.wavenet-watchdog-campaign.v2","tag":")" << tag
        << R"(","mode":")" << cell.mode << R"(","frames":)" << cell.frames << R"(,"block_ms":)"
        << period_ns / 1.0e6 << R"(,"lead":)" << cell.lead << R"(,"worker":")"
        << (cell.wake ? "wake_on_write" : "poll") << R"(","model":")" << cell.model << "\"";
    double load[3]{};
    if (getloadavg(load, 3) > 0)
        out << R"(,"host_load_1m_start":)" << load[0];
    const bool hung = cell.mode != "measure";
    const auto model = make_model(hung ? "tiny" : cell.model);
    const ga::GpuWaveNetDescriptor descriptor{.block_size = cell.frames,
                                              .sample_rate = kSampleRate,
                                              .stream_instances = 1,
                                              .layers = model.layers,
                                              .weight_count = model.weights.size()};
    const ga::GpuWaveNetSession::Config session_config{
        .descriptor = descriptor, .weights = model.weights, .slots = 2};
    std::unique_ptr<WaveNetRealtimeChannel> inner;
    if (hung) {
        inner = make_provider_channel(cell.frames, model, true, 2, nullptr);
    } else if (auto created = ga::GpuWaveNetSession::create(session_config)) {
        inner =
            std::make_unique<ga::detail::GpuWaveNetSessionChannel<>>(std::move(created.session), 2);
    }
    if (!inner) {
        out << R"(,"error":"provider_unavailable"})";
        return out.str();
    }
    const auto total = static_cast<std::size_t>(cell.seconds * 1.0e9 / period_ns);
    Timeline timeline(total + 64);
    ga::GpuWaveNetRealtimeNode node({.session = session_config,
                                     .channels = 1,
                                     .lead_blocks = cell.lead,
                                     .capacity = cell.capacity,
                                     .max_inflight = 1,
                                     .watchdog_blocks = cell.watchdog});
    std::vector<std::unique_ptr<WaveNetRealtimeChannel>> channels;
    channels.push_back(std::make_unique<TimingChannel>(std::move(inner), timeline));
    if (!ga::detail::WaveNetRealtimeTestAccess::prepare(node, std::move(channels))) {
        out << R"(,"error":"node_prepare_failed"})";
        return out.str();
    }
    ga::GpuAudioTransport transport;
    if (!transport.prepare(&node, {.ring_blocks = std::max(4u, cell.lead + 2),
                                   .run_worker_thread = true,
                                   .wake_on_write = cell.wake})) {
        out << R"(,"error":"transport_prepare_failed"})";
        (void)node.release();
        return out.str();
    }

    bool realtime = false;
    std::uint64_t overruns = 0, ran = 0, latched_at = 0;
    ga::GpuAudioRecoveryReason first_reason = ga::GpuAudioRecoveryReason::None;
    std::thread callback([&] {
        realtime = make_realtime(period_ns);
        std::vector<float> in(cell.frames), out_samples(cell.frames);
        const float* in_ptr = in.data();
        float* out_ptr = out_samples.data();
        pulp::audio::BufferView<const float> input(&in_ptr, 1, cell.frames);
        pulp::audio::BufferView<float> output(&out_ptr, 1, cell.frames);
        timeline.start_ns = now_ns() + 10'000'000;
        for (std::size_t block = 0; block < total; ++block) {
            const auto due = timeline.start_ns + static_cast<std::uint64_t>(block * period_ns);
            if (now_ns() > due + static_cast<std::uint64_t>(period_ns))
                ++overruns;
            sleep_until_ns(due);
            for (std::uint32_t i = 0; i < cell.frames; ++i)
                in[i] = 0.1f * std::sin(0.01f * static_cast<float>(block * cell.frames + i));
            transport.process(input, output, cell.frames);
            timeline.callback_end[block] = now_ns();
            timeline.position.store(block + 1, std::memory_order_release);
            ran = block + 1;
            const auto admitted = timeline.next_admit.load(std::memory_order_acquire);
            const auto backlog = block + 1 > admitted ? block + 1 - admitted : 0;
            ++timeline.backlog_hist[std::min<std::uint64_t>(backlog, 128)];
            if (cell.mode == "ordering") {
                // The worker classifies a saturation at its next pump, so the
                // stream runs on briefly before the settled cause is read.
                const auto reason = node.recovery_reason();
                if (reason != ga::GpuAudioRecoveryReason::None && latched_at == 0) {
                    first_reason = reason;
                    latched_at = block + 1;
                }
                if (latched_at != 0 && block + 1 >= latched_at + 16)
                    break;
            } else if (cell.mode == "hang" && backlog >= 512) {
                break;
            }
        }
    });
    callback.join();
    const auto status = node.status_snapshot();
    transport.release();
    (void)node.release();
    if (getloadavg(load, 3) > 0)
        out << R"(,"host_load_1m_end":)" << load[0];
    out << R"(,"blocks":)" << ran << R"(,"seconds":)"
        << static_cast<double>(ran) * period_ns / 1.0e9 << R"(,"realtime_thread":)"
        << (realtime ? "true" : "false") << R"(,"callback_overruns":)" << overruns;

    if (cell.mode == "ordering") {
        // Blocks from the hung block's admission to the latch.
        const auto admitted_at = timeline.submitted[0];
        out << R"(,"capacity":)" << cell.capacity << R"(,"watchdog_blocks":)" << cell.watchdog
            << R"(,"latched":")" << recovery_name(first_reason) << R"(","settled":")"
            << recovery_name(status.recovery_reason) << R"(","latched_after_callbacks":)"
            << latched_at << R"(,"latched_after_ms":)"
            << (latched_at && admitted_at
                    ? static_cast<double>(timeline.callback_end[latched_at - 1] - admitted_at) /
                          1.0e6
                    : -1.0)
            << R"(,"status_reason":")" << recovery_name(status.recovery_reason)
            << R"(","provider_state":)" << static_cast<int>(status.provider_state) << "}";
        return out.str();
    }
    if (cell.mode == "hang") {
        // Detection latency of a window of W blocks: from the hung block's
        // admission to the first pump whose silence reached W.
        const auto admitted_at = timeline.submitted[0];
        out << R"(,"detect_ms_at_blocks":{)";
        bool first = true;
        for (const std::uint32_t w : {1u, 3u, 8u, 16u, 32u, 64u, 128u}) {
            const auto t = w < kReach ? timeline.reach_ns[w] : 0;
            out << (first ? "" : ",") << "\"" << w << "\":"
                << (t && admitted_at ? static_cast<double>(t - admitted_at) / 1.0e6 : -1.0);
            first = false;
        }
        out << R"(},"detect_ms_at_ms":{)";
        first = true;
        for (const double ms : {10.0, 20.0, 50.0, 100.0}) {
            const auto w = static_cast<std::size_t>(std::ceil(ms * 1.0e6 / period_ns));
            const auto t = w < kReach ? timeline.reach_ns[w] : 0;
            out << (first ? "" : ",") << "\"" << ms << "\":"
                << (t && admitted_at ? static_cast<double>(t - admitted_at) / 1.0e6 : -1.0);
            first = false;
        }
        out << "}}";
        return out.str();
    }

    std::vector<double> blocks, ms, latency;
    std::uint64_t accepted = 0;
    for (std::size_t s = 0; s < ran; ++s) {
        if (timeline.received[s] == 0 || timeline.submitted[s] == 0)
            continue;
        ++accepted;
        blocks.push_back(timeline.silent_blocks[s]);
        ms.push_back(static_cast<double>(timeline.silent_ns[s]) / 1.0e6);
        latency.push_back(static_cast<double>(timeline.received[s] - timeline.submitted[s]) /
                          1.0e6);
    }
    out << R"(,"accepted_blocks":)" << accepted << R"(,"missed_blocks":)" << status.missed_blocks
        << R"(,"recovery_reason":")" << recovery_name(status.recovery_reason)
        << R"(","recovery_sequence":)"
        << (status.recovery_sequence == UINT64_MAX
                ? -1LL
                : static_cast<long long>(status.recovery_sequence))
        << R"(,"watchdog_blocks":)" << cell.watchdog << R"(,"silent_blocks":)" << stats(blocks)
        << R"(,"silent_ms":)" << stats(ms) << R"(,"submit_to_collect_ms":)" << stats(latency);
    // False trips: accepted blocks whose silence reached the window.
    out << R"(,"trips_at_blocks":{)";
    bool first = true;
    for (const std::uint32_t w : {1u, 3u, 8u, 16u, 32u}) {
        const auto n =
            std::count_if(blocks.begin(), blocks.end(), [&](double b) { return b >= w; });
        out << (first ? "" : ",") << "\"" << w << "\":" << n;
        first = false;
    }
    out << R"(},"trips_at_ms":{)";
    first = true;
    for (const double t : {10.0, 20.0, 50.0, 100.0}) {
        // A time window is enforced as ceil(T / block) callback blocks.
        const auto w = std::ceil(t * 1.0e6 / period_ns);
        const auto n =
            std::count_if(blocks.begin(), blocks.end(), [&](double b) { return b >= w; });
        out << (first ? "" : ",") << "\"" << t << "\":" << n;
        first = false;
    }
    // Callbacks at which a ring of C blocks would have been full.
    out << R"(},"backlog_at_least":{)";
    first = true;
    for (const std::uint32_t c : {4u, 6u, 8u, 16u, 32u, 64u, 128u}) {
        std::uint64_t over = 0;
        for (std::size_t i = c; i < timeline.backlog_hist.size(); ++i)
            over += timeline.backlog_hist[i];
        out << (first ? "" : ",") << "\"" << c << "\":" << over;
        first = false;
    }
    out << "}}";
    return out.str();
}

// measure mode. A latch of any cause fences the node, so the run is split
// into segments: when one latches, its settled cause is recorded and a fresh
// session, node and transport continue the stream until the cell's time is
// spent. With the default watchdog (--watchdog=0) the latches it reports are
// the false trips of that window on a host that never hangs.
std::string run_measure(const Cell& cell, const std::string& tag) {
    std::ostringstream out;
    const double period_ns = 1.0e9 * cell.frames / kSampleRate;
    out << R"({"schema":"pulp.gpu-audio.wavenet-watchdog-campaign.v3","tag":")" << tag
        << R"(","mode":"measure","frames":)" << cell.frames << R"(,"block_ms":)"
        << period_ns / 1.0e6 << R"(,"lead":)" << cell.lead << R"(,"worker":")"
        << (cell.wake ? "wake_on_write" : "poll") << R"(","model":")" << cell.model
        << R"(","capacity":)" << cell.capacity << R"(,"watchdog_blocks":)" << cell.watchdog
        << R"(,"max_inflight":)" << cell.inflight << R"(,"storage":")"
        << (cell.staged_timestamps ? "staged_timestamps" : "imported_host_pointer") << "\"";
    double load[3]{};
    if (getloadavg(load, 3) > 0)
        out << R"(,"host_load_1m_start":)" << load[0];
    const auto model = make_model(cell.model);
    const ga::GpuWaveNetDescriptor descriptor{.block_size = cell.frames,
                                              .sample_rate = kSampleRate,
                                              .stream_instances = 1,
                                              .layers = model.layers,
                                              .weight_count = model.weights.size()};
    const auto slots = std::max(2u, cell.inflight);
    const ga::GpuWaveNetSession::Config session_config{
        .descriptor = descriptor, .weights = model.weights, .slots = slots};
    const auto total = static_cast<std::size_t>(cell.seconds * 1.0e9 / period_ns);
    std::vector<double> blocks, ms, latency, admit, gpu, host;
    std::uint64_t gpu_delivered = 0;
    std::array<std::uint64_t, 129> backlog_hist{};
    std::uint64_t accepted = 0, missed = 0, overruns = 0, ran_total = 0, segments = 0;
    std::uint64_t unresponsive = 0, saturated = 0, other = 0;
    bool realtime = true;
    std::string error;
    while (ran_total + 64 < total && segments < 400) {
        ++segments;
        const auto budget = total - ran_total;
        Timeline timeline(budget + 64);
        std::unique_ptr<WaveNetRealtimeChannel> inner;
        if (cell.staged_timestamps) {
            inner = make_provider_channel(cell.frames, model, false, slots, &timeline);
        } else if (auto created = ga::GpuWaveNetSession::create(session_config)) {
            inner = std::make_unique<ga::detail::GpuWaveNetSessionChannel<>>(
                std::move(created.session), slots);
        }
        if (!inner) {
            error = "provider_unavailable";
            break;
        }
        ga::GpuWaveNetRealtimeNode node({.session = session_config,
                                         .channels = 1,
                                         .lead_blocks = cell.lead,
                                         .capacity = cell.capacity,
                                         .max_inflight = cell.inflight,
                                         .watchdog_blocks = cell.watchdog});
        std::vector<std::unique_ptr<WaveNetRealtimeChannel>> channels;
        channels.push_back(std::make_unique<TimingChannel>(std::move(inner), timeline));
        if (!ga::detail::WaveNetRealtimeTestAccess::prepare(node, std::move(channels))) {
            error = "node_prepare_failed";
            break;
        }
        ga::GpuAudioTransport transport;
        if (!transport.prepare(&node, {.ring_blocks = std::max(4u, cell.lead + 2),
                                       .run_worker_thread = true,
                                       .wake_on_write = cell.wake})) {
            error = "transport_prepare_failed";
            (void)node.release();
            break;
        }
        std::uint64_t ran = 0, latched_at = 0;
        std::thread callback([&] {
            realtime = make_realtime(period_ns) && realtime;
            std::vector<float> in(cell.frames), out_samples(cell.frames);
            const float* in_ptr = in.data();
            float* out_ptr = out_samples.data();
            pulp::audio::BufferView<const float> input(&in_ptr, 1, cell.frames);
            pulp::audio::BufferView<float> output(&out_ptr, 1, cell.frames);
            timeline.start_ns = now_ns() + 10'000'000;
            for (std::size_t block = 0; block < budget; ++block) {
                const auto due = timeline.start_ns + static_cast<std::uint64_t>(block * period_ns);
                if (now_ns() > due + static_cast<std::uint64_t>(period_ns))
                    ++overruns;
                sleep_until_ns(due);
                for (std::uint32_t i = 0; i < cell.frames; ++i)
                    in[i] = 0.1f * std::sin(0.01f * static_cast<float>(block * cell.frames + i));
                transport.process(input, output, cell.frames);
                timeline.callback_end[block] = now_ns();
                timeline.position.store(block + 1, std::memory_order_release);
                ran = block + 1;
                const auto admitted = timeline.next_admit.load(std::memory_order_acquire);
                const auto backlog = block + 1 > admitted ? block + 1 - admitted : 0;
                ++backlog_hist[std::min<std::uint64_t>(backlog, 128)];
                // The worker classifies a saturation at its next pump, so the
                // stream runs on briefly before the settled cause is read.
                if (latched_at == 0 && node.recovery_reason() != ga::GpuAudioRecoveryReason::None)
                    latched_at = block + 1;
                if (latched_at != 0 && block + 1 >= latched_at + 16)
                    break;
            }
        });
        callback.join();
        const auto status = node.status_snapshot();
        transport.release();
        (void)node.release();
        ran_total += ran;
        missed += status.missed_blocks;
        gpu_delivered += status.terminal_counts[static_cast<std::size_t>(
            ga::GpuAudioTerminalDisposition::GpuDelivered)];
        switch (status.recovery_reason) {
        case ga::GpuAudioRecoveryReason::None:
            break;
        case ga::GpuAudioRecoveryReason::ProviderUnresponsive:
            ++unresponsive;
            break;
        case ga::GpuAudioRecoveryReason::InputSaturated:
            ++saturated;
            break;
        default:
            ++other;
            break;
        }
        for (std::size_t s = 0; s < ran; ++s) {
            if (timeline.received[s] == 0 || timeline.submitted[s] == 0)
                continue;
            ++accepted;
            blocks.push_back(timeline.silent_blocks[s]);
            ms.push_back(static_cast<double>(timeline.silent_ns[s]) / 1.0e6);
            const auto collect = timeline.received[s] - timeline.submitted[s];
            latency.push_back(static_cast<double>(collect) / 1.0e6);
            const auto due = timeline.callback_end[s];
            admit.push_back(timeline.submitted[s] > due
                                ? static_cast<double>(timeline.submitted[s] - due) / 1.0e6
                                : 0.0);
            if (timeline.gpu_ns[s] != 0) {
                gpu.push_back(static_cast<double>(timeline.gpu_ns[s]) / 1.0e6);
                host.push_back(static_cast<double>(collect > timeline.gpu_ns[s]
                                                       ? collect - timeline.gpu_ns[s]
                                                       : 0) /
                               1.0e6);
            }
        }
        if (status.recovery_reason == ga::GpuAudioRecoveryReason::None)
            break;
    }
    if (getloadavg(load, 3) > 0)
        out << R"(,"host_load_1m_end":)" << load[0];
    if (!error.empty())
        out << R"(,"error":")" << error << "\"";
    out << R"(,"blocks":)" << ran_total << R"(,"seconds":)"
        << static_cast<double>(ran_total) * period_ns / 1.0e9 << R"(,"realtime_thread":)"
        << (realtime ? "true" : "false") << R"(,"callback_overruns":)" << overruns
        << R"(,"segments":)" << segments << R"(,"latched":{"provider_unresponsive":)"
        << unresponsive << R"(,"input_saturated":)" << saturated << R"(,"other":)" << other
        << R"(},"accepted_blocks":)" << accepted << R"(,"missed_blocks":)" << missed
        << R"(,"silent_blocks":)" << stats(blocks) << R"(,"silent_ms":)" << stats(ms)
        << R"(,"submit_to_collect_ms":)" << stats(latency) << R"(,"callback_to_admit_ms":)"
        << stats(admit) << R"(,"gpu_busy_ms":)" << stats(gpu) << R"(,"collect_minus_gpu_ms":)"
        << stats(host) << R"(,"gpu_delivered_share":)"
        << (ran_total ? static_cast<double>(gpu_delivered) / static_cast<double>(ran_total) : 0.0);
    // Accepted blocks whose silence reached each window: the trips that
    // window would have latched.
    out << R"(,"trips_at_blocks":{)";
    bool first = true;
    for (const std::uint32_t w : {1u, 3u, 8u, 16u, 32u}) {
        const auto n =
            std::count_if(blocks.begin(), blocks.end(), [&](double b) { return b >= w; });
        out << (first ? "" : ",") << "\"" << w << "\":" << n;
        first = false;
    }
    out << R"(},"trips_at_ms":{)";
    first = true;
    for (const double t : {10.0, 20.0, 50.0, 100.0}) {
        // A time window is enforced as ceil(T / block) callback blocks.
        const auto w = std::ceil(t * 1.0e6 / period_ns);
        const auto n =
            std::count_if(blocks.begin(), blocks.end(), [&](double b) { return b >= w; });
        out << (first ? "" : ",") << "\"" << t << "\":" << n;
        first = false;
    }
    // Callbacks at which a ring of C blocks would have been full.
    out << R"(},"backlog_at_least":{)";
    first = true;
    for (const std::uint32_t c : {4u, 6u, 8u, 16u, 32u, 64u, 128u}) {
        std::uint64_t over = 0;
        for (std::size_t i = c; i < backlog_hist.size(); ++i)
            over += backlog_hist[i];
        out << (first ? "" : ",") << "\"" << c << "\":" << over;
        first = false;
    }
    out << "}}";
    return out.str();
}

std::vector<std::string> split(std::string_view value) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find(',', start);
        parts.emplace_back(value.substr(start, end == std::string_view::npos ? end : end - start));
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return parts;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> frames{"64", "128", "256"}, leads{"2"}, workers{"wake"},
        models{"standard"}, capacities{"4", "8", "16", "32", "64"},
        watchdogs{"0", "3", "8", "16", "32"};
    double seconds = 600.0;
    std::uint32_t measure_watchdog = kNever;
    // Measure mode's ring: wide enough by default that only a stall far past
    // any window could fill it.
    std::uint32_t measure_capacity = 512;
    std::vector<std::string> inflights{"1"};
    bool staged_timestamps = false;
    std::string mode = "measure", tag = "run";
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto value = [&](std::string_view prefix) { return arg.substr(prefix.size()); };
        if (arg.starts_with("--frames="))
            frames = split(value("--frames="));
        else if (arg.starts_with("--leads="))
            leads = split(value("--leads="));
        else if (arg.starts_with("--workers="))
            workers = split(value("--workers="));
        else if (arg.starts_with("--models="))
            models = split(value("--models="));
        else if (arg.starts_with("--capacities="))
            capacities = split(value("--capacities="));
        else if (arg.starts_with("--watchdogs="))
            watchdogs = split(value("--watchdogs="));
        else if (arg.starts_with("--seconds="))
            seconds = std::strtod(std::string(value("--seconds=")).c_str(), nullptr);
        else if (arg.starts_with("--watchdog="))
            // Measure with the node's real watchdog at this window instead of
            // a disabled one (the forced-small-window mutation).
            measure_watchdog =
                static_cast<std::uint32_t>(std::stoul(std::string(value("--watchdog="))));
        else if (arg.starts_with("--inflight="))
            inflights = split(value("--inflight="));
        else if (arg == "--staged-timestamps")
            staged_timestamps = true;
        else if (arg.starts_with("--capacity="))
            measure_capacity =
                static_cast<std::uint32_t>(std::stoul(std::string(value("--capacity="))));
        else if (arg.starts_with("--tag="))
            tag = std::string(value("--tag="));
        else if (arg == "--hang")
            mode = "hang";
        else if (arg == "--ordering")
            mode = "ordering";
        else
            return 2;
    }
    for (const auto& f : frames)
        for (const auto& l : leads)
            for (const auto& w : workers) {
                Cell cell{.frames = static_cast<std::uint32_t>(std::stoul(f)),
                          .lead = static_cast<std::uint32_t>(std::stoul(l)),
                          .wake = w == "wake",
                          .seconds = seconds,
                          .mode = mode};
                if (mode == "ordering") {
                    for (const auto& c : capacities)
                        for (const auto& wd : watchdogs) {
                            cell.capacity = static_cast<std::uint32_t>(std::stoul(c));
                            cell.watchdog = static_cast<std::uint32_t>(std::stoul(wd));
                            if (cell.capacity <= cell.lead)
                                continue;
                            std::cout << run_cell(cell, tag) << std::endl;
                        }
                } else if (mode == "hang") {
                    std::cout << run_cell(cell, tag) << std::endl;
                } else {
                    cell.watchdog = measure_watchdog;
                    cell.capacity = measure_capacity;
                    cell.staged_timestamps = staged_timestamps;
                    for (const auto& m : models)
                        for (const auto& n : inflights) {
                            cell.model = m;
                            cell.inflight = static_cast<std::uint32_t>(std::stoul(n));
                            std::cout << run_measure(cell, tag) << std::endl;
                        }
                }
            }
    return 0;
}
