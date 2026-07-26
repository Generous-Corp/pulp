#include <pulp/playback/compile_context_registry.hpp>

#include <algorithm>
#include <iterator>
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

struct OwnedContextSubscription {
    timeline::ItemId owner_sequence;
    timeline::CompileContextKind kind;

    constexpr auto operator<=>(const OwnedContextSubscription&) const = default;
};

void append_subscriptions(std::vector<OwnedContextSubscription>& destination,
                          timeline::ItemId owner_sequence,
                          timeline::CompileContextSubscriptions source) {
    for (std::size_t kind = 0; kind < timeline::kCompileContextKindCount; ++kind) {
        const auto context = static_cast<timeline::CompileContextKind>(kind);
        const OwnedContextSubscription subscription{owner_sequence, context};
        if (source.reads(context) &&
            std::find(destination.begin(), destination.end(), subscription) ==
                destination.end())
            destination.push_back(subscription);
    }
}

void merge_subscriptions(std::vector<OwnedContextSubscription>& destination,
                         const std::vector<OwnedContextSubscription>& source) {
    for (const auto& subscription : source) {
        if (std::find(destination.begin(), destination.end(), subscription) ==
            destination.end())
            destination.push_back(subscription);
    }
}

class ReferencedSequenceSubscriptionCache {
  public:
    ReferencedSequenceSubscriptionCache(const timeline::Project& project,
                                        const CompileContextRegistry& registry)
        : project_(project), registry_(registry), cached_(project.sequences().size()) {}

    const std::vector<OwnedContextSubscription>&
    subscriptions_for(timeline::ItemId sequence_id) {
        static const std::vector<OwnedContextSubscription> empty;
        const auto* sequence = project_.find_sequence(sequence_id);
        if (!sequence)
            return empty;
        const auto index =
            static_cast<std::size_t>(sequence - project_.sequences().data());
        if (cached_[index])
            return *cached_[index];

        std::vector<OwnedContextSubscription> result;
        for (const timeline::Track& track : sequence->tracks()) {
            for (const timeline::Clip& clip : track.clips()) {
                std::visit(timeline::ClipContentCases{
                               [](const timeline::EmptyContent&) {},
                               [](const timeline::MediaRef&) {},
                               [](const timeline::NoteContent&) {},
                               [&](const timeline::RegisteredContent& content) {
                                   append_subscriptions(
                                       result, sequence_id,
                                       registry_.subscriptions_for(
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
        return *cached_[index];
    }

  private:
    const timeline::Project& project_;
    const CompileContextRegistry& registry_;
    std::vector<std::optional<std::vector<OwnedContextSubscription>>> cached_;
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
                        return std::vector<OwnedContextSubscription>{};
                    },
                    [](const timeline::MediaRef&) {
                        return std::vector<OwnedContextSubscription>{};
                    },
                    [](const timeline::NoteContent&) {
                        return std::vector<OwnedContextSubscription>{};
                    },
                    [&](const timeline::RegisteredContent& content) {
                        std::vector<OwnedContextSubscription> result;
                        append_subscriptions(
                            result, sequence_id,
                            registry.subscriptions_for(content.schema().type_name));
                        return result;
                    },
                    // Opaque content is content no renderer claimed. Once one
                    // does, the same bytes decode as RegisteredContent and take
                    // the branch above; until then there is no program to
                    // invalidate, so it subscribes to nothing.
                    [](const timeline::OpaqueContent&) {
                        return std::vector<OwnedContextSubscription>{};
                    },
                    [&](const timeline::SequenceRef& reference) {
                        return std::vector<OwnedContextSubscription>(
                            referenced_subscriptions
                                .subscriptions_for(reference.sequence_id));
                    },
                },
                clip.content());
            if (subscriptions.empty())
                continue;
            for (const auto& subscription : subscriptions) {
                const auto kind = kind_index(subscription.kind);
                index.by_kind_[kind].push_back(track.id());
                auto owner = std::find_if(
                    index.by_owner_sequence_.begin(),
                    index.by_owner_sequence_.end(),
                    [&](const ContextSubscriberIndex::OwnedSubscribers& entry) {
                        return entry.owner_sequence == subscription.owner_sequence;
                    });
                if (owner == index.by_owner_sequence_.end()) {
                    index.by_owner_sequence_.push_back(
                        {.owner_sequence = subscription.owner_sequence});
                    owner = std::prev(index.by_owner_sequence_.end());
                }
                owner->by_kind[kind].push_back(track.id());
            }
        }
    }
    for (auto& subscribers : index.by_kind_) {
        std::sort(subscribers.begin(), subscribers.end());
        subscribers.erase(std::unique(subscribers.begin(), subscribers.end()), subscribers.end());
    }
    for (auto& owner : index.by_owner_sequence_) {
        for (auto& subscribers : owner.by_kind) {
            std::sort(subscribers.begin(), subscribers.end());
            subscribers.erase(std::unique(subscribers.begin(), subscribers.end()),
                              subscribers.end());
        }
    }
    return index;
}

std::span<const timeline::ItemId>
ContextSubscriberIndex::subscribers(timeline::CompileContextKind kind) const noexcept {
    return by_kind_[kind_index(kind)];
}

std::span<const timeline::ItemId>
ContextSubscriberIndex::subscribers(timeline::ItemId owner_sequence,
                                    timeline::CompileContextKind kind) const noexcept {
    const auto owner =
        std::find_if(by_owner_sequence_.begin(), by_owner_sequence_.end(),
                     [&](const OwnedSubscribers& entry) {
                         return entry.owner_sequence == owner_sequence;
                     });
    return owner != by_owner_sequence_.end()
               ? std::span<const timeline::ItemId>(owner->by_kind[kind_index(kind)])
               : std::span<const timeline::ItemId>{};
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
        const auto* sequence = project.find_sequence(sequence_id);
        const auto subscribers =
            index.subscribers(context.owner_sequence, context.kind);
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
