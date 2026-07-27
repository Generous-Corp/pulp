#pragma once

#include <pulp/timeline/document_session.hpp>
#include <pulp/timeline/schema_json.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::timeline {

enum class NoteTransformCallbackError : std::uint8_t {
    InvalidParameters,
    Refused,
};

struct NoteTransformCallbackFailure {
    NoteTransformCallbackError code = NoteTransformCallbackError::Refused;
};

using NoteTransformFunction =
    runtime::Result<std::vector<NoteEvent>, NoteTransformCallbackFailure> (*)(
        std::span<const NoteEvent> notes, std::string_view canonical_params_json,
        std::uint64_t seed, const void* user_data) noexcept;

struct NoteTransformRegistration {
    SchemaIdentity identity;
    NoteTransformFunction function = nullptr;
    std::shared_ptr<const void> user_data;
};

enum class NoteTransformErrorCode : std::uint8_t {
    InvalidRegistration,
    DuplicateRegistration,
    CapacityExceeded,
    TransformNotFound,
    InvalidView,
    InvalidParameters,
    TargetMissing,
    WrongTargetKind,
    CallbackFailed,
    InvalidOutput,
    ForeignOutputIdentity,
    DuplicateOutputIdentity,
    OutputLimitExceeded,
    ItemIdExhausted,
    TransactionRejected,
};

struct NoteTransformError {
    NoteTransformErrorCode code = NoteTransformErrorCode::InvalidRegistration;
    SchemaIdentity transform;
    ItemId item;
    std::optional<NoteTransformCallbackFailure> callback_error;
    std::optional<PersistenceError> persistence_error;
    std::optional<ModelError> model_error;
    std::optional<TransactionError> transaction_error;
};

/// A typed, apply-time request. It is intentionally not a journal Command:
/// preparing it invokes extension code once and lowers the result to an
/// ordinary ReplaceNoteContent command.
struct ApplyNoteTransform {
    ItemId sequence_id;
    ItemId track_id;
    ItemId clip_id;
    SchemaIdentity transform;
    std::string canonical_params_json;
    std::uint64_t seed = 0;
};

struct NoteTransformPreview {
    Transaction transaction;
    Project snapshot;
    DirtySet dirty;
};

/// Control-thread registry for pure note transforms.
///
/// Registrations are immutable by identity once admitted. A transform may
/// preserve an input note's ItemId by returning it. An output with an invalid
/// ItemId is assigned a fresh project identity; any other identity is refused.
class NoteTransformRegistry {
  public:
    static constexpr std::size_t kMaxRegistrations = 1024;
    static constexpr std::size_t kMaxParameterBytes = 1024 * 1024;
    // ReplaceNoteContent persists both the input and output arrays under this
    // shared note quota.
    static constexpr std::size_t kMaxDurableCommandNotes = DecodeLimits{}.max_notes;

    std::optional<NoteTransformError> register_transform(NoteTransformRegistration registration);

    runtime::Result<NoteTransformPreview, NoteTransformError>
    preview(const DocumentView& view, WriterToken& writer, const ApplyNoteTransform& request) const;

    std::size_t size() const noexcept {
        return registrations_.size();
    }

  private:
    std::vector<NoteTransformRegistration> registrations_;
};

} // namespace pulp::timeline
