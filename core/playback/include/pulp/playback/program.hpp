#pragma once

#include <pulp/playback/automation_limits.hpp>
#include <pulp/playback/audio_renderer_limits.hpp>
#include <pulp/playback/program_identity.hpp>
#include <pulp/playback/track_mixer_program.hpp>
#include <pulp/runtime/result.hpp>
#include <pulp/runtime/slot.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timeline/model.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace pulp::playback {

class ProgramCompilerTask;
class AudioTrackRendererProgram;
class DecodedAudioAssetPool;
class TrackAutomationProgram;

enum class ProgramErrorCode : std::uint8_t {
    InvalidGeneration,
    InvalidIdentity,
    DuplicateTrack,
    DuplicateClip,
    MissingTempoMap,
};

struct ProgramError {
    ProgramErrorCode code = ProgramErrorCode::InvalidGeneration;
    timeline::ItemId item;
};

runtime::Result<ProgramGeneration, ProgramError>
next_program_generation(ProgramGeneration current) noexcept;

enum class ProviderKind : std::uint8_t { Arrangement, Launcher, ExternalInput };

struct ProviderSelectorProgram {
    ProviderKind selected = ProviderKind::Arrangement;
    std::uint8_t available_mask = 1u;

    constexpr bool available(ProviderKind provider) const noexcept {
        const auto index = static_cast<unsigned>(provider);
        return index <= static_cast<unsigned>(ProviderKind::ExternalInput) &&
               (available_mask & (1u << index)) != 0;
    }
};

enum class RendererStatePolicy : std::uint8_t { Stateless, CarryByItemId };

enum class NoteProgramEventKind : std::uint8_t { Off, On };

/// Immutable arrangement-note event lowered by the program compiler. Musical
/// ticks remain available for diagnostics and state snapshots, while the
/// sample position is authoritative for half-open block scheduling against the
/// exact CompiledTempoMap used to build the owning PlaybackProgram. Distinct
/// ticks can quantize to the same compiled sample, so tick order remains
/// authoritative within one sample; note-offs precede note-ons only when both
/// the sample and tick are equal.
struct NoteProgramEvent {
    timebase::SamplePosition sample;
    timebase::TickPosition tick;
    timeline::ItemId clip_id;
    timeline::ItemId note_id;
    std::uint16_t velocity = 0xffff;
    std::uint8_t pitch = 60;
    std::uint8_t channel = 0;
    NoteProgramEventKind kind = NoteProgramEventKind::On;
    constexpr auto operator<=>(const NoteProgramEvent&) const = default;
};

/// One note's playback modifier, lowered with its draw key already folded from
/// the content's authored seed and the note identity. Only notes that actually
/// carry a modifier appear here, so an arrangement that authors none carries no
/// per-note modifier data and the renderer's gate costs one empty-span check.
struct CompiledNoteModifier {
    std::uint64_t draw_key = 0;
    timeline::NoteModifier modifier;
};

/// Looks up the modifier for `note_id` in a table sorted by note id. Returns
/// nullptr when the note plays unconditionally, which is the common case.
const CompiledNoteModifier* find_note_modifier(std::span<const CompiledNoteModifier> modifiers,
                                               timeline::ItemId note_id) noexcept;

constexpr bool note_program_event_less(const NoteProgramEvent& lhs,
                                       const NoteProgramEvent& rhs) noexcept {
    if (lhs.sample != rhs.sample)
        return lhs.sample < rhs.sample;
    if (lhs.tick != rhs.tick)
        return lhs.tick < rhs.tick;
    if (lhs.kind != rhs.kind)
        return lhs.kind < rhs.kind; // note-offs first at one exact tick
    if (lhs.clip_id != rhs.clip_id)
        return lhs.clip_id < rhs.clip_id;
    return lhs.note_id < rhs.note_id;
}

class TrackProgram {
  public:
    timeline::ItemId id() const noexcept {
        return id_;
    }
    ProgramGeneration generation() const noexcept {
        return generation_;
    }
    ProviderSelectorProgram provider() const noexcept {
        return provider_;
    }
    RendererStatePolicy state_policy() const noexcept {
        return state_policy_;
    }
    std::span<const timeline::ItemId> ordered_clip_ids() const noexcept {
        return clip_ids_;
    }
    std::span<const NoteProgramEvent> arrangement_note_events() const noexcept {
        return note_events_;
    }
    /// Sorted by note id. Empty for a track whose notes all play by default.
    std::span<const CompiledNoteModifier> note_modifiers() const noexcept {
        return note_modifiers_;
    }
    const AudioTrackRendererProgram* audio_program() const noexcept {
        return audio_program_.get();
    }
    std::span<const timeline::ItemId> ordered_device_placement_ids() const noexcept {
        return device_placement_ids_;
    }
    const TrackAutomationProgram* automation_program() const noexcept {
        return automation_program_.get();
    }
    const std::shared_ptr<const TrackAutomationProgram>& automation_program_owner() const noexcept {
        return automation_program_;
    }
    std::uint64_t expanded_clip_count() const noexcept {
        return expanded_clip_count_;
    }
    std::uint64_t expanded_note_event_count() const noexcept {
        return expanded_note_event_count_;
    }
    std::uint64_t generated_id_count() const noexcept {
        return generated_id_count_;
    }
    std::uint64_t generated_id_start() const noexcept {
        return generated_id_start_;
    }

    /// The track's own level and stereo placement, with any lanes that automate
    /// them already resolved. Borrows from automation_program_, which this
    /// program holds alive.
    const TrackMixerProgram& mixer() const noexcept {
        return mixer_;
    }

  private:
    friend class ProgramCompilerTask;
    TrackProgram(timeline::ItemId id, ProgramGeneration generation,
                 ProviderSelectorProgram provider, RendererStatePolicy state_policy,
                 std::vector<timeline::ItemId> clip_ids, std::vector<NoteProgramEvent> note_events,
                 std::vector<CompiledNoteModifier> note_modifiers,
                 std::shared_ptr<const AudioTrackRendererProgram> audio_program,
                 std::vector<timeline::ItemId> device_placement_ids,
                 std::shared_ptr<const TrackAutomationProgram> automation_program,
                 std::uint64_t expanded_clip_count,
                 std::uint64_t expanded_note_event_count,
                 std::uint64_t generated_id_start,
                 std::uint64_t generated_id_count,
                 TrackMixerProgram mixer) noexcept;

    timeline::ItemId id_;
    ProgramGeneration generation_ = 0;
    ProviderSelectorProgram provider_;
    RendererStatePolicy state_policy_ = RendererStatePolicy::CarryByItemId;
    std::vector<timeline::ItemId> clip_ids_;
    std::vector<NoteProgramEvent> note_events_;
    std::vector<CompiledNoteModifier> note_modifiers_;
    std::shared_ptr<const AudioTrackRendererProgram> audio_program_;
    std::vector<timeline::ItemId> device_placement_ids_;
    std::shared_ptr<const TrackAutomationProgram> automation_program_;
    std::uint64_t expanded_clip_count_ = 0;
    std::uint64_t expanded_note_event_count_ = 0;
    std::uint64_t generated_id_start_ = 0;
    std::uint64_t generated_id_count_ = 0;
    TrackMixerProgram mixer_;
};

class PlaybackProgram {
  public:
    ProgramGeneration generation() const noexcept {
        return generation_;
    }
    std::uint64_t document_revision() const noexcept {
        return document_revision_;
    }
    timeline::ItemId project_id() const noexcept {
        return project_id_;
    }
    timeline::ItemId sequence_id() const noexcept {
        return sequence_id_;
    }
    const timebase::CompiledTempoMap& tempo_map() const noexcept {
        return *tempo_map_;
    }
    const std::shared_ptr<const timebase::CompiledTempoMap>& tempo_map_owner() const noexcept {
        return tempo_map_;
    }
    const std::shared_ptr<const DecodedAudioAssetPool>& audio_assets_owner() const noexcept {
        return audio_assets_;
    }
    const AudioRendererLimits& audio_limits() const noexcept {
        return audio_limits_;
    }
    const AutomationPlaybackLimits& automation_limits() const noexcept {
        return automation_limits_;
    }
    std::uint64_t generated_id_base() const noexcept {
        return generated_id_base_;
    }
    std::span<const std::shared_ptr<const TrackProgram>> tracks() const noexcept {
        return tracks_;
    }
    const TrackProgram* find_track(timeline::ItemId id) const noexcept;

  private:
    friend class ProgramCompilerTask;
    PlaybackProgram(ProgramGeneration generation, std::uint64_t document_revision,
                    timeline::ItemId project_id, timeline::ItemId sequence_id,
                    std::shared_ptr<const timebase::CompiledTempoMap> tempo_map,
                    std::shared_ptr<const DecodedAudioAssetPool> audio_assets,
                    AudioRendererLimits audio_limits, AutomationPlaybackLimits automation_limits,
                    std::uint64_t generated_id_base,
                    std::vector<std::shared_ptr<const TrackProgram>> tracks) noexcept;
    const std::shared_ptr<const TrackProgram>* find_track_owner(timeline::ItemId id) const noexcept;

    ProgramGeneration generation_ = 0;
    std::uint64_t document_revision_ = 0;
    timeline::ItemId project_id_;
    timeline::ItemId sequence_id_;
    std::shared_ptr<const timebase::CompiledTempoMap> tempo_map_;
    std::shared_ptr<const DecodedAudioAssetPool> audio_assets_;
    AudioRendererLimits audio_limits_;
    AutomationPlaybackLimits automation_limits_;
    std::uint64_t generated_id_base_ = 0;
    std::vector<std::shared_ptr<const TrackProgram>> tracks_;
};

class PlaybackProgramStore {
  public:
    using ReadGuard = runtime::Slot<const PlaybackProgram>::ReadGuard;
    [[nodiscard]] ReadGuard read() const noexcept {
        return slot_.read();
    }
    bool has_value() const noexcept {
        return slot_.has_value();
    }

  private:
    friend struct PlaybackProgramCompilerCore;
    friend class ProgramCompilerTask;
    void publish(std::shared_ptr<const PlaybackProgram> program,
                 std::shared_ptr<const timeline::Project> project) {
        live_project_ = std::move(project);
        slot_.publish(std::move(program));
    }
    const std::shared_ptr<const PlaybackProgram>& live() const noexcept {
        return slot_.live();
    }
    const std::shared_ptr<const timeline::Project>& live_project() const noexcept {
        return live_project_;
    }
    bool try_bind_compiler() noexcept {
        bool expected = false;
        return compiler_bound_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    }
    void unbind_compiler() noexcept {
        compiler_bound_.store(false, std::memory_order_release);
    }
    runtime::Slot<const PlaybackProgram> slot_;
    // Control-thread provenance for exact same-revision adoption by a newly
    // bound compiler. Render threads never touch this sidecar.
    std::shared_ptr<const timeline::Project> live_project_;
    std::atomic<bool> compiler_bound_{false};
};

} // namespace pulp::playback
