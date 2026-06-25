#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/mpe_buffer.hpp>
#include <pulp/midi/ump_buffer.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>

namespace pulp::format {

struct ProcessContext;

/// Coarse processing mode for one block.
///
/// Realtime blocks run under the hard audio-thread contract. Offline blocks may
/// be rendered faster or slower than wall clock, but still use bounded buffers
/// and explicit event/scratch contracts.
enum class ProcessMode : std::uint8_t {
    Realtime,
    Offline,
};

struct ProcessBlockFlags {
    bool bypass = false;
    bool reset = false;
    bool tail_drain = false;
    bool transport_jump = false;
};

class ScratchArenaView {
public:
    ScratchArenaView() = default;
    explicit ScratchArenaView(std::span<std::byte> memory) noexcept
        : memory_(memory) {}
    ScratchArenaView(const ScratchArenaView&) = delete;
    ScratchArenaView& operator=(const ScratchArenaView&) = delete;
    ScratchArenaView(ScratchArenaView&&) = default;
    ScratchArenaView& operator=(ScratchArenaView&&) = default;

    void reset() noexcept { offset_ = 0; }
    std::size_t size_bytes() const noexcept { return memory_.size(); }
    std::size_t used_bytes() const noexcept { return offset_; }
    std::size_t remaining_bytes() const noexcept {
        return offset_ <= memory_.size() ? memory_.size() - offset_ : 0;
    }
    bool empty() const noexcept { return memory_.empty(); }

    std::span<std::byte> try_allocate_bytes(std::size_t byte_count,
                                             std::size_t alignment) noexcept {
        if (byte_count == 0) return {};
        if (alignment == 0 || !std::has_single_bit(alignment)) return {};
        if (memory_.empty()) return {};

        const auto base = reinterpret_cast<std::uintptr_t>(memory_.data());
        const auto current = base + offset_;
        const auto aligned = align_up(current, alignment);
        const auto aligned_offset = static_cast<std::size_t>(aligned - base);
        if (aligned_offset > memory_.size()) return {};
        if (byte_count > memory_.size() - aligned_offset) return {};

        offset_ = aligned_offset + byte_count;
        return memory_.subspan(aligned_offset, byte_count);
    }

    template<typename T>
    std::span<T> try_allocate(std::size_t count) noexcept {
        static_assert(std::is_trivially_copyable_v<T>,
                      "scratch spans are for plain DSP scratch types");
        if (count == 0) return {};
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) return {};
        auto bytes = try_allocate_bytes(count * sizeof(T), alignof(T));
        if (bytes.empty()) return {};
        return {reinterpret_cast<T*>(bytes.data()), count};
    }

private:
    static std::uintptr_t align_up(std::uintptr_t value,
                                   std::size_t alignment) noexcept {
        const auto mask = static_cast<std::uintptr_t>(alignment - 1);
        return (value + mask) & ~mask;
    }

    std::span<std::byte> memory_;
    std::size_t offset_ = 0;
};

/// Prepare-time scratch view. Storage is caller-owned and may be reused between
/// prepare slices. This type does not allocate.
class PrepareScratch : public ScratchArenaView {
public:
    using ScratchArenaView::ScratchArenaView;
};

/// Block-local scratch view for realtime/offline processing. Storage is
/// caller-owned and should be reset at the start of each block.
class BlockScratch : public ScratchArenaView {
public:
    using ScratchArenaView::ScratchArenaView;
};

enum class BusDirection : std::uint8_t {
    Input,
    Output,
};

enum class BusRole : std::uint8_t {
    Main,
    Aux,
    Sidechain,
};

struct BusBuffer {
    std::string_view name;
    BusDirection direction = BusDirection::Input;
    BusRole role = BusRole::Main;
    bool active = true;
    audio::BufferView<const float> input;
    audio::BufferView<float> output;

    std::size_t num_channels() const noexcept {
        return direction == BusDirection::Output ? output.num_channels()
                                                 : input.num_channels();
    }

    std::size_t num_frames() const noexcept {
        return direction == BusDirection::Output ? output.num_samples()
                                                 : input.num_samples();
    }

    bool empty() const noexcept {
        return direction == BusDirection::Output ? output.empty() : input.empty();
    }
};

/// Fixed-capacity, non-owning set of block-local bus views.
///
/// Names are string_views owned by the caller. Audio memory is owned by the host
/// or test harness. Adding buses never allocates and fails when capacity is
/// exhausted. Duplicate bus names are caller-owned; find() returns the first
/// matching (direction, name) pair.
class BusBufferSet {
public:
    static constexpr std::size_t kMaxBuses = 16;

    bool add_input(std::string_view name,
                   audio::BufferView<const float> input,
                   BusRole role = BusRole::Main,
                   bool active = true) noexcept {
        if (size_ >= buses_.size()) return false;
        buses_[size_++] = BusBuffer{name, BusDirection::Input, role, active, input, {}};
        return true;
    }

    bool add_output(std::string_view name,
                    audio::BufferView<float> output,
                    BusRole role = BusRole::Main,
                    bool active = true) noexcept {
        if (size_ >= buses_.size()) return false;
        buses_[size_++] = BusBuffer{name, BusDirection::Output, role, active, {}, output};
        return true;
    }

    void clear() noexcept { size_ = 0; }
    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return buses_.size(); }
    bool empty() const noexcept { return size_ == 0; }

    std::span<BusBuffer> buses() noexcept { return {buses_.data(), size_}; }
    std::span<const BusBuffer> buses() const noexcept { return {buses_.data(), size_}; }

    BusBuffer* find(BusDirection direction, std::string_view name) noexcept {
        for (auto& bus : buses()) {
            if (bus.direction == direction && bus.name == name) return &bus;
        }
        return nullptr;
    }

    const BusBuffer* find(BusDirection direction, std::string_view name) const noexcept {
        for (const auto& bus : buses()) {
            if (bus.direction == direction && bus.name == name) return &bus;
        }
        return nullptr;
    }

    BusBuffer* first(BusDirection direction,
                     BusRole role = BusRole::Main) noexcept {
        for (auto& bus : buses()) {
            if (bus.direction == direction && bus.role == role) return &bus;
        }
        return nullptr;
    }

    const BusBuffer* first(BusDirection direction,
                           BusRole role = BusRole::Main) const noexcept {
        for (const auto& bus : buses()) {
            if (bus.direction == direction && bus.role == role) return &bus;
        }
        return nullptr;
    }

    bool validate_frame_count(std::size_t frame_count) const noexcept {
        for (const auto& bus : buses()) {
            if (bus.active && bus.num_frames() != frame_count) return false;
        }
        return true;
    }

private:
    std::array<BusBuffer, kMaxBuses> buses_{};
    std::size_t size_ = 0;
};

/// Metadata that travels with a non-owning richer-surface process bus view.
struct ProcessBusBufferInfo {
    std::string_view name;
    std::size_t index = 0;
    BusDirection direction = BusDirection::Input;
    BusRole role = BusRole::Main;
    int declared_channels = 0;
    bool optional = false;
    bool active = true;
};

/// Non-owning view of one richer-surface process bus.
template <typename SampleType>
struct ProcessBusBufferView {
    ProcessBusBufferInfo info;
    audio::BufferView<SampleType> buffer;

    bool active() const noexcept { return info.active; }
    bool optional() const noexcept { return info.optional; }
    bool main() const noexcept { return info.role == BusRole::Main; }
    bool sidechain() const noexcept { return info.role == BusRole::Sidechain; }
    std::size_t num_channels() const noexcept { return buffer.num_channels(); }
    std::size_t num_samples() const noexcept { return buffer.num_samples(); }

    bool matches_declared_layout() const noexcept {
        if (!active()) return buffer.empty();
        return info.declared_channels >= 0 &&
               num_channels() == static_cast<std::size_t>(info.declared_channels);
    }

    bool has_channel_storage() const noexcept {
        if (!active() || buffer.num_channels() == 0) return true;
        for (std::size_t ch = 0; ch < buffer.num_channels(); ++ch) {
            if (buffer.channel_ptr(ch) == nullptr) return false;
        }
        return true;
    }
};

/// Non-owning span of richer-surface process buses for one direction.
template <typename SampleType>
class ProcessBusBufferSet {
public:
    ProcessBusBufferSet() = default;
    explicit ProcessBusBufferSet(std::span<ProcessBusBufferView<SampleType>> buses)
        : buses_(buses) {}

    std::size_t size() const noexcept { return buses_.size(); }
    bool empty() const noexcept { return buses_.empty(); }

    ProcessBusBufferView<SampleType>& operator[](std::size_t index) noexcept {
        return buses_[index];
    }
    const ProcessBusBufferView<SampleType>& operator[](std::size_t index) const noexcept {
        return buses_[index];
    }

    ProcessBusBufferView<SampleType>* find(BusRole role) noexcept {
        return find(role, 0);
    }

    const ProcessBusBufferView<SampleType>* find(BusRole role) const noexcept {
        return find(role, 0);
    }

    ProcessBusBufferView<SampleType>* find(BusRole role,
                                           std::size_t occurrence) noexcept {
        std::size_t seen = 0;
        for (auto& bus : buses_) {
            if (bus.info.role != role) continue;
            if (seen == occurrence) return &bus;
            ++seen;
        }
        return nullptr;
    }

    const ProcessBusBufferView<SampleType>* find(
        BusRole role,
        std::size_t occurrence) const noexcept {
        std::size_t seen = 0;
        for (const auto& bus : buses_) {
            if (bus.info.role != role) continue;
            if (seen == occurrence) return &bus;
            ++seen;
        }
        return nullptr;
    }

    ProcessBusBufferView<SampleType>* find_by_index(std::size_t index) noexcept {
        for (auto& bus : buses_) {
            if (bus.info.index == index) return &bus;
        }
        return nullptr;
    }

    const ProcessBusBufferView<SampleType>* find_by_index(
        std::size_t index) const noexcept {
        for (const auto& bus : buses_) {
            if (bus.info.index == index) return &bus;
        }
        return nullptr;
    }

    ProcessBusBufferView<SampleType>* find_by_name(std::string_view name) noexcept {
        for (auto& bus : buses_) {
            if (bus.info.name == name) return &bus;
        }
        return nullptr;
    }

    const ProcessBusBufferView<SampleType>* find_by_name(
        std::string_view name) const noexcept {
        for (const auto& bus : buses_) {
            if (bus.info.name == name) return &bus;
        }
        return nullptr;
    }

    ProcessBusBufferView<SampleType>* main() noexcept { return find(BusRole::Main); }
    const ProcessBusBufferView<SampleType>* main() const noexcept {
        return find(BusRole::Main);
    }

    ProcessBusBufferView<SampleType>* sidechain() noexcept {
        return find(BusRole::Sidechain);
    }
    const ProcessBusBufferView<SampleType>* sidechain() const noexcept {
        return find(BusRole::Sidechain);
    }

    std::size_t active_count() const noexcept {
        std::size_t count = 0;
        for (const auto& bus : buses_) {
            if (bus.active()) ++count;
        }
        return count;
    }

    std::size_t count(BusRole role) const noexcept {
        std::size_t total = 0;
        for (const auto& bus : buses_) {
            if (bus.info.role == role) ++total;
        }
        return total;
    }

    std::size_t active_count(BusRole role) const noexcept {
        std::size_t total = 0;
        for (const auto& bus : buses_) {
            if (bus.info.role == role && bus.active()) ++total;
        }
        return total;
    }

    bool layouts_match_descriptors() const noexcept {
        for (const auto& bus : buses_) {
            if (!bus.matches_declared_layout()) return false;
        }
        return true;
    }

    bool active_buses_have_storage() const noexcept {
        for (const auto& bus : buses_) {
            if (!bus.has_channel_storage()) return false;
        }
        return true;
    }

private:
    std::span<ProcessBusBufferView<SampleType>> buses_;
};

/// Additive richer-surface process-buffer view. It is intentionally non-owning.
struct ProcessBuffers {
    ProcessBusBufferSet<const float> inputs;
    ProcessBusBufferSet<float> outputs;

    const audio::BufferView<const float>* main_input() const noexcept {
        if (auto* bus = inputs.main(); bus && bus->active()) return &bus->buffer;
        return nullptr;
    }

    audio::BufferView<float>* main_output() noexcept {
        if (auto* bus = outputs.main(); bus && bus->active()) return &bus->buffer;
        return nullptr;
    }

    const audio::BufferView<const float>* sidechain_input() const noexcept {
        if (auto* bus = inputs.sidechain(); bus && bus->active()) return &bus->buffer;
        return nullptr;
    }

    bool layouts_match_descriptors() const noexcept {
        return inputs.layouts_match_descriptors() &&
               outputs.layouts_match_descriptors();
    }

    bool active_buses_have_storage() const noexcept {
        return inputs.active_buses_have_storage() &&
               outputs.active_buses_have_storage();
    }
};

struct EventDropCounters {
    std::uint32_t parameter_events = 0;
    std::uint32_t midi_events = 0;
    std::uint32_t sysex_events = 0;
    std::uint32_t ump_packets = 0;
    std::uint32_t mpe_events = 0;
    std::uint32_t graph_events = 0;
    std::uint32_t audio_rate_modulations = 0;

    bool any() const noexcept {
        return parameter_events != 0 || audio_rate_modulations != 0 ||
               midi_events != 0 || sysex_events != 0 || ump_packets != 0 ||
               mpe_events != 0 || graph_events != 0;
    }

    std::uint32_t total() const noexcept {
        return parameter_events + audio_rate_modulations + midi_events +
               sysex_events + ump_packets + mpe_events + graph_events;
    }
};

/// Non-owning dense per-sample parameter-modulation lane.
///
/// Values are already in the destination parameter's plain value domain, one
/// value per process frame. ProcessBlock-native runtimes can consume these
/// without expanding audio-rate modulation into sparse ParameterEventQueue
/// entries. The pointed-to sample storage is caller-owned.
struct AudioRateModulationView {
    std::uint32_t param_id = 0;
    std::span<const float> values;

    bool empty() const noexcept { return values.empty(); }
    std::size_t size() const noexcept { return values.size(); }
};

/// Non-owning view of all sparse events visible to one process block.
///
/// The event containers are owned by adapters or test harnesses. Overflow/drop
/// counters are explicit so realtime code can report bounded-capacity loss
/// without logging or allocating from the audio callback.
struct EventBlock {
    const state::ParameterEventQueue* parameter_events = nullptr;
    // Mutable to match the legacy Processor::process() ABI. Processors should
    // still treat inbound MIDI as read-only; graph paths that need shared
    // immutable event streams should adapt before entering the legacy ABI.
    midi::MidiBuffer* midi_in = nullptr;
    midi::MidiBuffer* midi_out = nullptr;
    // Null means no sidecar events for this block. Adapters should leave these
    // null instead of publishing empty sidecars.
    const midi::MpeBuffer* mpe_input = nullptr;
    const midi::UmpBuffer* ump_input = nullptr;
    EventDropCounters drops;
    std::span<const AudioRateModulationView> audio_rate_modulations;

    std::span<const state::ParameterEvent> parameters() const noexcept {
        return parameter_events ? parameter_events->events()
                                : std::span<const state::ParameterEvent>{};
    }

    std::size_t parameter_event_count() const noexcept {
        return parameter_events ? parameter_events->size() : 0;
    }

    std::size_t midi_input_event_count() const noexcept {
        return midi_in ? midi_in->size() : 0;
    }

    std::size_t midi_output_event_count() const noexcept {
        return midi_out ? midi_out->size() : 0;
    }

    std::size_t audio_rate_modulation_count() const noexcept {
        return audio_rate_modulations.size();
    }

    std::size_t sysex_event_count() const noexcept {
        return midi_in ? midi_in->sysex_size() : 0;
    }

    bool empty() const noexcept {
        return parameter_event_count() == 0 && midi_input_event_count() == 0 &&
               midi_output_event_count() == 0 && sysex_event_count() == 0 &&
               mpe_input == nullptr && ump_input == nullptr &&
               audio_rate_modulations.empty() && !drops.any();
    }
};

/// Additive block-scoped process contract.
///
/// This does not replace Processor::process(). It packages the
/// existing transport context with explicit bus, event, scratch, mode, and
/// render-speed state so new runtime paths can share one contract.
struct ProcessBlock {
    ProcessMode mode = ProcessMode::Realtime;
    ProcessBlockFlags flags;
    double sample_rate = 0.0;
    std::uint32_t frame_count = 0;
    double render_speed = 1.0;
    std::uint64_t block_index = 0;
    const ProcessContext* transport = nullptr;
    BusBufferSet* buses = nullptr;
    EventBlock* events = nullptr;
    BlockScratch* scratch = nullptr;

    bool is_realtime() const noexcept { return mode == ProcessMode::Realtime; }
    bool is_offline() const noexcept { return mode == ProcessMode::Offline; }
    bool has_transport() const noexcept { return transport != nullptr; }
    bool has_scratch() const noexcept { return scratch != nullptr; }

    bool validate() const noexcept {
        if (mode != ProcessMode::Realtime && mode != ProcessMode::Offline) return false;
        if (sample_rate <= 0.0 || !std::isfinite(sample_rate) || frame_count == 0) return false;
        if (render_speed <= 0.0 || !std::isfinite(render_speed)) return false;
        if (buses && !buses->validate_frame_count(frame_count)) return false;
        if (events) {
            for (const auto& lane : events->audio_rate_modulations) {
                if (lane.values.size() != frame_count) return false;
            }
        }
        return true;
    }
};

} // namespace pulp::format
