#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <pulp/audio/sample_identity.hpp>
#include <pulp/audio/sample_slot_bank.hpp>

namespace pulp::audio {

struct EditableSampleSource {
    std::uint32_t sample_id = kInvalidSampleId;
    std::uint64_t generation = 0;
    std::uint32_t num_channels = 0;
    std::uint64_t num_frames = 0;
    double sample_rate = 0.0;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] static EditableSampleSource from_published_sample(std::uint32_t sample_id,
                                                                    const PublishedSampleView& view) noexcept;
};

struct EditableSampleState {
    EditableSampleSource source{};
    std::uint64_t revision = 0;
    std::uint64_t trim_start = 0;
    std::uint64_t trim_end = 0;
    std::uint64_t fade_in_frames = 0;
    std::uint64_t fade_out_frames = 0;
    bool reversed = false;
    float normalize_gain = 1.0f;
    bool has_loop = false;
    std::uint64_t loop_start = 0;
    std::uint64_t loop_end = 0;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::uint64_t trimmed_frames() const noexcept;
};

enum class SampleEditOperationKind : std::uint8_t {
    none,
    set_trim,
    set_fades,
    set_loop,
    clear_loop,
    set_reverse,
    set_normalize_gain,
};

struct SampleEditOperation {
    SampleEditOperationKind kind = SampleEditOperationKind::none;
    EditableSampleState before{};
    EditableSampleState after{};

    [[nodiscard]] bool valid() const noexcept;
};

// Metadata-only sample edit document for control/editor code.
//
// The document owns no sample audio and performs no import/export, file I/O, or
// destructive buffer mutation. Operations describe edit intent that a later
// renderer/exporter can materialize off the audio thread.
class EditableSampleDocument {
public:
    bool reset(EditableSampleSource source) noexcept;
    void clear() noexcept;

    [[nodiscard]] bool valid() const noexcept { return state_.valid(); }
    [[nodiscard]] const EditableSampleState& state() const noexcept { return state_; }

    [[nodiscard]] SampleEditOperation make_set_trim(std::uint64_t start,
                                                    std::uint64_t end) const noexcept;
    [[nodiscard]] SampleEditOperation make_set_fades(std::uint64_t fade_in_frames,
                                                     std::uint64_t fade_out_frames) const noexcept;
    [[nodiscard]] SampleEditOperation make_set_loop(std::uint64_t start,
                                                    std::uint64_t end) const noexcept;
    [[nodiscard]] SampleEditOperation make_clear_loop() const noexcept;
    [[nodiscard]] SampleEditOperation make_set_reverse(bool reversed) const noexcept;
    [[nodiscard]] SampleEditOperation make_set_normalize_gain(float gain) const noexcept;

    bool apply(const SampleEditOperation& operation) noexcept;
    bool restore_state(const EditableSampleState& state) noexcept;

private:
    [[nodiscard]] SampleEditOperation make_operation(SampleEditOperationKind kind,
                                                    EditableSampleState after) const noexcept;

    EditableSampleState state_{};
};

// Bounded undo/redo stack for editor-owned sample edit operations.
// prepare() reserves storage; perform/undo/redo do not allocate while the
// operation count remains within the prepared capacity. This is still a
// control-thread/editor primitive, not an audio-callback data structure.
class SampleEditHistory {
public:
    bool prepare(std::size_t max_operations);
    void clear() noexcept;

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t undo_count() const noexcept { return undo_stack_.size(); }
    [[nodiscard]] std::size_t redo_count() const noexcept { return redo_stack_.size(); }
    [[nodiscard]] bool can_undo() const noexcept { return !undo_stack_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_stack_.empty(); }

    bool perform(EditableSampleDocument& document, const SampleEditOperation& operation);
    bool undo(EditableSampleDocument& document);
    bool redo(EditableSampleDocument& document);

private:
    std::size_t capacity_ = 0;
    std::vector<SampleEditOperation> undo_stack_;
    std::vector<SampleEditOperation> redo_stack_;
};

}  // namespace pulp::audio
