#include <pulp/vellum/param_port_model.hpp>

#include <pulp/vellum/mapping.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace pulp::vellum {

using ::vellum::value::ParamHandle;
using ::vellum::value::Subscription;

struct ParamPortModel::ListenerSet {
    struct Entry {
        std::uint64_t id = 0;
        std::shared_ptr<Listener> listener;
    };

    std::uint64_t next_id = 1;
    std::vector<Entry> entries;

    [[nodiscard]] bool contains(std::uint64_t id) const noexcept {
        return std::any_of(entries.begin(), entries.end(),
                           [id](const Entry& e) { return e.id == id; });
    }
};

ParamPortModel::ParamPortModel(format::ParamPort& port, Options options)
    : port_(port), options_(std::move(options)), listeners_(std::make_shared<ListenerSet>()) {
    const auto params = port_.params();
    entries_.reserve(params.size());
    for (const auto& info : params) {
        const auto index = static_cast<std::uint32_t>(entries_.size());
        entries_.push_back({&info, 0});
        index_by_id_.emplace(info.id, index);
        if (options_.resolver)
            continue;
        const auto claim = [&](std::string key) {
            if (!index_by_key_.emplace(std::move(key), index).second)
                throw std::invalid_argument("ParamPortModel: duplicate parameter key");
        };
        if (!info.name.empty())
            claim(info.name);
        claim(std::to_string(info.id));
    }
    port_subscription_ =
        port_.add_listener([this](state::ParamID id, float value) { on_port_change(id, value); });
}

ParamPortModel::~ParamPortModel() {
    // Close what this model opened, so a destroyed editor never leaves the host
    // with an open gesture. A disconnected port ignores the call.
    for (auto& entry : entries_) {
        if (entry.gesture_depth > 0) {
            entry.gesture_depth = 0;
            port_.end(entry.info->id);
        }
    }
}

ParamPortModel::Entry* ParamPortModel::find(ParamHandle param) noexcept {
    return param.valid() && param.owner == this && param.id < entries_.size() ? &entries_[param.id]
                                                                              : nullptr;
}

const ParamPortModel::Entry* ParamPortModel::find(ParamHandle param) const noexcept {
    return param.valid() && param.owner == this && param.id < entries_.size() ? &entries_[param.id]
                                                                              : nullptr;
}

ParamHandle ParamPortModel::resolve(std::string_view key) {
    if (options_.resolver) {
        const auto id = options_.resolver(key);
        if (!id)
            return {};
        const auto it = index_by_id_.find(*id);
        return it == index_by_id_.end() ? ParamHandle{} : ParamHandle{it->second, this};
    }
    const auto it = index_by_key_.find(key);
    return it == index_by_key_.end() ? ParamHandle{} : ParamHandle{it->second, this};
}

std::optional<state::ParamID> ParamPortModel::param_id(ParamHandle param) const noexcept {
    const Entry* e = find(param);
    if (!e)
        return std::nullopt;
    return e->info->id;
}

double ParamPortModel::normalized(ParamHandle param) {
    const Entry* e = find(param);
    return e ? to_normalized(*e->info, port_.get(e->info->id)) : 0.0;
}

void ParamPortModel::set_normalized(ParamHandle param, double normalized) {
    Entry* e = find(param);
    if (!e || std::isnan(normalized))
        return;
    const float plain = to_plain(*e->info, normalized);
    // The subscriber being told about a host change set the value it was
    // handed; writing it back would record the host's value as an editor edit.
    if (delivering_ == param.id && plain == port_.get(e->info->id))
        return;
    port_.set(e->info->id, plain);
}

void ParamPortModel::begin(ParamHandle param) {
    Entry* e = find(param);
    if (!e)
        return;
    if (e->gesture_depth++ == 0)
        port_.begin(e->info->id);
}

void ParamPortModel::end(ParamHandle param) {
    Entry* e = find(param);
    if (!e || e->gesture_depth == 0)
        return;
    if (--e->gesture_depth == 0)
        port_.end(e->info->id);
}

std::string ParamPortModel::display_text(ParamHandle param, double normalized) {
    const Entry* e = find(param);
    if (!e)
        return {};
    const float plain =
        std::isnan(normalized) ? port_.get(e->info->id) : to_plain(*e->info, normalized);
    return format_plain(*e->info, plain);
}

int ParamPortModel::value_count(ParamHandle param) {
    const Entry* e = find(param);
    return e ? ::pulp::vellum::value_count(*e->info) : 0;
}

::vellum::value::ParamInfo ParamPortModel::info(ParamHandle param) {
    const Entry* e = find(param);
    if (!e)
        return {};
    return {e->info->name, e->info->unit, to_normalized(*e->info, e->info->range.default_value),
            ::pulp::vellum::value_count(*e->info)};
}

double ParamPortModel::plain(ParamHandle param, double normalized) {
    const Entry* e = find(param);
    if (!e || std::isnan(normalized))
        return 0.0;
    return static_cast<double>(to_plain(*e->info, normalized));
}

double ParamPortModel::normalized_from_plain(ParamHandle param, double plain) {
    const Entry* e = find(param);
    if (!e || std::isnan(plain))
        return 0.0;
    return to_normalized(*e->info, static_cast<float>(plain));
}

std::optional<double> ParamPortModel::parse_text(ParamHandle param, std::string_view text) {
    const Entry* e = find(param);
    if (!e)
        return std::nullopt;
    return parse_normalized(*e->info, text);
}

std::optional<double> ParamPortModel::modulated_normalized(ParamHandle param) {
    const Entry* e = find(param);
    if (!e || !options_.modulation)
        return std::nullopt;
    const auto shown = options_.modulation(e->info->id);
    if (!shown || std::isnan(*shown))
        return std::nullopt;
    return std::clamp(static_cast<double>(*shown), 0.0, 1.0);
}

void ParamPortModel::on_port_change(state::ParamID id, float plain_value) {
    const auto it = index_by_id_.find(id);
    if (it == index_by_id_.end())
        return;
    const std::uint32_t index = it->second;
    const double value = to_normalized(*entries_[index].info, plain_value);

    // Deliver to a snapshot so a listener may subscribe or unsubscribe during
    // delivery; one removed mid-delivery is not called afterwards.
    const auto keep_alive = listeners_;
    const std::vector<ListenerSet::Entry> snapshot = keep_alive->entries;
    const auto previous = std::exchange(delivering_, index);
    for (const auto& entry : snapshot) {
        if (keep_alive->contains(entry.id))
            (*entry.listener)(ParamHandle{index, this}, value);
    }
    delivering_ = previous;
}

Subscription ParamPortModel::subscribe(Listener listener) {
    if (!listener)
        return {};
    const std::uint64_t id = listeners_->next_id++;
    listeners_->entries.push_back({id, std::make_shared<Listener>(std::move(listener))});
    // The registration may outlive the model; cancelling it then does nothing.
    return Subscription([weak = std::weak_ptr<ListenerSet>(listeners_), id] {
        if (const auto set = weak.lock())
            std::erase_if(set->entries, [id](const ListenerSet::Entry& e) { return e.id == id; });
    });
}

} // namespace pulp::vellum
