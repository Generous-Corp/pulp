#include <pulp/view/value_channel_set.hpp>

#include <algorithm>

namespace pulp::view {

ValueChannelSet::~ValueChannelSet() {
    detail::value_channel_telemetry_index_release(telemetry_control_.get());
}

std::uint64_t ValueChannelSet::generation_identity() const noexcept {
    return detail::value_channel_telemetry_control_identity(
        telemetry_control_.get());
}

const char* ValueChannelSet::describe(DeclareError e) noexcept {
    switch (e) {
        case DeclareError::ok: return "declared";
        case DeclareError::empty_name: return "channel name was empty";
        case DeclareError::duplicate_name: return "a channel with that name is already declared";
        case DeclareError::reserved_character:
            return "channel name contains ':', which is reserved for the value: prefix";
    }
    return "unknown declaration error";
}

ValueChannelSet::Entry* ValueChannelSet::add_entry(std::string name, std::string unit,
                                                   float neutral, ValueChannelShape shape,
                                                   DeclareError* error) {
    const auto fail = [&](DeclareError e) -> Entry* {
        if (error) *error = e;
        return nullptr;
    };

    if (name.empty()) return fail(DeclareError::empty_name);
    // ':' separates the "value:" namespace from the key on the JS side, so a
    // name containing one would resolve ambiguously there.
    if (name.find(':') != std::string::npos) return fail(DeclareError::reserved_character);
    const auto clash = std::find_if(infos_.begin(), infos_.end(),
                                    [&](const ValueChannelInfo& i) { return i.name == name; });
    if (clash != infos_.end()) return fail(DeclareError::duplicate_name);

    // Allocate the existing control sidecar before mutating the ordered
    // declaration vectors. This keeps a failed setup allocation transactional.
    if (!telemetry_control_)
        telemetry_control_ = detail::make_value_channel_telemetry_control();
    infos_.push_back(ValueChannelInfo{std::move(name), std::move(unit), shape, neutral});
    try {
        entries_.push_back(std::make_unique<Entry>());
        detail::value_channel_telemetry_index_add(telemetry_control_.get(), infos_.back().name,
                                                  shape, infos_.size() - 1);
    } catch (...) {
        if (entries_.size() >= infos_.size())
            entries_.pop_back();
        infos_.pop_back();
        throw;
    }
    if (error) *error = DeclareError::ok;
    return entries_.back().get();
}

ScalarSource* ValueChannelSet::declare_scalar(std::string name, std::string unit, float neutral,
                                              DeclareError* error) {
    auto* entry = add_entry(std::move(name), std::move(unit), neutral,
                            ValueChannelShape::scalar, error);
    if (!entry) return nullptr;
    entry->telemetry = detail::make_scalar_telemetry_state();
    entry->scalar = std::make_unique<ScalarSource>();
    entry->scalar->telemetry_ = detail::scalar_telemetry_writer(entry->telemetry.get());
    return entry->scalar.get();
}

MeterSource* ValueChannelSet::declare_meter(std::string name, std::string unit, float neutral,
                                            DeclareError* error) {
    auto* entry = add_entry(std::move(name), std::move(unit), neutral,
                            ValueChannelShape::meter, error);
    if (!entry) return nullptr;
    entry->telemetry = detail::make_meter_telemetry_state();
    entry->meter = std::make_unique<MeterSource>();
    entry->meter->telemetry_ = detail::meter_telemetry_writer(entry->telemetry.get());
    return entry->meter.get();
}

VectorSource* ValueChannelSet::declare_vector(std::string name, std::string unit, float neutral,
                                              DeclareError* error) {
    auto* entry = add_entry(std::move(name), std::move(unit), neutral,
                            ValueChannelShape::vector, error);
    if (!entry) return nullptr;
    entry->telemetry = detail::make_vector_telemetry_state();
    entry->vector = std::make_unique<VectorSource>();
    entry->vector->telemetry_ = detail::vector_telemetry_writer(entry->telemetry.get());
    return entry->vector.get();
}

EventSource* ValueChannelSet::declare_events(std::string name, std::string unit,
                                             DeclareError* error) {
    auto* entry = add_entry(std::move(name), std::move(unit), 0.0f,
                            ValueChannelShape::events, error);
    if (!entry) return nullptr;
    entry->telemetry = detail::make_event_telemetry_state();
    entry->events = std::make_unique<EventSource>();
    entry->events->telemetry_ = detail::event_telemetry_writer(entry->telemetry.get());
    return entry->events.get();
}

std::ptrdiff_t ValueChannelSet::index_of(std::string_view name,
                                         ValueChannelShape shape) const {
    // Exact match remains deliberate: a shape mismatch is a miss rather than
    // a wrong-typed hit, so a binding can never silently read another source.
    const auto indexed =
        detail::value_channel_telemetry_index_lookup(telemetry_control_.get(), name, shape);
    if (indexed >= 0)
        return indexed;
    // A control created by an older SDK has no side-table entry. Keep the
    // source-compatible behavior correct for that case; new declarations use
    // the O(1) index above and only misses pay this compatibility scan.
    for (std::size_t i = 0; i < infos_.size(); ++i) {
        if (infos_[i].name == name && infos_[i].shape == shape)
            return static_cast<std::ptrdiff_t>(i);
    }
    return -1;
}

ScalarSource* ValueChannelSet::scalar(std::string_view name) const {
    const auto i = index_of(name, ValueChannelShape::scalar);
    return i < 0 ? nullptr : entries_[static_cast<std::size_t>(i)]->scalar.get();
}

MeterSource* ValueChannelSet::meter(std::string_view name) const {
    const auto i = index_of(name, ValueChannelShape::meter);
    return i < 0 ? nullptr : entries_[static_cast<std::size_t>(i)]->meter.get();
}

VectorSource* ValueChannelSet::vector(std::string_view name) const {
    const auto i = index_of(name, ValueChannelShape::vector);
    return i < 0 ? nullptr : entries_[static_cast<std::size_t>(i)]->vector.get();
}

EventSource* ValueChannelSet::events(std::string_view name) const {
    const auto i = index_of(name, ValueChannelShape::events);
    return i < 0 ? nullptr : entries_[static_cast<std::size_t>(i)]->events.get();
}

ValueChannelTelemetryAttachment ValueChannelSet::attach_telemetry() const {
    if (!telemetry_control_)
        return {};
    std::vector<std::shared_ptr<detail::ValueChannelTelemetryState>> states;
    states.reserve(entries_.size());
    for (const auto& entry : entries_)
        states.push_back(entry->telemetry);
    return ValueChannelTelemetryAttachment(telemetry_control_, std::move(states), infos_);
}

}  // namespace pulp::view
