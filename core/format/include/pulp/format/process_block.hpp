#pragma once

#include <pulp/audio/buffer.hpp>
#include <cstddef>
#include <span>
#include <string_view>

namespace pulp::format {

/// Direction of an audio bus in a process block.
enum class BusDirection {
    Input,
    Output,
};

/// Semantic role for a process-block bus.
enum class BusRole {
    Main,
    Sidechain,
    Aux,
};

/// Metadata that travels with a non-owning bus buffer view.
struct BusBufferInfo {
    std::string_view name;
    std::size_t index = 0;
    BusDirection direction = BusDirection::Input;
    BusRole role = BusRole::Main;
    int declared_channels = 0;
    bool optional = false;
    bool active = true;
};

/// Non-owning view of one process bus.
///
/// The buffer view is host-owned and valid only for the current process call.
/// This is an additive multi-bus vocabulary; the existing Processor::process()
/// main-in/main-out signature remains the ergonomic plugin-author path.
template <typename SampleType>
struct BusBufferView {
    BusBufferInfo info;
    audio::BufferView<SampleType> buffer;

    bool active() const noexcept { return info.active; }
    bool optional() const noexcept { return info.optional; }
    bool main() const noexcept { return info.role == BusRole::Main; }
    bool sidechain() const noexcept { return info.role == BusRole::Sidechain; }
    std::size_t num_channels() const noexcept { return buffer.num_channels(); }
    std::size_t num_samples() const noexcept { return buffer.num_samples(); }

    /// True when an active bus has a non-empty view whose channel count matches
    /// the descriptor's declared count. Inactive buses are allowed to carry an
    /// empty view.
    bool matches_declared_layout() const noexcept {
        if (!active()) return buffer.empty();
        return info.declared_channels >= 0 &&
               num_channels() == static_cast<std::size_t>(info.declared_channels);
    }

    /// True when the bus is inactive or every advertised channel pointer is
    /// present. Hosts that receive null channel pointers should mark the bus
    /// inactive or omit it from the active process set.
    bool has_channel_storage() const noexcept {
        if (!active() || buffer.num_channels() == 0) return true;
        for (std::size_t ch = 0; ch < buffer.num_channels(); ++ch) {
            if (buffer.channel_ptr(ch) == nullptr) return false;
        }
        return true;
    }
};

/// Non-owning span of process buses for one direction.
template <typename SampleType>
class BusBufferSet {
public:
    BusBufferSet() = default;
    explicit BusBufferSet(std::span<BusBufferView<SampleType>> buses)
        : buses_(buses) {}

    std::size_t size() const noexcept { return buses_.size(); }
    bool empty() const noexcept { return buses_.empty(); }

    BusBufferView<SampleType>& operator[](std::size_t index) noexcept {
        return buses_[index];
    }
    const BusBufferView<SampleType>& operator[](std::size_t index) const noexcept {
        return buses_[index];
    }

    BusBufferView<SampleType>* find(BusRole role) noexcept {
        return find(role, 0);
    }

    const BusBufferView<SampleType>* find(BusRole role) const noexcept {
        return find(role, 0);
    }

    BusBufferView<SampleType>* find(BusRole role, std::size_t occurrence) noexcept {
        std::size_t seen = 0;
        for (auto& bus : buses_) {
            if (bus.info.role != role) continue;
            if (seen == occurrence) return &bus;
            ++seen;
        }
        return nullptr;
    }

    const BusBufferView<SampleType>* find(
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

    BusBufferView<SampleType>* find_by_index(std::size_t index) noexcept {
        for (auto& bus : buses_) {
            if (bus.info.index == index) return &bus;
        }
        return nullptr;
    }

    const BusBufferView<SampleType>* find_by_index(std::size_t index) const noexcept {
        for (const auto& bus : buses_) {
            if (bus.info.index == index) return &bus;
        }
        return nullptr;
    }

    BusBufferView<SampleType>* find_by_name(std::string_view name) noexcept {
        for (auto& bus : buses_) {
            if (bus.info.name == name) return &bus;
        }
        return nullptr;
    }

    const BusBufferView<SampleType>* find_by_name(std::string_view name) const noexcept {
        for (const auto& bus : buses_) {
            if (bus.info.name == name) return &bus;
        }
        return nullptr;
    }

    BusBufferView<SampleType>* main() noexcept { return find(BusRole::Main); }
    const BusBufferView<SampleType>* main() const noexcept { return find(BusRole::Main); }

    BusBufferView<SampleType>* sidechain() noexcept { return find(BusRole::Sidechain); }
    const BusBufferView<SampleType>* sidechain() const noexcept {
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
    std::span<BusBufferView<SampleType>> buses_;
};

/// Additive multi-bus process-block view. It is intentionally non-owning.
struct ProcessBuffers {
    BusBufferSet<const float> inputs;
    BusBufferSet<float> outputs;

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

} // namespace pulp::format
