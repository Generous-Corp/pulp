#pragma once

#include "streaming_model.hpp"

#include <cstdint>
#include <limits>
#include <utility>

namespace pulp::gpu_audio::detail {

/// Provider preference is a control-plane policy.  It does not make a
/// provider executable by itself; an adapter must explicitly publish its
/// capabilities before a future worker backend can be selected.
enum class NeuralProvider : std::uint8_t { Cpu, Mlx, Dawn };
enum class NeuralProviderPreference : std::uint8_t { CpuOnly, PreferMlx, PreferDawn, Auto };

struct NeuralProviderCapabilities {
    bool cpu = true;
    bool mlx = false;
    bool dawn = false;
};

/// A model entry is a control-thread snapshot.  It copies scalar metadata and
/// retains immutable string views owned by the model/artifact store.  Callback
/// code only reads this snapshot; it never consults a mutable model registry.
struct NeuralModelEntry {
    StreamingModel* model = nullptr;
    StreamingModelSpec spec{};
    std::string_view artifact_id{};
    std::string_view artifact_hash{};

    bool valid() const noexcept {
        return model != nullptr && validate_streaming_model_spec(spec).accepted() &&
               !artifact_id.empty() && !artifact_hash.empty();
    }
};

struct NeuralProcessorSnapshot {
    const NeuralModelEntry* entry = nullptr;
    NeuralProvider provider = NeuralProvider::Cpu;
    NeuralProviderPreference preference = NeuralProviderPreference::CpuOnly;
    std::uint64_t generation = 0;
    std::uint32_t max_frames = 0;
    bool prepared = false;
    bool fell_back_to_cpu = false;
};

/// Private lifecycle facade for CPU models and future MLX/Dawn adapters.
/// Preparation is transactional: prepare() writes a pending snapshot, and
/// publish() is the only operation that makes it visible to process_cpu().
/// The callback path is a bounded pointer read plus the model's allocation-free
/// process method; it never allocates, loads models, or chooses a provider.
class NeuralProcessor final {
  public:
    explicit NeuralProcessor(StreamingModel& model) noexcept : model_(model) {
        entry_.model = &model;
        entry_.spec = model.spec();
    }

    NeuralProcessor(const NeuralProcessor&) = delete;
    NeuralProcessor& operator=(const NeuralProcessor&) = delete;

    const NeuralModelEntry& model_entry() const noexcept { return entry_; }
    const NeuralProcessorSnapshot& snapshot() const noexcept { return active_; }

    bool prepare(const StreamingPrepareContext& context,
                 NeuralProviderPreference preference = NeuralProviderPreference::Auto,
                 NeuralProviderCapabilities capabilities = {}) noexcept {
        if (active_.prepared || pending_.prepared || cleanup_required_)
            return false;
        if (next_generation_ == std::numeric_limits<std::uint64_t>::max())
            return false;
        if (context.spec != &model_.spec() || !valid_streaming_prepare_context(context))
            return false;
        // This facade only owns the portable CPU execution lane.  A future
        // MLX/Dawn adapter must provide its own worker/backend seam before it
        // can advertise a non-CPU capability here.
        if (!capabilities.cpu)
            return false;

        entry_.spec = model_.spec();
        entry_.artifact_id = context.artifact_id;
        entry_.artifact_hash = context.artifact_hash;
        if (!entry_.valid())
            return false;
        if (!model_.prepare(context)) {
            // A failed prepare may have acquired partial model resources. The
            // model contract makes release idempotent and retryable so the
            // failed transaction cannot leak into the next admission.
            cleanup_required_ = true;
            if (model_.release())
                cleanup_required_ = false;
            return false;
        }
        cleanup_required_ = false;

        pending_ = {};
        pending_.entry = &entry_;
        pending_.preference = preference;
        pending_.provider = NeuralProvider::Cpu;
        // Auto on a CPU-only host is an honest CPU selection, not a fallback.
        // An explicit non-CPU preference (or an advertised non-CPU capability
        // that this CPU-only facade cannot yet execute) records fallback.
        pending_.fell_back_to_cpu =
            preference == NeuralProviderPreference::PreferMlx ||
            preference == NeuralProviderPreference::PreferDawn || capabilities.mlx ||
            capabilities.dawn;
        pending_.max_frames = context.max_frames;
        pending_.generation = ++next_generation_;
        pending_.prepared = true;
        return true;
    }

    bool publish() noexcept {
        if (!pending_.prepared || active_.prepared)
            return false;
        active_ = pending_;
        pending_ = {};
        return true;
    }

    void process_cpu(const audio::BufferView<const float>& input,
                     audio::BufferView<float>& output, std::uint32_t frames,
                     StreamingBlockStamp stamp) noexcept {
        if (!active_.prepared || active_.entry == nullptr || active_.entry->model == nullptr) {
            output.clear();
            return;
        }
        active_.entry->model->process_cpu(input, output, frames,
                                          stamp);
    }

    bool reset(StreamingResetReason reason) noexcept {
        if (!active_.prepared || active_.entry == nullptr)
            return false;
        if (!model_.quiesce())
            return false;
        auto next = active_.generation + 1;
        if (next == 0)
            return false;
        model_.reset(next, reason);
        next_generation_ = next;
        active_.generation = next;
        return true;
    }

    bool release() noexcept {
        const bool owns_model = active_.prepared || pending_.prepared || cleanup_required_;
        if (!owns_model)
            return true;
        if (!model_.quiesce())
            return false;
        if (!model_.release())
            return false;
        pending_ = {};
        active_ = {};
        cleanup_required_ = false;
        return true;
    }

  private:
    StreamingModel& model_;
    NeuralModelEntry entry_{};
    NeuralProcessorSnapshot active_{};
    NeuralProcessorSnapshot pending_{};
    std::uint64_t next_generation_ = 0;
    bool cleanup_required_ = false;
};

static_assert(noexcept(std::declval<NeuralProcessor&>().publish()));

} // namespace pulp::gpu_audio::detail
