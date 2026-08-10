#include <pulp/inspect/control_telemetry_tap.hpp>

#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace pulp::inspect {
namespace {

std::string subscription_id() {
    const auto bytes = runtime::secure_random_bytes(16);
    return bytes ? "telemetry-" + runtime::hex_encode(*bytes) : std::string{};
}

bool same_authority(const ControlTelemetryAuthority& left, const ControlTelemetryAuthority& right) {
    return !left.client_id.empty() && left.client_id == right.client_id &&
           left.registration_id == right.registration_id && left.instance_id == right.instance_id &&
           left.grant_id == right.grant_id;
}

} // namespace

struct ControlTelemetryTap::Impl {
    struct Channel {
        std::size_t index = 0;
        view::ValueChannelTelemetryDescriptor descriptor;
        view::ValueChannelTelemetrySnapshot continuous;
        std::vector<view::ValueChannelTelemetryEventBatch> events;
        std::uint64_t event_overflow = 0;
    };
    struct Subscription {
        struct PendingEvents {
            std::deque<float> values;
            std::uint64_t latest_publication = 0;
            std::uint64_t dropped = 0;
            std::uint64_t observed_overflow = 0;
        };
        std::string id;
        ControlTelemetryAuthority authority;
        std::vector<std::string> names;
        std::size_t maximum_vector_values = 0;
        std::chrono::steady_clock::duration period{};
        std::chrono::steady_clock::time_point next_due{};
        std::uint64_t sequence = 0;
        std::uint64_t loss_debt = 0;
        std::deque<ControlTelemetryFrame> queued;
        std::map<std::string, PendingEvents, std::less<>> pending_events;
    };

    ControlTelemetryTapConfig config;
    Clock clock;
    Classifier classifier;
    view::ValueChannelTelemetryAttachment attachment;
    std::vector<Channel> channels;
    std::unordered_map<std::string, std::size_t> channel_index;
    std::map<std::string, Subscription, std::less<>> subscriptions;
    mutable std::mutex mutex;

    std::chrono::steady_clock::time_point now() const {
        return clock ? clock() : std::chrono::steady_clock::now();
    }

    void refresh() {
        for (auto& channel : channels) {
            channel.events.clear();
            if (channel.descriptor.info.shape == view::ValueChannelShape::events) {
                view::ValueChannelTelemetryEventBatch batch;
                for (std::size_t count = 0; count < config.maximum_vector_values &&
                                            attachment.try_pop_events(channel.index, batch);
                     ++count)
                    channel.events.push_back(std::move(batch));
                channel.event_overflow = attachment.event_stats(channel.index).overflow_count;
            } else {
                (void)attachment.read_continuous(channel.index, channel.continuous);
            }
        }
    }

    ControlTelemetryChannelSample sample(const Subscription& subscription,
                                         const std::string& name) const {
        const auto found = channel_index.find(name);
        if (found == channel_index.end())
            return {};
        const auto& channel = channels[found->second];
        const bool sensitive =
            !classifier || classifier(name) == ControlTelemetrySensitivity::Sensitive;
        const bool redacted = sensitive && !subscription.authority.allow_sensitive;
        ControlTelemetryChannelSample result{
            .channel = redacted ? "channel-" + std::to_string(channel.index) : name,
            .shape = channel.descriptor.info.shape,
            .source_alive = attachment.source_alive(channel.index),
        };
        if (redacted) {
            result.redacted = true;
            return result;
        }
        if (channel.descriptor.info.shape == view::ValueChannelShape::events) {
            const auto pending = subscription.pending_events.find(name);
            if (pending == subscription.pending_events.end())
                return result;
            result.source_dropped = pending->second.dropped;
            result.source_publication = pending->second.latest_publication;
            result.values.assign(pending->second.values.begin(), pending->second.values.end());
            return result;
        }
        result.source_publication = channel.continuous.source_publication;
        if (!channel.continuous.available)
            return result;
        std::visit(
            [&](const auto& payload) {
                using T = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<T, float>) {
                    result.values.push_back(payload);
                } else if constexpr (std::is_same_v<T, view::MeterFrame>) {
                    const auto count = std::min<std::size_t>(
                        static_cast<std::size_t>(
                            std::clamp(payload.channels, 0, view::MeterFrame::kMaxChannels)),
                        subscription.maximum_vector_values / 2);
                    for (std::size_t index = 0; index < count; ++index) {
                        result.values.push_back(payload.peak[index]);
                        result.values.push_back(payload.rms[index]);
                    }
                } else {
                    const auto count = std::min<std::size_t>(
                        static_cast<std::size_t>(
                            std::clamp(payload.count, 0, view::VectorFrame::kMaxSamples)),
                        subscription.maximum_vector_values);
                    result.values.insert(result.values.end(), payload.samples.begin(),
                                         payload.samples.begin() +
                                             static_cast<std::ptrdiff_t>(count));
                }
            },
            channel.continuous.payload);
        return result;
    }
};

ControlTelemetryTap::ControlTelemetryTap(ControlTelemetryTapConfig config, Clock clock)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = std::move(config);
    impl_->clock = std::move(clock);
}
ControlTelemetryTap::~ControlTelemetryTap() = default;
ControlTelemetryTap::ControlTelemetryTap(ControlTelemetryTap&&) noexcept = default;
ControlTelemetryTap& ControlTelemetryTap::operator=(ControlTelemetryTap&&) noexcept = default;

bool ControlTelemetryTap::attach(view::ValueChannelTelemetryAttachment attachment,
                                 Classifier classifier) {
    std::lock_guard lock(impl_->mutex);
    impl_->subscriptions.clear();
    impl_->channel_index.clear();
    impl_->channels.clear();
    impl_->classifier = {};
    impl_->attachment = {};
    if (!impl_->config.enabled || !attachment.valid() ||
        !std::isfinite(impl_->config.minimum_rate_hz) ||
        !std::isfinite(impl_->config.maximum_rate_hz) || impl_->config.minimum_rate_hz <= 0.0 ||
        impl_->config.maximum_rate_hz < impl_->config.minimum_rate_hz ||
        impl_->config.maximum_subscriptions == 0 ||
        impl_->config.maximum_subscriptions_per_client == 0 ||
        impl_->config.maximum_channels_per_subscription == 0 ||
        impl_->config.maximum_vector_values == 0 || impl_->config.maximum_queued_frames == 0)
        return false;
    impl_->attachment = std::move(attachment);
    impl_->classifier = std::move(classifier);
    for (const auto& descriptor : impl_->attachment.channels()) {
        if (!impl_->channel_index.emplace(descriptor.info.name, impl_->channels.size()).second) {
            impl_->subscriptions.clear();
            impl_->channel_index.clear();
            impl_->channels.clear();
            impl_->classifier = {};
            impl_->attachment = {};
            return false;
        }
        impl_->channels.push_back({.index = descriptor.index, .descriptor = descriptor});
    }
    return true;
}

void ControlTelemetryTap::detach() noexcept {
    std::lock_guard lock(impl_->mutex);
    impl_->subscriptions.clear();
    impl_->channel_index.clear();
    impl_->channels.clear();
    impl_->classifier = {};
    impl_->attachment = {};
}

std::optional<std::string>
ControlTelemetryTap::subscribe(const ControlTelemetryAuthority& authority,
                               ControlTelemetrySubscriptionRequest request) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->attachment.valid() || authority.client_id.empty() ||
        authority.registration_id.empty() || authority.instance_id.empty() ||
        authority.grant_id.empty() || request.channels.empty() ||
        request.channels.size() > impl_->config.maximum_channels_per_subscription ||
        !std::isfinite(request.rate_hz) || request.rate_hz < impl_->config.minimum_rate_hz ||
        request.maximum_vector_values == 0 ||
        request.maximum_vector_values > impl_->config.maximum_vector_values ||
        impl_->subscriptions.size() >= impl_->config.maximum_subscriptions)
        return std::nullopt;
    std::set<std::string, std::less<>> unique;
    for (const auto& name : request.channels)
        if (!impl_->channel_index.contains(name) || !unique.insert(name).second)
            return std::nullopt;
    const auto owned = std::ranges::count_if(impl_->subscriptions, [&](const auto& entry) {
        return entry.second.authority.client_id == authority.client_id;
    });
    if (static_cast<std::size_t>(owned) >= impl_->config.maximum_subscriptions_per_client)
        return std::nullopt;
    const auto id = subscription_id();
    if (id.empty())
        return std::nullopt;
    const auto effective_rate = std::min(request.rate_hz, impl_->config.maximum_rate_hz);
    Impl::Subscription subscription{
        .id = id,
        .authority = authority,
        .names = std::move(request.channels),
        .maximum_vector_values = request.maximum_vector_values,
        .period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / effective_rate)),
        .next_due = impl_->now(),
    };
    for (const auto& name : subscription.names) {
        const auto& channel = impl_->channels[impl_->channel_index.at(name)];
        if (channel.descriptor.info.shape == view::ValueChannelShape::events) {
            subscription.pending_events.emplace(
                name, Impl::Subscription::PendingEvents{
                          .observed_overflow =
                              impl_->attachment.event_stats(channel.index).overflow_count});
        }
    }
    impl_->subscriptions.emplace(id, std::move(subscription));
    return id;
}

bool ControlTelemetryTap::unsubscribe(std::string_view subscription_id,
                                      const ControlTelemetryAuthority& authority) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->subscriptions.find(subscription_id);
    if (found == impl_->subscriptions.end() || !same_authority(found->second.authority, authority))
        return false;
    impl_->subscriptions.erase(found);
    return true;
}

std::size_t ControlTelemetryTap::end_authority(
    const ControlTelemetryAuthority& authority) noexcept {
    std::lock_guard lock(impl_->mutex);
    const auto before = impl_->subscriptions.size();
    std::erase_if(impl_->subscriptions, [&](const auto& entry) {
        return same_authority(entry.second.authority, authority);
    });
    return before - impl_->subscriptions.size();
}

void ControlTelemetryTap::poll() {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->attachment.valid())
        return;
    impl_->refresh();
    const auto sampled_at = impl_->now();
    const auto sampled_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(sampled_at.time_since_epoch()).count();
    for (auto& [id, subscription] : impl_->subscriptions) {
        (void)id;
        for (const auto& name : subscription.names) {
            const auto channel_found = impl_->channel_index.find(name);
            if (channel_found == impl_->channel_index.end())
                continue;
            const auto& channel = impl_->channels[channel_found->second];
            if (channel.descriptor.info.shape != view::ValueChannelShape::events)
                continue;
            auto& pending = subscription.pending_events.at(name);
            if (channel.event_overflow >= pending.observed_overflow)
                pending.dropped += channel.event_overflow - pending.observed_overflow;
            pending.observed_overflow = channel.event_overflow;
            for (const auto& event : channel.events) {
                if (pending.values.size() == subscription.maximum_vector_values) {
                    pending.values.pop_front();
                    ++pending.dropped;
                }
                pending.values.push_back(static_cast<float>(std::max(0, event.frame.count)));
                pending.latest_publication = event.source_publication;
            }
        }
        if (sampled_at < subscription.next_due)
            continue;
        subscription.next_due = sampled_at + subscription.period;
        ControlTelemetryFrame frame{.sequence = ++subscription.sequence,
                                    .sampled_at_ns =
                                        sampled_ns > 0 ? static_cast<std::uint64_t>(sampled_ns) : 0,
                                    .dropped_since_previous = subscription.loss_debt};
        frame.channels.reserve(subscription.names.size());
        for (const auto& name : subscription.names) {
            frame.channels.push_back(impl_->sample(subscription, name));
            if (auto pending = subscription.pending_events.find(name);
                pending != subscription.pending_events.end()) {
                pending->second.values.clear();
                pending->second.dropped = 0;
            }
        }
        if (subscription.queued.size() == impl_->config.maximum_queued_frames) {
            subscription.queued.pop_front();
            ++subscription.loss_debt;
        }
        subscription.queued.push_back(std::move(frame));
        subscription.queued.front().dropped_since_previous = subscription.loss_debt;
    }
}

std::optional<ControlTelemetryFrame>
ControlTelemetryTap::try_pop(std::string_view subscription_id,
                             const ControlTelemetryAuthority& authority) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->subscriptions.find(subscription_id);
    if (found == impl_->subscriptions.end() ||
        !same_authority(found->second.authority, authority) || found->second.queued.empty())
        return std::nullopt;
    auto frame = std::move(found->second.queued.front());
    found->second.queued.pop_front();
    if (!found->second.queued.empty())
        found->second.queued.front().dropped_since_previous = 0;
    found->second.loss_debt = 0;
    return frame;
}

double ControlTelemetryTap::effective_rate_hz(double requested_rate_hz) const noexcept {
    if (!std::isfinite(requested_rate_hz))
        return 0.0;
    return std::min(requested_rate_hz, impl_->config.maximum_rate_hz);
}

std::size_t ControlTelemetryTap::subscription_count() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->subscriptions.size();
}

} // namespace pulp::inspect
