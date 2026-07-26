#include <pulp/playback/compile_context_registry.hpp>

#include <algorithm>
#include <optional>
#include <variant>
#include <vector>

namespace pulp::playback {
namespace {

bool by_type_name(const ContentRendererRegistration& registration,
                  std::string_view wanted) noexcept {
    return registration.content_type_name < wanted;
}

std::size_t kind_index(timeline::CompileContextKind kind) noexcept {
    return static_cast<std::size_t>(kind);
}

void merge_subscriptions(timeline::CompileContextSubscriptions& destination,
                         timeline::CompileContextSubscriptions source) noexcept {
    for (std::size_t kind = 0; kind < timeline::kCompileContextKindCount; ++kind) {
        const auto context = static_cast<timeline::CompileContextKind>(kind);
        if (source.reads(context))
            destination.subscribe(context);
    }
}

class ReferencedSequenceSubscriptionCache {
  public:
    ReferencedSequenceSubscriptionCache(const timeline::Project& project,
                                        const CompileContextRegistry& registry)
        : project_(project), registry_(registry), cached_(project.sequences().size()) {}

    timeline::CompileContextSubscriptions subscriptions_for(timeline::ItemId sequence_id) {
        const auto* sequence = project_.find_sequence(sequence_id);
        if (!sequence)
            return timeline::CompileContextSubscriptions::none();
        const auto index =
            static_cast<std::size_t>(sequence - project_.sequences().data());
        if (cached_[index])
            return *cached_[index];

        auto result = timeline::CompileContextSubscriptions::none();
        for (const timeline::Track& track : sequence->tracks()) {
            for (const timeline::Clip& clip : track.clips()) {
                std::visit(timeline::ClipContentCases{
                               [](const timeline::EmptyContent&) {},
                               [](const timeline::MediaRef&) {},
                               [](const timeline::NoteContent&) {},
                               [&](const timeline::RegisteredContent& content) {
                                   merge_subscriptions(
                                       result, registry_.subscriptions_for(
                                                   content.schema().type_name));
                               },
                               [](const timeline::OpaqueContent&) {},
                               [&](const timeline::SequenceRef& reference) {
                                   merge_subscriptions(
                                       result, subscriptions_for(reference.sequence_id));
                               },
                           },
                           clip.content());
            }
        }
        cached_[index] = result;
        return result;
    }

  private:
    const timeline::Project& project_;
    const CompileContextRegistry& registry_;
    std::vector<std::optional<timeline::CompileContextSubscriptions>> cached_;
};

} // namespace

std::optional<ContextRegistrationError>
CompileContextRegistry::declare(ContentRendererRegistration registration) {
    if (registration.content_type_name.empty())
        return ContextRegistrationError{ContextRegistrationErrorCode::EmptyContentTypeName,
                                        std::move(registration.content_type_name)};
    if (registrations_.size() >= kMaxRegistrations)
        return ContextRegistrationError{ContextRegistrationErrorCode::CapacityExceeded,
                                        std::move(registration.content_type_name)};
    const auto position =
        std::lower_bound(registrations_.begin(), registrations_.end(),
                         std::string_view(registration.content_type_name), by_type_name);
    if (position != registrations_.end() &&
        position->content_type_name == registration.content_type_name)
        return ContextRegistrationError{ContextRegistrationErrorCode::DuplicateContentType,
                                        std::move(registration.content_type_name)};
    registrations_.insert(position, std::move(registration));
    return std::nullopt;
}

timeline::CompileContextSubscriptions
CompileContextRegistry::subscriptions_for(std::string_view content_type_name) const noexcept {
    const auto found = std::lower_bound(registrations_.begin(), registrations_.end(),
                                        content_type_name, by_type_name);
    return found != registrations_.end() && found->content_type_name == content_type_name
               ? found->subscriptions
               : timeline::CompileContextSubscriptions::none();
}

ContextSubscriberIndex ContextSubscriberIndex::build(const timeline::Project& project,
                                                     timeline::ItemId sequence_id,
                                                     const CompileContextRegistry& registry) {
    ContextSubscriberIndex index;
    const auto* sequence = project.find_sequence(sequence_id);
    if (!sequence)
        return index;
    ReferencedSequenceSubscriptionCache referenced_subscriptions(project, registry);
    for (const timeline::Track& track : sequence->tracks()) {
        for (const timeline::Clip& clip : track.clips()) {
            const auto subscriptions = std::visit(
                timeline::ClipContentCases{
                    // Built-in content is rendered by built-in renderers, none
                    // of which read anything outside their own clip.
                    [](const timeline::EmptyContent&) {
                        return timeline::CompileContextSubscriptions::none();
                    },
                    [](const timeline::MediaRef&) {
                        return timeline::CompileContextSubscriptions::none();
                    },
                    [](const timeline::NoteContent&) {
                        return timeline::CompileContextSubscriptions::none();
                    },
                    [&](const timeline::RegisteredContent& content) {
                        return registry.subscriptions_for(content.schema().type_name);
                    },
                    // Opaque content is content no renderer claimed. Once one
                    // does, the same bytes decode as RegisteredContent and take
                    // the branch above; until then there is no program to
                    // invalidate, so it subscribes to nothing.
                    [](const timeline::OpaqueContent&) {
                        return timeline::CompileContextSubscriptions::none();
                    },
                    [&](const timeline::SequenceRef& reference) {
                        return referenced_subscriptions.subscriptions_for(
                            reference.sequence_id);
                    },
                },
                clip.content());
            if (!subscriptions.any())
                continue;
            for (std::size_t kind = 0; kind < timeline::kCompileContextKindCount; ++kind) {
                if (subscriptions.reads(static_cast<timeline::CompileContextKind>(kind)))
                    index.by_kind_[kind].push_back(track.id());
            }
        }
    }
    for (auto& subscribers : index.by_kind_) {
        std::sort(subscribers.begin(), subscribers.end());
        subscribers.erase(std::unique(subscribers.begin(), subscribers.end()), subscribers.end());
    }
    return index;
}

std::span<const timeline::ItemId>
ContextSubscriberIndex::subscribers(timeline::CompileContextKind kind) const noexcept {
    return by_kind_[kind_index(kind)];
}

bool ContextSubscriberIndex::empty() const noexcept {
    for (const auto& subscribers : by_kind_) {
        if (!subscribers.empty())
            return false;
    }
    return true;
}

DirtyTrackSet resolve_dirty_tracks(const timeline::Project& project,
                                   timeline::ItemId sequence_id, const timeline::DirtySet& dirty,
                                   const ContextSubscriberIndex& index) {
    DirtyTrackSet result;
    for (const timeline::DirtyItem& item : dirty.items()) {
        const auto flags = static_cast<std::uint16_t>(item.flags);
        if (!item.owner_sequence.valid()) {
            // Project-scoped: tempo, meter, or the asset table. Every track's
            // program is derived from those, so none of them can be reused.
            result.all = true;
            continue;
        }
        if (item.owner_sequence != sequence_id)
            continue;
        if (item.owner_track.valid()) {
            result.tracks.push_back(item.owner_track);
            continue;
        }
        // Context companions name no track on purpose: their readers come from
        // the reverse index. Markers are sequence metadata and do not
        // contribute to any compiled track program. Any other trackless item
        // is a structural edit to the sequence itself.
        const auto ignored = static_cast<std::uint16_t>(timeline::DirtyFlags::Context) |
                             static_cast<std::uint16_t>(timeline::DirtyFlags::Marker);
        if ((flags & ignored) == 0)
            result.all = true;
    }
    for (const timeline::DirtyContext& context : dirty.contexts()) {
        if (context.owner_sequence != sequence_id)
            continue;
        const auto* sequence = project.find_sequence(sequence_id);
        const auto subscribers = index.subscribers(context.kind);
        for (const auto track_id : subscribers) {
            const auto* track = sequence ? sequence->find_track(track_id) : nullptr;
            // A freeze or selected take comp replaces the arrangement
            // wholesale, so its arrangement renderer is not live in this
            // snapshot. The declaration remains indexed so returning to the
            // arrangement cannot make the index stale.
            if (track && !track->freeze().has_value() &&
                !track->active_take_lane_id().valid())
                result.tracks.push_back(track_id);
        }
    }
    std::sort(result.tracks.begin(), result.tracks.end());
    result.tracks.erase(std::unique(result.tracks.begin(), result.tracks.end()),
                        result.tracks.end());
    return result;
}

} // namespace pulp::playback
