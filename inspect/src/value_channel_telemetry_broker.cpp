#include <pulp/inspect/value_channel_telemetry_broker.hpp>

#include <pulp/inspect/protocol.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pulp::inspect {
namespace {

using TimePoint = std::chrono::steady_clock::time_point;

constexpr std::size_t kMaxSubscriptionIdBytes = 128;

std::int64_t time_ns(TimePoint value) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count();
}

const char* shape_name(view::ValueChannelShape shape) noexcept {
    switch (shape) {
    case view::ValueChannelShape::scalar:
        return "scalar";
    case view::ValueChannelShape::meter:
        return "meter";
    case view::ValueChannelShape::vector:
        return "vector";
    case view::ValueChannelShape::events:
        return "events";
    }
    return "scalar";
}

choc::value::Value null_value() {
    return {};
}

// Preserve DSP anomaly evidence instead of letting CHOC serialize non-finite
// numbers as indistinguishable JSON nulls. This matches the inspector motion
// wire convention.
choc::value::Value wire_number(double value) {
    if (std::isnan(value))
        return choc::value::createString("NaN");
    if (std::isinf(value) && value > 0)
        return choc::value::createString("Infinity");
    if (std::isinf(value) && value < 0)
        return choc::value::createString("-Infinity");
    return choc::value::createFloat64(value);
}

} // namespace

struct ValueChannelTelemetryBroker::Impl {
    struct EventOccurrence {
        std::uint64_t source_publication = 0;
        std::uint32_t frame_index = 0;
        float value = 0.0f;
    };

    struct ChannelCache {
        view::ValueChannelTelemetryDescriptor descriptor;
        view::ValueChannelTelemetrySnapshot continuous;
        std::vector<EventOccurrence> latest_events;
        std::uint64_t event_overflow = 0;
        std::uint64_t latest_event_coalesced = 0;
        std::uint64_t latest_event_publication = 0;
        bool continuous_read = false;
        bool source_alive = false;
    };

    struct PendingChannel {
        std::vector<EventOccurrence> events;
        std::uint64_t source_dropped = 0;
        std::uint64_t coalesced = 0;
        std::uint64_t last_source_publication = 0;
        bool terminal_reported = false;
    };

    struct Subscription {
        std::string id;
        std::vector<std::string> names;
        double requested_rate_hz = 15.0;
        double effective_rate_hz = 15.0;
        std::size_t max_vector_values = 512;
        TimePoint next_due{};
        std::uint64_t attempt_sequence = 0;
        std::uint64_t transport_loss_debt = 0;
        std::uint64_t attached_generation = 0;
        bool reattached_pending = false;
        std::unordered_map<std::string, PendingChannel> pending;
    };

    explicit Impl(Config requested, Clock requested_clock)
        : config(std::move(requested)), clock(std::move(requested_clock)) {
        config.max_rate_hz = std::clamp(config.max_rate_hz, 1.0, 60.0);
        config.default_rate_hz = std::clamp(config.default_rate_hz, 1.0, config.max_rate_hz);
        config.max_channels = std::clamp<std::size_t>(config.max_channels, 1, 32);
        config.max_vector_values = std::clamp<std::size_t>(
            config.max_vector_values, 1, static_cast<std::size_t>(view::VectorFrame::kMaxSamples));
        config.max_events_per_channel =
            std::clamp<std::size_t>(config.max_events_per_channel, 1,
                                    static_cast<std::size_t>(view::EventFrame::kMaxEvents));
        config.max_wire_bytes =
            std::clamp<std::size_t>(config.max_wire_bytes, 1024, kInspectorExtendedMessageBytes);
        config.stale_after = std::max(config.stale_after, std::chrono::milliseconds(1));
        if (!clock)
            clock = [] { return std::chrono::steady_clock::now(); };
    }

    TimePoint now() const {
        return clock();
    }

    const ChannelCache* find_channel(std::string_view name) const {
        const auto found = channel_by_name.find(std::string(name));
        return found == channel_by_name.end() ? nullptr : &channels[found->second];
    }

    bool selects(const Subscription& subscription, std::string_view name) const {
        return std::find(subscription.names.begin(), subscription.names.end(), name) !=
               subscription.names.end();
    }

    void rebuild_channels() {
        channels.clear();
        channel_by_name.clear();
        if (!attachment.valid())
            return;
        const auto descriptors = attachment.channels();
        const auto count = descriptors.size();
        channels.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            ChannelCache cache;
            cache.descriptor = descriptors[i];
            cache.source_alive = attachment.source_alive(cache.descriptor.index);
            if (cache.descriptor.info.shape == view::ValueChannelShape::events) {
                cache.event_overflow =
                    attachment.event_stats(cache.descriptor.index).overflow_count;
            }
            channel_by_name.emplace(cache.descriptor.info.name, channels.size());
            channels.push_back(std::move(cache));
        }
        for (auto& [client, subscription] : subscriptions) {
            (void)client;
            subscription.reattached_pending = true;
            subscription.attached_generation = source_generation;
            subscription.pending.clear();
            for (const auto& name : subscription.names)
                subscription.pending.try_emplace(name);
            subscription.next_due = now();
        }
    }

    void clear_source() {
        attachment = {};
        channels.clear();
        channel_by_name.clear();
        for (auto& [client, subscription] : subscriptions) {
            (void)client;
            subscription.reattached_pending = true;
            subscription.pending.clear();
            for (const auto& name : subscription.names)
                subscription.pending.try_emplace(name);
            subscription.next_due = now();
        }
    }

    void refresh_sources() {
        if (!attachment.valid())
            return;
        for (auto& channel : channels) {
            const auto index = channel.descriptor.index;
            channel.source_alive = attachment.source_alive(index);
            if (channel.descriptor.info.shape != view::ValueChannelShape::events) {
                channel.continuous_read = attachment.read_continuous(index, channel.continuous);
                continue;
            }

            const auto stats = attachment.event_stats(index);
            const auto overflow_delta = stats.overflow_count >= channel.event_overflow
                                            ? stats.overflow_count - channel.event_overflow
                                            : stats.overflow_count;
            channel.event_overflow = stats.overflow_count;
            for (auto& [client, subscription] : subscriptions) {
                (void)client;
                if (selects(subscription, channel.descriptor.info.name))
                    subscription.pending[channel.descriptor.info.name].source_dropped +=
                        overflow_delta;
            }

            view::ValueChannelTelemetryEventBatch batch;
            bool received_batch = false;
            while (attachment.try_pop_events(index, batch)) {
                if (!received_batch) {
                    channel.latest_events.clear();
                    channel.latest_event_coalesced = 0;
                    received_batch = true;
                }
                channel.latest_event_publication = batch.source_publication;
                const auto count = std::clamp(batch.frame.count, 0, view::EventFrame::kMaxEvents);
                for (int i = 0; i < count; ++i) {
                    const auto& occurrence = batch.frame.events[static_cast<std::size_t>(i)];
                    EventOccurrence value{
                        batch.source_publication,
                        occurrence.frame_index,
                        occurrence.value,
                    };
                    if (channel.latest_events.size() == config.max_events_per_channel) {
                        channel.latest_events.erase(channel.latest_events.begin());
                        ++channel.latest_event_coalesced;
                    }
                    channel.latest_events.push_back(value);
                    for (auto& [client, subscription] : subscriptions) {
                        (void)client;
                        if (!selects(subscription, channel.descriptor.info.name))
                            continue;
                        auto& pending = subscription.pending[channel.descriptor.info.name];
                        if (pending.events.size() < config.max_events_per_channel) {
                            pending.events.push_back(value);
                        } else {
                            ++pending.coalesced;
                        }
                    }
                }
            }
        }
    }

    std::vector<std::string> parse_names(const choc::value::ValueView& params,
                                         std::string& error) const {
        std::vector<std::string> names;
        if (!params.hasObjectMember("channels")) {
            if (channels.size() > config.max_channels) {
                error = "Telemetry channel selection exceeds the 32-channel limit";
                return {};
            }
            names.reserve(channels.size());
            for (const auto& channel : channels)
                names.push_back(channel.descriptor.info.name);
            return names;
        }
        const auto requested = params["channels"];
        if (!requested.isArray()) {
            error = "Telemetry channels must be an array of names";
            return {};
        }
        if (requested.size() > config.max_channels) {
            error = "Telemetry channel selection exceeds the 32-channel limit";
            return {};
        }
        std::unordered_set<std::string> unique;
        for (std::uint32_t i = 0; i < requested.size(); ++i) {
            const auto value = requested[i];
            if (!value.isString() || value.getString().empty()) {
                error = "Telemetry channel names must be non-empty strings";
                return {};
            }
            auto name = std::string(value.getString());
            if (!find_channel(name)) {
                error = "Unknown value channel";
                return {};
            }
            if (!unique.insert(name).second) {
                error = "Duplicate value channel";
                return {};
            }
            names.push_back(std::move(name));
        }
        if (names.empty())
            error = "Telemetry channel selection must not be empty";
        return names;
    }

    double parse_rate(const choc::value::ValueView& params, std::string& error) const {
        if (!params.hasObjectMember("rateHz"))
            return config.default_rate_hz;
        const auto value = params["rateHz"];
        if (!value.isInt() && !value.isFloat()) {
            error = "Telemetry rateHz must be numeric";
            return 0.0;
        }
        const auto rate = value.getWithDefault(0.0);
        if (!std::isfinite(rate) || rate <= 0.0) {
            error = "Telemetry rateHz must be greater than zero";
            return 0.0;
        }
        return rate;
    }

    std::size_t parse_vector_cap(const choc::value::ValueView& params, std::string& error) const {
        if (!params.hasObjectMember("maxVectorValues"))
            return config.max_vector_values;
        const auto value = params["maxVectorValues"];
        if (!value.isInt()) {
            error = "Telemetry maxVectorValues must be an integer";
            return 0;
        }
        const auto requested = value.getWithDefault<std::int64_t>(0);
        if (requested <= 0) {
            error = "Telemetry maxVectorValues must be greater than zero";
            return 0;
        }
        return std::min<std::size_t>(static_cast<std::size_t>(requested), config.max_vector_values);
    }

    std::size_t payload_value_cap(std::size_t selected, std::size_t requested_cap) const {
        const auto per_channel =
            config.max_wire_bytes > 4096
                ? (config.max_wire_bytes - 4096) / std::max<std::size_t>(selected, 1)
                : 256;
        return std::max<std::size_t>(
            1, std::min(requested_cap, per_channel > 768 ? (per_channel - 768) / 32 : 1));
    }

    std::size_t payload_event_cap(std::size_t selected) const {
        const auto per_channel =
            config.max_wire_bytes > 4096
                ? (config.max_wire_bytes - 4096) / std::max<std::size_t>(selected, 1)
                : 256;
        return std::max<std::size_t>(1, std::min(config.max_events_per_channel,
                                                 per_channel > 768 ? (per_channel - 768) / 96 : 1));
    }

    choc::value::Value channel_value(const std::string& name, PendingChannel& pending,
                                     TimePoint sampled_at, std::size_t selected_count,
                                     std::size_t max_vector_values,
                                     std::chrono::milliseconds stale_after,
                                     bool for_snapshot) const {
        auto result = choc::value::createObject("");
        result.addMember("name", choc::value::createString(name));
        result.addMember("sourceTimestampNs", null_value());
        const auto* channel = find_channel(name);
        if (!channel) {
            result.addMember("shape", choc::value::createString("unknown"));
            result.addMember("available", choc::value::createBool(false));
            result.addMember("sourceAlive", choc::value::createBool(false));
            result.addMember("terminal", choc::value::createBool(false));
            result.addMember("stale", choc::value::createBool(true));
            result.addMember("staleReason",
                             choc::value::createString("unavailable_after_reattach"));
            result.addMember("sequence", choc::value::createInt64(0));
            result.addMember("uiTimestampNs", null_value());
            result.addMember("sourcePublication", choc::value::createInt64(0));
            result.addMember("coalesced", choc::value::createInt64(0));
            result.addMember("sourceDropped", choc::value::createInt64(0));
            result.addMember("payloadBytes", choc::value::createInt64(0));
            result.addMember("payload", null_value());
            return result;
        }

        const auto shape = channel->descriptor.info.shape;
        const bool is_events = shape == view::ValueChannelShape::events;
        bool available = is_events ? channel->source_alive
                                   : channel->continuous_read && channel->continuous.available;
        bool stale = false;
        std::string stale_reason = "none";
        std::int64_t age_ms = 0;
        if (!channel->source_alive) {
            stale = true;
            stale_reason = "source_dead";
        } else if (!available) {
            stale = true;
            stale_reason = "not_yet_consumed";
        } else if (!is_events) {
            const auto sampled_ns = time_ns(sampled_at);
            const auto age_ns =
                std::max<std::int64_t>(0, sampled_ns - channel->continuous.ui_snapshot_time_ns);
            age_ms = age_ns / 1000000;
            if (age_ns >
                std::chrono::duration_cast<std::chrono::nanoseconds>(stale_after).count()) {
                stale = true;
                stale_reason = "ui_not_consuming";
            }
        }

        std::uint64_t source_publication =
            is_events ? channel->latest_event_publication : channel->continuous.source_publication;
        const auto previous = pending.last_source_publication;
        std::uint64_t coalesced =
            pending.coalesced + (for_snapshot && is_events ? channel->latest_event_coalesced : 0);
        if (!is_events && previous != 0 && source_publication > previous + 1)
            coalesced += source_publication - previous - 1;

        auto payload = null_value();
        // Source death does not make an unobserved payload valid. Continuous
        // variants are safe to inspect only after a sidecar snapshot exists.
        if (is_events || channel->continuous.available) {
            switch (shape) {
            case view::ValueChannelShape::scalar:
                payload = wire_number(std::get<float>(channel->continuous.payload));
                break;
            case view::ValueChannelShape::meter: {
                const auto& frame = std::get<view::MeterFrame>(channel->continuous.payload);
                auto meter = choc::value::createObject("");
                auto rms = choc::value::createEmptyArray();
                auto peak = choc::value::createEmptyArray();
                const auto count = std::clamp(frame.channels, 0, view::MeterFrame::kMaxChannels);
                for (int i = 0; i < count; ++i) {
                    rms.addArrayElement(wire_number(frame.rms[static_cast<std::size_t>(i)]));
                    peak.addArrayElement(wire_number(frame.peak[static_cast<std::size_t>(i)]));
                }
                meter.addMember("rms", rms);
                meter.addMember("peak", peak);
                payload = std::move(meter);
                break;
            }
            case view::ValueChannelShape::vector: {
                const auto& frame = std::get<view::VectorFrame>(channel->continuous.payload);
                auto vector = choc::value::createObject("");
                auto values = choc::value::createEmptyArray();
                const auto count = static_cast<std::size_t>(
                    std::clamp(frame.count, 0, view::VectorFrame::kMaxSamples));
                const auto retained =
                    std::min(count, payload_value_cap(selected_count, max_vector_values));
                for (std::size_t i = 0; i < retained; ++i)
                    values.addArrayElement(wire_number(frame.samples[i]));
                vector.addMember("values", values);
                vector.addMember("count",
                                 choc::value::createInt64(static_cast<std::int64_t>(count)));
                vector.addMember("truncated", choc::value::createBool(retained < count));
                coalesced += count - retained;
                payload = std::move(vector);
                break;
            }
            case view::ValueChannelShape::events: {
                auto events = choc::value::createEmptyArray();
                const auto& occurrences = for_snapshot ? channel->latest_events : pending.events;
                const auto retained =
                    std::min(occurrences.size(), payload_event_cap(selected_count));
                const auto first = for_snapshot ? occurrences.size() - retained : 0;
                for (std::size_t i = 0; i < retained; ++i) {
                    const auto& occurrence = occurrences[first + i];
                    auto event = choc::value::createObject("");
                    event.addMember("sourcePublication",
                                    choc::value::createInt64(
                                        static_cast<std::int64_t>(occurrence.source_publication)));
                    event.addMember("frameIndex", choc::value::createInt64(occurrence.frame_index));
                    event.addMember("value", wire_number(occurrence.value));
                    events.addArrayElement(std::move(event));
                }
                coalesced += occurrences.size() - retained;
                payload = std::move(events);
                break;
            }
            }
        }
        const auto payload_bytes = choc::json::toString(payload, false).size();
        result.addMember("shape", choc::value::createString(shape_name(shape)));
        result.addMember("available", choc::value::createBool(available));
        result.addMember("sourceAlive", choc::value::createBool(channel->source_alive));
        result.addMember("terminal", choc::value::createBool(!channel->source_alive));
        result.addMember("stale", choc::value::createBool(stale));
        result.addMember("staleReason", choc::value::createString(stale_reason));
        result.addMember("ageMs", choc::value::createInt64(age_ms));
        result.addMember("sequence", choc::value::createInt64(static_cast<std::int64_t>(
                                         is_events ? source_publication
                                                   : channel->continuous.ui_snapshot_sequence)));
        if (is_events || !channel->continuous.available) {
            result.addMember("uiTimestampNs", null_value());
        } else {
            result.addMember("uiTimestampNs",
                             choc::value::createInt64(channel->continuous.ui_snapshot_time_ns));
        }
        result.addMember("sourcePublication",
                         choc::value::createInt64(static_cast<std::int64_t>(source_publication)));
        result.addMember("coalesced",
                         choc::value::createInt64(static_cast<std::int64_t>(coalesced)));
        const auto source_dropped =
            for_snapshot && is_events ? channel->event_overflow : pending.source_dropped;
        result.addMember("sourceDropped",
                         choc::value::createInt64(static_cast<std::int64_t>(source_dropped)));
        result.addMember("payloadBytes",
                         choc::value::createInt64(static_cast<std::int64_t>(payload_bytes)));
        result.addMember("payload", std::move(payload));
        return result;
    }

    std::string sample_json(Subscription& subscription, TimePoint sampled_at,
                            bool for_snapshot) const {
        auto root = choc::value::createObject("");
        root.addMember(
            "schema", choc::value::createString(for_snapshot ? "pulp.inspect.telemetry.snapshot.v1"
                                                             : "pulp.inspect.telemetry.sample.v1"));
        root.addMember("schemaVersion", choc::value::createInt64(1));
        root.addMember("kind", choc::value::createString(for_snapshot ? "snapshot" : "sample"));
        root.addMember("sourceGeneration",
                       choc::value::createInt64(static_cast<std::int64_t>(source_generation)));
        root.addMember("reattached", choc::value::createBool(subscription.reattached_pending));
        root.addMember("subscriptionId", choc::value::createString(subscription.id));
        root.addMember("attemptSequence", choc::value::createInt64(static_cast<std::int64_t>(
                                              subscription.attempt_sequence)));
        root.addMember("sampledAtNs", choc::value::createInt64(time_ns(sampled_at)));
        root.addMember("requestedRateHz", subscription.requested_rate_hz);
        root.addMember("effectiveRateHz", subscription.effective_rate_hz);
        root.addMember("maxVectorValues", choc::value::createInt64(static_cast<std::int64_t>(
                                              subscription.max_vector_values)));
        root.addMember(
            "transportDroppedSincePrevious",
            choc::value::createInt64(static_cast<std::int64_t>(subscription.transport_loss_debt)));
        auto values = choc::value::createEmptyArray();
        for (const auto& name : subscription.names) {
            auto& pending = subscription.pending[name];
            if (!for_snapshot && pending.terminal_reported)
                continue;
            values.addArrayElement(channel_value(
                name, pending, sampled_at, subscription.names.size(),
                subscription.max_vector_values,
                for_snapshot ? config.stale_after
                             : std::max(config.stale_after,
                                        std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::duration<double>(
                                                2.0 / subscription.effective_rate_hz))),
                for_snapshot));
        }
        root.addMember("channelCount",
                       choc::value::createInt64(static_cast<std::int64_t>(values.size())));
        root.addMember("channels", std::move(values));
        return choc::json::toString(root, false);
    }

    void commit_delivery(Subscription& subscription) {
        for (const auto& name : subscription.names) {
            auto& pending = subscription.pending[name];
            if (const auto* channel = find_channel(name)) {
                pending.last_source_publication =
                    channel->descriptor.info.shape == view::ValueChannelShape::events
                        ? channel->latest_event_publication
                        : channel->continuous.source_publication;
                if (!channel->source_alive)
                    pending.terminal_reported = true;
            }
            pending.events.clear();
            pending.source_dropped = 0;
            pending.coalesced = 0;
        }
        subscription.reattached_pending = false;
    }

    Config config;
    Clock clock;
    EventSink sink;
    view::ValueChannelTelemetryAttachment attachment;
    std::vector<ChannelCache> channels;
    std::unordered_map<std::string, std::size_t> channel_by_name;
    std::map<std::string, Subscription> subscriptions;
    std::uint64_t source_generation = 0;
    std::uint64_t next_subscription_id = 1;
};

ValueChannelTelemetryBroker::ValueChannelTelemetryBroker()
    : ValueChannelTelemetryBroker(Config{}) {}

ValueChannelTelemetryBroker::ValueChannelTelemetryBroker(Config config, Clock clock)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(clock))) {}

ValueChannelTelemetryBroker::~ValueChannelTelemetryBroker() = default;
ValueChannelTelemetryBroker::ValueChannelTelemetryBroker(ValueChannelTelemetryBroker&&) noexcept =
    default;
ValueChannelTelemetryBroker&
ValueChannelTelemetryBroker::operator=(ValueChannelTelemetryBroker&&) noexcept = default;

void ValueChannelTelemetryBroker::set_event_sink(EventSink sink) {
    impl_->sink = std::move(sink);
}

bool ValueChannelTelemetryBroker::replace_attachment(
    view::ValueChannelTelemetryAttachment attachment) {
    impl_->clear_source();
    if (!attachment.valid())
        return false;
    impl_->attachment = std::move(attachment);
    ++impl_->source_generation;
    impl_->rebuild_channels();
    return true;
}

void ValueChannelTelemetryBroker::clear_attachment() {
    impl_->clear_source();
}

InspectorMessage ValueChannelTelemetryBroker::handle(const InspectorRequestContext& context,
                                                     const InspectorMessage& request) {
    if (context.client_id.empty()) {
        return make_error(request.id, "Telemetry requires authenticated client identity",
                          "telemetry_client_required");
    }
    if (request.method != methods::kTelemetryGetSnapshot &&
        request.method != methods::kTelemetrySubscribe &&
        request.method != methods::kTelemetryUnsubscribe) {
        return make_error(request.id, "Unknown Telemetry method", "method_not_found");
    }
    choc::value::Value params;
    try {
        params = request.params_json.empty() ? choc::value::createObject("")
                                             : choc::json::parse(request.params_json);
    } catch (...) {
        return make_error(request.id, "Telemetry params are not valid JSON", "invalid_params");
    }
    if (!params.isObject()) {
        return make_error(request.id, "Telemetry params must be an object", "invalid_params");
    }

    const auto client_id = std::string(context.client_id);
    if (request.method == methods::kTelemetryUnsubscribe) {
        if (!params.hasObjectMember("subscriptionId") || !params["subscriptionId"].isString() ||
            params["subscriptionId"].getString().empty()) {
            return make_error(request.id, "Telemetry unsubscribe requires subscriptionId",
                              "invalid_params");
        }
        const auto requested_id = std::string(params["subscriptionId"].getString());
        if (requested_id.size() > kMaxSubscriptionIdBytes) {
            return make_error(request.id, "Telemetry subscriptionId exceeds the size limit",
                              "invalid_params");
        }
        const auto found = impl_->subscriptions.find(client_id);
        if (found != impl_->subscriptions.end() && found->second.id != requested_id) {
            return make_error(request.id, "Telemetry subscriptionId does not belong to client",
                              "subscription_mismatch");
        }
        const bool removed = found != impl_->subscriptions.end();
        auto result = choc::value::createObject("");
        result.addMember("schema",
                         choc::value::createString("pulp.inspect.telemetry.unsubscribe.v1"));
        result.addMember("schemaVersion", choc::value::createInt64(1));
        result.addMember("kind", choc::value::createString("unsubscribe"));
        result.addMember("subscriptionId", choc::value::createString(requested_id));
        result.addMember("unsubscribed", choc::value::createBool(removed));
        auto response = make_response(request.id, choc::json::toString(result, false));
        if (encode_message(response).size() > impl_->config.max_wire_bytes) {
            return make_error(request.id, "Telemetry unsubscribe exceeds the wire limit",
                              "telemetry_payload_too_large");
        }
        if (removed)
            impl_->subscriptions.erase(found);
        return response;
    }
    if (!impl_->attachment.valid()) {
        return make_error(request.id, "Value-channel telemetry is unavailable",
                          "telemetry_unavailable");
    }

    std::string error;
    auto names = impl_->parse_names(params, error);
    if (!error.empty())
        return make_error(request.id, std::move(error), "invalid_params");
    const auto max_vector_values = impl_->parse_vector_cap(params, error);
    if (!error.empty())
        return make_error(request.id, std::move(error), "invalid_params");

    if (request.method == methods::kTelemetryGetSnapshot) {
        impl_->refresh_sources();
        Impl::Subscription snapshot;
        snapshot.id = "snapshot";
        snapshot.names = std::move(names);
        snapshot.requested_rate_hz = 0.0;
        snapshot.effective_rate_hz = 0.0;
        snapshot.max_vector_values = max_vector_values;
        snapshot.attached_generation = impl_->source_generation;
        for (const auto& name : snapshot.names)
            snapshot.pending.try_emplace(name);
        const auto json = impl_->sample_json(snapshot, impl_->now(), true);
        auto response = make_response(request.id, json);
        if (encode_message(response).size() > impl_->config.max_wire_bytes) {
            return make_error(request.id, "Telemetry snapshot exceeds the wire limit",
                              "telemetry_payload_too_large");
        }
        return response;
    }

    const auto requested_rate = impl_->parse_rate(params, error);
    if (!error.empty())
        return make_error(request.id, std::move(error), "invalid_params");
    Impl::Subscription subscription;
    subscription.id = "telemetry-" + std::to_string(impl_->next_subscription_id++);
    subscription.names = std::move(names);
    subscription.requested_rate_hz = requested_rate;
    subscription.effective_rate_hz = std::min(requested_rate, impl_->config.max_rate_hz);
    subscription.max_vector_values = max_vector_values;
    subscription.next_due = impl_->now();
    subscription.attached_generation = impl_->source_generation;
    for (const auto& name : subscription.names)
        subscription.pending.try_emplace(name);
    auto result = choc::value::createObject("");
    result.addMember("schema", choc::value::createString("pulp.inspect.telemetry.subscription.v1"));
    result.addMember("schemaVersion", choc::value::createInt64(1));
    result.addMember("kind", choc::value::createString("subscription"));
    result.addMember("subscriptionId", choc::value::createString(subscription.id));
    result.addMember("sourceGeneration",
                     choc::value::createInt64(static_cast<std::int64_t>(impl_->source_generation)));
    result.addMember("reattached", choc::value::createBool(false));
    result.addMember("requestedRateHz", requested_rate);
    result.addMember("effectiveRateHz", subscription.effective_rate_hz);
    result.addMember("maxVectorValues", choc::value::createInt64(static_cast<std::int64_t>(
                                            subscription.max_vector_values)));
    auto selected = choc::value::createEmptyArray();
    for (const auto& name : subscription.names)
        selected.addArrayElement(choc::value::createString(name));
    result.addMember("channels", std::move(selected));
    result.addMember("channelCount", choc::value::createInt64(
                                         static_cast<std::int64_t>(subscription.names.size())));
    auto response_json = choc::json::toString(result, false);
    auto response = make_response(request.id, response_json);
    if (encode_message(response).size() > impl_->config.max_wire_bytes) {
        return make_error(request.id, "Telemetry subscription exceeds the wire limit",
                          "telemetry_payload_too_large");
    }
    impl_->subscriptions.insert_or_assign(client_id, std::move(subscription));
    return response;
}

void ValueChannelTelemetryBroker::poll() {
    impl_->refresh_sources();
    const auto sampled_at = impl_->now();
    for (auto iterator = impl_->subscriptions.begin(); iterator != impl_->subscriptions.end();) {
        auto& subscription = iterator->second;
        if (sampled_at < subscription.next_due) {
            ++iterator;
            continue;
        }
        const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / subscription.effective_rate_hz));
        subscription.next_due = sampled_at + period;

        bool has_reportable_channel = false;
        for (const auto& name : subscription.names) {
            if (!subscription.pending[name].terminal_reported) {
                has_reportable_channel = true;
                break;
            }
        }
        if (!has_reportable_channel) {
            ++iterator;
            continue;
        }

        ++subscription.attempt_sequence;
        auto params = impl_->sample_json(subscription, sampled_at, false);
        auto event = make_event(methods::kTelemetrySample, params);
        bool wire_truncated = encode_message(event).size() > impl_->config.max_wire_bytes;
        if (wire_truncated) {
            auto bounded = choc::value::createObject("");
            bounded.addMember("schema",
                              choc::value::createString("pulp.inspect.telemetry.sample.v1"));
            bounded.addMember("schemaVersion", choc::value::createInt64(1));
            bounded.addMember("kind", choc::value::createString("sample"));
            bounded.addMember(
                "sourceGeneration",
                choc::value::createInt64(static_cast<std::int64_t>(impl_->source_generation)));
            bounded.addMember("attemptSequence", choc::value::createInt64(static_cast<std::int64_t>(
                                                     subscription.attempt_sequence)));
            bounded.addMember("subscriptionId", choc::value::createString(subscription.id));
            bounded.addMember("sampledAtNs", choc::value::createInt64(time_ns(sampled_at)));
            bounded.addMember("requestedRateHz", subscription.requested_rate_hz);
            bounded.addMember("effectiveRateHz", subscription.effective_rate_hz);
            bounded.addMember("maxVectorValues", choc::value::createInt64(static_cast<std::int64_t>(
                                                     subscription.max_vector_values)));
            bounded.addMember("transportDroppedSincePrevious",
                              choc::value::createInt64(
                                  static_cast<std::int64_t>(subscription.transport_loss_debt)));
            bounded.addMember("reattached",
                              choc::value::createBool(subscription.reattached_pending));
            bounded.addMember("wireTruncated", choc::value::createBool(true));
            bounded.addMember("terminal", choc::value::createBool(true));
            bounded.addMember("terminalReason", choc::value::createString("wire_limit"));
            bounded.addMember("channelCount", choc::value::createInt64(0));
            bounded.addMember("channels", choc::value::createEmptyArray());
            params = choc::json::toString(bounded, false);
            event = make_event(methods::kTelemetrySample, params);
        }

        const auto result = impl_->sink ? impl_->sink(iterator->first, event, subscription.id)
                                        : InspectorTargetedEventResult::EventUnavailable;
        const bool queued = result == InspectorTargetedEventResult::Queued ||
                            result == InspectorTargetedEventResult::QueuedAfterLossyEviction;
        if (queued) {
            impl_->commit_delivery(subscription);
            subscription.transport_loss_debt =
                result == InspectorTargetedEventResult::QueuedAfterLossyEviction ? 1 : 0;
        } else {
            ++subscription.transport_loss_debt;
        }
        if ((wire_truncated && queued) || result == InspectorTargetedEventResult::ClientNotFound ||
            result == InspectorTargetedEventResult::ReliableOverflow ||
            result == InspectorTargetedEventResult::MessageTooLarge) {
            iterator = impl_->subscriptions.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

void ValueChannelTelemetryBroker::disconnect(std::string_view client_id) {
    impl_->subscriptions.erase(std::string(client_id));
}

std::size_t ValueChannelTelemetryBroker::subscription_count() const noexcept {
    return impl_->subscriptions.size();
}

} // namespace pulp::inspect
