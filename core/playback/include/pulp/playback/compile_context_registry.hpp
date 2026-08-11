#pragma once

#include <pulp/playback/dirty_track_resolver.hpp>
#include <pulp/runtime/result.hpp>
#include <pulp/timeline/compile_context.hpp>
#include <pulp/timeline/production_mode.hpp>
#include <pulp/timeline/schema_registry.hpp>
#include <pulp/timeline/transaction.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::playback {

namespace detail {
struct CompileInvalidationData;
struct ContextRegistryGeneration;
} // namespace detail

/// The immutable program-fragment family emitted by a registered compiler.
/// Built-in MIDI and audio stay on their direct compiler paths.
enum class ContentProgramOutputKind : std::uint8_t { Notes };

/// Registered note fragments currently admit Reset only. CarryByItemId is a
/// reserved value for a future fragment contract with renderer-stable note
/// keys; ordinal-generated IDs cannot safely carry state across insertions.
enum class RegisteredRendererStatePolicy : std::uint8_t { Reset, CarryByItemId };

/// One musical note emitted by a registered content compiler. Positions are
/// relative to the owning clip; ProgramCompiler performs the authoritative
/// tempo-map conversion and assigns collision-free generated identities.
struct ContentFragmentNote {
    timebase::TickPosition start;
    timebase::TickDuration duration;
    std::uint16_t velocity = 0xffff;
    std::uint8_t pitch = 60;
    std::uint8_t channel = 0;
    constexpr auto operator<=>(const ContentFragmentNote&) const = default;
};

enum class ContentFragmentErrorCode : std::uint8_t {
    RendererFailed,
    InvalidNote,
};

struct ContentFragmentError {
    ContentFragmentErrorCode code = ContentFragmentErrorCode::RendererFailed;
    std::size_t note_index = 0;
};

/// Owned immutable note output from one off-realtime compile-hook invocation.
class ContentProgramFragment {
  public:
    static runtime::Result<ContentProgramFragment, ContentFragmentError>
    create(std::vector<ContentFragmentNote> notes, timebase::TickDuration clip_duration) noexcept;

    std::span<const ContentFragmentNote> notes() const noexcept {
        return *notes_;
    }

  private:
    explicit ContentProgramFragment(
        std::shared_ptr<const std::vector<ContentFragmentNote>> notes) noexcept
        : notes_(std::move(notes)) {}
    std::shared_ptr<const std::vector<ContentFragmentNote>> notes_;
};

/// Bounded input presented to a trusted compiler hook on the compile worker.
/// The context view is narrowed to the registration's declared subscriptions.
struct RegisteredContentCompileInput {
    const timeline::RegisteredContent& content;
    timeline::ItemId clip_id;
    timebase::TickDuration clip_duration;
    timebase::TickPosition context_start;
    timeline::CompileContextView context;
    std::size_t maximum_fragment_notes = 0;
};

// A hook invocation is one trusted atomic compile work unit: it must not block
// or perform unbounded work, and it must honor maximum_fragment_notes. The
// compiler checks the returned size before admission; the registry-wide 4096
// ceiling bounds valid fragment construction even though arbitrary hook code
// cannot be preempted from inside this function-pointer ABI.
using RegisteredContentCompileFn =
    runtime::Result<ContentProgramFragment, ContentFragmentError> (*)(
        const RegisteredContentCompileInput& input, const void* context) noexcept;

/// One content renderer's registration, reduced to the part the dirty set
/// depends on: which content kind it renders, and which timeline context its
/// compile hook reads beyond that content.
///
/// The content kind is named by the schema type of the RegisteredContent it
/// compiles, because that is the identity the document itself carries. Built-in
/// MIDI declares its Groove dependency directly in the playback index; media
/// and empty content declare no context dependency.
struct ContentRendererRegistration {
    std::string content_type_name;
    timeline::CompileContextSubscriptions subscriptions;
    // Zero plus a null hook is the legacy subscription-only declaration. It is
    // intentionally unresolved at compile time, not silently rendered.
    std::uint32_t schema_version = 0;
    ContentProgramOutputKind output_kind = ContentProgramOutputKind::Notes;
    std::size_t maximum_fragment_notes = 0;
    RegisteredRendererStatePolicy state_policy = RegisteredRendererStatePolicy::Reset;
    timeline::ProductionDeclaration production;
    std::shared_ptr<const void> compile_context;
    RegisteredContentCompileFn compile = nullptr;
    // Filled by the validated declare overload. Dispatch compares this opaque
    // codec provenance with RegisteredContent before any value_as<T>() call.
    std::shared_ptr<const void> schema_registry_identity;
};

enum class ContextRegistrationErrorCode : std::uint8_t {
    EmptyContentTypeName,
    DuplicateContentType,
    CapacityExceeded,
    InvalidSchemaVersion,
    SchemaUnavailable,
    SchemaCodecUnavailable,
    InvalidFragmentQuota,
    InvalidProductionDeclaration,
    MissingCompileHook,
    InvalidOutputKind,
    InvalidStatePolicy,
};

struct ContextRegistrationError {
    ContextRegistrationErrorCode code = ContextRegistrationErrorCode::EmptyContentTypeName;
    std::string content_type_name;
};

/// The declaration side of the compile-context subscription contract.
///
/// A registry is mutated only on the control thread. Each compile invalidation
/// input takes an immutable declaration snapshot, so later control-thread
/// changes apply to the next request and never race a worker. Registration is
/// refused rather than overwritten on a
/// duplicate type name: two renderers disagreeing about what a content kind
/// reads is a configuration bug, and silently keeping one of them would make
/// the invalidation depend on registration order.
class CompileContextRegistry {
  public:
    static constexpr std::size_t kMaxRegistrations = 1024;
    static constexpr std::size_t kMaximumFragmentNotesPerClip = 4096;

    CompileContextRegistry();
    CompileContextRegistry(const CompileContextRegistry& other);
    CompileContextRegistry(CompileContextRegistry&& other);
    CompileContextRegistry& operator=(const CompileContextRegistry& other);
    CompileContextRegistry& operator=(CompileContextRegistry&& other);

    /// Returns the refusal, or nothing when the registration was accepted.
    std::optional<ContextRegistrationError> declare(ContentRendererRegistration registration);

    /// Declares a trusted compiler after proving its exact Content schema and
    /// codec exist in the immutable schema registry. Compiler registrations
    /// cannot enter through the subscription-only overload above.
    std::optional<ContextRegistrationError> declare(ContentRendererRegistration registration,
                                                    const timeline::SchemaRegistry& schemas);

    /// What the renderer for `content_type_name` declared. An unregistered type
    /// reads nothing: no renderer compiles it, so there is no program that
    /// could go stale.
    timeline::CompileContextSubscriptions
    subscriptions_for(std::string_view content_type_name) const noexcept;

    /// Finds the exact schema-version registration. The returned pointer is
    /// registry-owned and remains valid only until the next mutation; compile
    /// callers hold an immutable copied registry snapshot for the full hook.
    const ContentRendererRegistration* find(const timeline::SchemaIdentity& schema) const noexcept;
    const ContentRendererRegistration*
    find(const timeline::RegisteredContent& content) const noexcept;

    std::size_t size() const noexcept {
        return registrations_.size();
    }

    /// Monotonic version within this registry generation's declarations.
    ///
    /// A compiler pairs this with the opaque generation identity. Ordinary
    /// mutable copies and assignments begin a new generation even when they
    /// contain the same declarations, so replacement cannot alias an older
    /// revision number. Internal immutable dispatch snapshots retain their
    /// captured source watermark and are safe to resubmit unchanged.
    std::uint64_t revision() const noexcept;
    std::shared_ptr<const void> generation_identity() const noexcept {
        return snapshot_generation_ ? snapshot_generation_ : generation_;
    }

  private:
    friend class CompileInvalidationIndex;
    friend class PlaybackProgramCompiler;
    friend struct CompileInvalidationInput;

    /// Copies declarations while retaining the exact source watermark. The
    /// returned const owner is safe to dispatch or submit again; unlike an
    /// ordinary mutable registry copy, it must not mint a replacement identity.
    std::shared_ptr<const CompileContextRegistry> immutable_snapshot() const;

    // Sorted by content_type_name so lookup is a binary search on a hot path.
    std::vector<ContentRendererRegistration> registrations_;
    std::shared_ptr<detail::ContextRegistryGeneration> generation_;
    std::shared_ptr<const detail::ContextRegistryGeneration> snapshot_generation_;
    std::optional<std::uint64_t> snapshot_revision_;
};

/// Bundled reverse index for nested dependencies and compile-context readers.
///
/// Built atomically from one pinned snapshot, root, and registry, it turns "the
/// chord lane changed" into an exact track set instead of a full recompile.
/// Project structure identity and registry generation are checked in O(1);
/// stale or mismatched indices fail closed. A context edit alone does not
/// invalidate the index because editing a lane's contents does not change who
/// reads it.
/// Arrangement declarations remain indexed while a freeze or take lane is
/// selected. Resolution filters them against the current snapshot, which keeps
/// the index valid when playback later returns to the arrangement.
class CompileInvalidationIndex {
  public:
    static CompileInvalidationIndex build(const timeline::Project& project,
                                          timeline::ItemId sequence_id,
                                          const CompileContextRegistry& registry);

    /// Sorted, deduplicated track ids that declared a read of `kind`.
    /// This aggregate query includes declarations reached through references.
    std::span<const timeline::ItemId> subscribers(timeline::CompileContextKind kind) const noexcept;

    /// Sorted, deduplicated root track ids that read `kind` from
    /// `owner_sequence`.
    std::span<const timeline::ItemId> subscribers(timeline::ItemId owner_sequence,
                                                  timeline::CompileContextKind kind) const noexcept;

    bool empty() const noexcept;

    bool valid() const noexcept;
    bool matches(const timeline::Project& project,
                 timeline::ItemId root_sequence_id) const noexcept;

  private:
    friend DirtyTrackSet resolve_dirty_tracks(const timeline::Project&, timeline::ItemId,
                                              const timeline::DirtySet&,
                                              const CompileInvalidationIndex&);

    std::shared_ptr<const detail::CompileInvalidationData> data_;
};

} // namespace pulp::playback
