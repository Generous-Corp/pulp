#pragma once

#include <pulp/playback/dirty_track_resolver.hpp>
#include <pulp/timeline/compile_context.hpp>
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

/// One content renderer's registration, reduced to the part the dirty set
/// depends on: which content kind it renders, and which timeline context its
/// compile hook reads beyond that content.
///
/// The content kind is named by the schema type of the RegisteredContent it
/// compiles, because that is the identity the document itself carries. Built-in
/// content (media, notes) is not registered here and therefore declares no
/// context, which is exactly true of the built-in renderers today.
struct ContentRendererRegistration {
    std::string content_type_name;
    timeline::CompileContextSubscriptions subscriptions;
};

enum class ContextRegistrationErrorCode : std::uint8_t {
    EmptyContentTypeName,
    DuplicateContentType,
    CapacityExceeded,
};

struct ContextRegistrationError {
    ContextRegistrationErrorCode code = ContextRegistrationErrorCode::EmptyContentTypeName;
    std::string content_type_name;
};

/// The declaration side of the compile-context subscription contract.
///
/// A registry is built on the control thread before compiles are submitted and
/// then only read. Registration is refused rather than overwritten on a
/// duplicate type name: two renderers disagreeing about what a content kind
/// reads is a configuration bug, and silently keeping one of them would make
/// the invalidation depend on registration order.
class CompileContextRegistry {
  public:
    static constexpr std::size_t kMaxRegistrations = 1024;

    CompileContextRegistry();
    CompileContextRegistry(const CompileContextRegistry& other);
    CompileContextRegistry(CompileContextRegistry&& other);
    CompileContextRegistry& operator=(const CompileContextRegistry& other);
    CompileContextRegistry& operator=(CompileContextRegistry&& other);

    /// Returns the refusal, or nothing when the registration was accepted.
    std::optional<ContextRegistrationError> declare(ContentRendererRegistration registration);

    /// What the renderer for `content_type_name` declared. An unregistered type
    /// reads nothing: no renderer compiles it, so there is no program that
    /// could go stale.
    timeline::CompileContextSubscriptions
    subscriptions_for(std::string_view content_type_name) const noexcept;

    std::size_t size() const noexcept {
        return registrations_.size();
    }

  private:
    friend class CompileInvalidationIndex;

    // Sorted by content_type_name so lookup is a binary search on a hot path.
    std::vector<ContentRendererRegistration> registrations_;
    std::shared_ptr<detail::ContextRegistryGeneration> generation_;
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
