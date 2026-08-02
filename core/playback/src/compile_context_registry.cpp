#include <pulp/playback/compile_context_registry.hpp>

#include "compile_invalidation_internal.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <span>
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

timeline::CompileContextSubscriptions
subscriptions_for_content(const timeline::ClipContent& content,
                          const CompileContextRegistry& registry) noexcept {
    return std::visit(
        timeline::ClipContentCases{
            [](const timeline::EmptyContent&) {
                return timeline::CompileContextSubscriptions::none();
            },
            [](const timeline::MediaRef&) { return timeline::CompileContextSubscriptions::none(); },
            [](const timeline::MidiContent&) {
                auto subscriptions = timeline::CompileContextSubscriptions::none();
                subscriptions.subscribe(timeline::CompileContextKind::Groove);
                return subscriptions;
            },
            [&](const timeline::RegisteredContent& registered) {
                return registry.subscriptions_for(registered.schema().type_name);
            },
            [](const timeline::OpaqueContent&) {
                return timeline::CompileContextSubscriptions::none();
            },
            [](const timeline::SequenceRef&) {
                return timeline::CompileContextSubscriptions::none();
            },
        },
        content);
}

} // namespace

CompileContextRegistry::CompileContextRegistry()
    : generation_(std::make_shared<detail::ContextRegistryGeneration>()) {}

CompileContextRegistry::CompileContextRegistry(const CompileContextRegistry& other)
    : registrations_(other.registrations_),
      generation_(std::make_shared<detail::ContextRegistryGeneration>()) {}

CompileContextRegistry::CompileContextRegistry(CompileContextRegistry&& other)
    : CompileContextRegistry() {
    registrations_ = std::move(other.registrations_);
    other.registrations_.clear();
    ++other.generation_->revision;
}

CompileContextRegistry& CompileContextRegistry::operator=(const CompileContextRegistry& other) {
    if (this == &other)
        return *this;
    auto replacement = other.registrations_;
    auto new_generation = std::make_shared<detail::ContextRegistryGeneration>();
    ++generation_->revision;
    registrations_ = std::move(replacement);
    generation_ = std::move(new_generation);
    return *this;
}

CompileContextRegistry& CompileContextRegistry::operator=(CompileContextRegistry&& other) {
    if (this == &other)
        return *this;
    auto new_generation = std::make_shared<detail::ContextRegistryGeneration>();
    ++generation_->revision;
    registrations_ = std::move(other.registrations_);
    other.registrations_.clear();
    ++other.generation_->revision;
    generation_ = std::move(new_generation);
    return *this;
}

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
    ++generation_->revision;
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

std::uint64_t CompileContextRegistry::revision() const noexcept {
    return generation_->revision;
}

CompileInvalidationIndex CompileInvalidationIndex::build(const timeline::Project& project,
                                                         timeline::ItemId sequence_id,
                                                         const CompileContextRegistry& registry) {
    CompileInvalidationIndex index;
    const auto* sequence = project.find_sequence(sequence_id);
    auto data = detail::build_sequence_dependencies(project, sequence_id);
    if (!sequence || !data)
        return index;
    data->registry_generation = registry.generation_;
    data->registry_revision = registry.generation_->revision;

    const auto append = [&](timeline::ItemId owner_sequence,
                            timeline::CompileContextSubscriptions subscriptions,
                            std::span<const timeline::ItemId> root_tracks) {
        auto owner = std::find_if(data->context_by_owner.begin(), data->context_by_owner.end(),
                                  [&](const detail::OwnedContextSubscribers& entry) {
                                      return entry.owner_sequence == owner_sequence;
                                  });
        if (owner == data->context_by_owner.end()) {
            data->context_by_owner.push_back({.owner_sequence = owner_sequence});
            owner = std::prev(data->context_by_owner.end());
        }
        for (std::size_t kind = 0; kind < timeline::kCompileContextKindCount; ++kind) {
            const auto context = static_cast<timeline::CompileContextKind>(kind);
            if (!subscriptions.reads(context))
                continue;
            data->context_by_kind[kind].insert(data->context_by_kind[kind].end(),
                                               root_tracks.begin(), root_tracks.end());
            owner->by_kind[kind].insert(owner->by_kind[kind].end(), root_tracks.begin(),
                                        root_tracks.end());
        }
    };

    for (const auto& owner_sequence : project.sequences()) {
        const bool root_owner = owner_sequence.id() == sequence_id;
        const auto nested_tracks = data->root_tracks_for(owner_sequence.id());
        if (!root_owner && nested_tracks.empty())
            continue;
        for (const auto& track : owner_sequence.tracks()) {
            const std::array direct_track{track.id()};
            const auto root_tracks =
                root_owner ? std::span<const timeline::ItemId>(direct_track) : nested_tracks;
            for (const auto& clip : track.clips())
                append(owner_sequence.id(), subscriptions_for_content(clip.content(), registry),
                       root_tracks);
        }
    }
    for (auto& subscribers : data->context_by_kind) {
        std::sort(subscribers.begin(), subscribers.end());
        subscribers.erase(std::unique(subscribers.begin(), subscribers.end()), subscribers.end());
    }
    for (auto& owner : data->context_by_owner) {
        for (auto& subscribers : owner.by_kind) {
            std::sort(subscribers.begin(), subscribers.end());
            subscribers.erase(std::unique(subscribers.begin(), subscribers.end()),
                              subscribers.end());
        }
    }
    index.data_ = std::move(data);
    return index;
}

std::span<const timeline::ItemId>
CompileInvalidationIndex::subscribers(timeline::CompileContextKind kind) const noexcept {
    return data_ ? std::span<const timeline::ItemId>(data_->context_by_kind[kind_index(kind)])
                 : std::span<const timeline::ItemId>{};
}

std::span<const timeline::ItemId>
CompileInvalidationIndex::subscribers(timeline::ItemId owner_sequence,
                                      timeline::CompileContextKind kind) const noexcept {
    return data_ ? data_->subscribers(owner_sequence, kind) : std::span<const timeline::ItemId>{};
}

bool CompileInvalidationIndex::empty() const noexcept {
    if (!data_)
        return true;
    for (const auto& subscribers : data_->context_by_kind) {
        if (!subscribers.empty())
            return false;
    }
    return true;
}

bool CompileInvalidationIndex::valid() const noexcept {
    return data_ && data_->valid();
}

bool CompileInvalidationIndex::matches(const timeline::Project& project,
                                       timeline::ItemId root_sequence_id) const noexcept {
    return data_ && data_->matches(project, root_sequence_id);
}

std::span<const timeline::ItemId>
detail::CompileInvalidationData::subscribers(timeline::ItemId owner_sequence,
                                             timeline::CompileContextKind kind) const noexcept {
    const auto owner = std::find_if(context_by_owner.begin(), context_by_owner.end(),
                                    [&](const OwnedContextSubscribers& entry) {
                                        return entry.owner_sequence == owner_sequence;
                                    });
    return owner != context_by_owner.end()
               ? std::span<const timeline::ItemId>(owner->by_kind[static_cast<std::size_t>(kind)])
               : std::span<const timeline::ItemId>{};
}

} // namespace pulp::playback
