#include <pulp/timeline/note_transform.hpp>

#include <algorithm>
#include <utility>

namespace pulp::timeline {

namespace {

NoteTransformError fail(NoteTransformErrorCode code, const ApplyNoteTransform& request,
                        ItemId item = {}) {
    return NoteTransformError{.code = code, .transform = request.transform, .item = item};
}

bool registration_less(const NoteTransformRegistration& registration,
                       const SchemaIdentity& identity) {
    return registration.identity < identity;
}

} // namespace

std::optional<NoteTransformError>
NoteTransformRegistry::register_transform(NoteTransformRegistration registration) {
    if (!registration.identity.valid() || registration.function == nullptr)
        return NoteTransformError{.code = NoteTransformErrorCode::InvalidRegistration,
                                  .transform = std::move(registration.identity)};
    const auto found = std::lower_bound(registrations_.begin(), registrations_.end(),
                                        registration.identity, registration_less);
    if (found != registrations_.end() && found->identity == registration.identity)
        return NoteTransformError{.code = NoteTransformErrorCode::DuplicateRegistration,
                                  .transform = std::move(registration.identity)};
    if (registrations_.size() >= kMaxRegistrations)
        return NoteTransformError{.code = NoteTransformErrorCode::CapacityExceeded,
                                  .transform = std::move(registration.identity)};
    registrations_.insert(found, std::move(registration));
    return std::nullopt;
}

runtime::Result<NoteTransformPreview, NoteTransformError>
NoteTransformRegistry::preview(const DocumentView& view, WriterToken& writer,
                               const ApplyNoteTransform& request) const {
    if (!view.snapshot)
        return runtime::Err(fail(NoteTransformErrorCode::InvalidView, request, request.clip_id));
    const auto& project = *view.snapshot;
    const auto registration = std::lower_bound(registrations_.begin(), registrations_.end(),
                                               request.transform, registration_less);
    if (registration == registrations_.end() || registration->identity != request.transform)
        return runtime::Err(
            fail(NoteTransformErrorCode::TransformNotFound, request, request.clip_id));

    const auto* sequence = project.find_sequence(request.sequence_id);
    const auto* track = sequence ? sequence->find_track(request.track_id) : nullptr;
    const auto* clip = track ? track->find_clip(request.clip_id) : nullptr;
    if (!clip)
        return runtime::Err(fail(NoteTransformErrorCode::TargetMissing, request, request.clip_id));
    const auto* note_content = std::get_if<NoteContent>(&clip->content());
    if (!note_content)
        return runtime::Err(
            fail(NoteTransformErrorCode::WrongTargetKind, request, request.clip_id));

    DecodeLimits parameter_limits;
    parameter_limits.max_input_bytes = kMaxParameterBytes;
    parameter_limits.max_string_bytes = kMaxParameterBytes;
    auto parsed_parameters = parse_json(request.canonical_params_json, parameter_limits);
    if (!parsed_parameters || parsed_parameters.value()->root().kind != JsonValue::Kind::Object) {
        auto error = fail(NoteTransformErrorCode::InvalidParameters, request, request.clip_id);
        error.persistence_error = parsed_parameters
                                      ? PersistenceError{PersistenceErrorCode::UnexpectedType,
                                                         parsed_parameters.value()->root().begin}
                                      : parsed_parameters.error();
        return runtime::Err(std::move(error));
    }
    auto canonical_parameters = canonicalize_json(parsed_parameters.value()->root());
    if (!canonical_parameters) {
        auto error = fail(NoteTransformErrorCode::InvalidParameters, request, request.clip_id);
        error.persistence_error = canonical_parameters.error();
        return runtime::Err(std::move(error));
    }

    auto transformed = registration->function(note_content->notes(), canonical_parameters.value(),
                                              request.seed, registration->user_data.get());
    if (!transformed) {
        auto error = fail(NoteTransformErrorCode::CallbackFailed, request, request.clip_id);
        error.callback_error = transformed.error();
        return runtime::Err(std::move(error));
    }

    auto output = std::move(transformed).value();
    // ReplaceNoteContent persists both the expected and replacement note
    // arrays through one DecodeLimits::max_notes budget. Keep previews inside
    // that durable command envelope rather than accepting a transaction that
    // cannot be read back from the journal.
    const auto input_note_count = note_content->notes().size();
    if (input_note_count > kMaxDurableCommandNotes ||
        output.size() > kMaxDurableCommandNotes - input_note_count)
        return runtime::Err(
            fail(NoteTransformErrorCode::OutputLimitExceeded, request, request.clip_id));
    std::vector<ItemId> input_ids;
    input_ids.reserve(note_content->notes().size());
    for (const auto& note : note_content->notes())
        input_ids.push_back(note.id);
    std::sort(input_ids.begin(), input_ids.end());

    auto allocator = project.item_id_allocator();
    for (auto& note : output) {
        if (!note.id.valid()) {
            auto allocated = allocator.allocate();
            if (!allocated) {
                auto error =
                    fail(NoteTransformErrorCode::ItemIdExhausted, request, request.clip_id);
                error.model_error = allocated.error();
                return runtime::Err(std::move(error));
            }
            note.id = allocated.value();
        } else if (!std::binary_search(input_ids.begin(), input_ids.end(), note.id)) {
            return runtime::Err(
                fail(NoteTransformErrorCode::ForeignOutputIdentity, request, note.id));
        }
    }

    std::vector<ItemId> output_ids;
    output_ids.reserve(output.size());
    for (const auto& note : output)
        output_ids.push_back(note.id);
    std::sort(output_ids.begin(), output_ids.end());
    const auto duplicate = std::adjacent_find(output_ids.begin(), output_ids.end());
    if (duplicate != output_ids.end())
        return runtime::Err(
            fail(NoteTransformErrorCode::DuplicateOutputIdentity, request, *duplicate));

    auto canonical_output = NoteContent::create(std::move(output));
    if (!canonical_output) {
        auto error = fail(NoteTransformErrorCode::InvalidOutput, request, request.clip_id);
        error.model_error = canonical_output.error();
        return runtime::Err(std::move(error));
    }

    Transaction transaction{
        .id = writer.allocate_transaction_id(),
        .expected_revision = view.revision,
        .commands = {{writer.allocate_command_id(),
                      ReplaceNoteContent{request.sequence_id, request.track_id, request.clip_id,
                                         std::vector<NoteEvent>(note_content->notes().begin(),
                                                                note_content->notes().end()),
                                         std::vector<NoteEvent>(canonical_output->notes().begin(),
                                                                canonical_output->notes().end())}}},
    };
    auto reduced = reduce_transaction(project, transaction);
    if (!reduced) {
        auto error = fail(NoteTransformErrorCode::TransactionRejected, request, request.clip_id);
        error.transaction_error = reduced.error();
        return runtime::Err(std::move(error));
    }
    auto reduced_value = std::move(reduced).value();
    return runtime::Ok(NoteTransformPreview{
        std::move(transaction), std::move(reduced_value.project), std::move(reduced_value.dirty)});
}

} // namespace pulp::timeline
