#include <pulp/view/value_channel_set.hpp>

#include <algorithm>

namespace pulp::view {

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

    infos_.push_back(ValueChannelInfo{std::move(name), std::move(unit), shape, neutral});
    entries_.push_back(std::make_unique<Entry>());
    if (error) *error = DeclareError::ok;
    return entries_.back().get();
}

ScalarSource* ValueChannelSet::declare_scalar(std::string name, std::string unit, float neutral,
                                              DeclareError* error) {
    auto* entry = add_entry(std::move(name), std::move(unit), neutral,
                            ValueChannelShape::scalar, error);
    if (!entry) return nullptr;
    entry->scalar = std::make_unique<ScalarSource>();
    return entry->scalar.get();
}

MeterSource* ValueChannelSet::declare_meter(std::string name, std::string unit, float neutral,
                                            DeclareError* error) {
    auto* entry = add_entry(std::move(name), std::move(unit), neutral,
                            ValueChannelShape::meter, error);
    if (!entry) return nullptr;
    entry->meter = std::make_unique<MeterSource>();
    return entry->meter.get();
}

VectorSource* ValueChannelSet::declare_vector(std::string name, std::string unit, float neutral,
                                              DeclareError* error) {
    auto* entry = add_entry(std::move(name), std::move(unit), neutral,
                            ValueChannelShape::vector, error);
    if (!entry) return nullptr;
    entry->vector = std::make_unique<VectorSource>();
    return entry->vector.get();
}

EventSource* ValueChannelSet::declare_events(std::string name, std::string unit,
                                             DeclareError* error) {
    auto* entry = add_entry(std::move(name), std::move(unit), 0.0f,
                            ValueChannelShape::events, error);
    if (!entry) return nullptr;
    entry->events = std::make_unique<EventSource>();
    return entry->events.get();
}

std::ptrdiff_t ValueChannelSet::index_of(std::string_view name,
                                         ValueChannelShape shape) const {
    for (std::size_t i = 0; i < infos_.size(); ++i) {
        // Exact match, deliberately — see the header on why a lookup key is not
        // canonicalized. A shape mismatch is a miss rather than a wrong-typed
        // hit, so binding a scope to a meter fails at bind time.
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

}  // namespace pulp::view
