// state_inspector.cpp — Parameter state monitoring and editing

#include <pulp/inspect/state_inspector.hpp>

namespace pulp::inspect {

StateInspector::StateInspector(StateStore& store)
    : store_(store)
    , alive_(std::make_shared<bool>(true))
{
    // Register listener with weak_ptr guard for safe destruction
    std::weak_ptr<bool> weak = alive_;
    store_.add_listener([this, weak](ParamID id, float value) {
        auto locked = weak.lock();
        if (!locked) return;  // inspector destroyed

        ParamChange change{id, value, std::chrono::steady_clock::now()};
        std::lock_guard lock(changes_mutex_);
        changes_.push_back(change);
        if (changes_.size() > kMaxChanges)
            changes_.erase(changes_.begin());
    });
}

StateInspector::~StateInspector() {
    // Invalidate the weak_ptr guard — listener will no-op after this
    alive_.reset();
}

std::vector<StateInspector::ParamSnapshot> StateInspector::all_params() const {
    std::vector<ParamSnapshot> result;
    auto params = store_.all_params();
    result.reserve(params.size());

    for (auto& info : params) {
        ParamSnapshot snap;
        snap.id = info.id;
        snap.name = info.name;
        snap.unit = info.unit;
        snap.value = store_.get_value(info.id);
        snap.normalized = store_.get_normalized(info.id);
        snap.modulated = store_.get_modulated(info.id);
        snap.default_value = info.range.default_value;
        snap.min = info.range.min;
        snap.max = info.range.max;
        snap.step = info.range.step;
        snap.display_value = info.to_string ? info.to_string(snap.value) : "";
        result.push_back(std::move(snap));
    }

    return result;
}

std::vector<StateInspector::ParamChange> StateInspector::recent_changes() const {
    std::lock_guard lock(changes_mutex_);
    return changes_;
}

void StateInspector::set_param(ParamID id, float value) {
    store_.set_value(id, value);
}

} // namespace pulp::inspect
