#pragma once

// Sample-accurate parameter-event queue passed into PluginSlot::process()
// each block. Produced by SignalGraph's automation pass (Phase 1E);
// consumed by each format loader and translated into the plugin's native
// per-block event stream (CLAP_EVENT_PARAM_VALUE, VST3 IParameterChanges,
// AudioUnitScheduleParameters, LV2 control-port writes).
//
// Ordering contract: events are sorted by sample_offset ascending before
// being handed to the plugin. Callers that append events unordered must
// call sort() before passing the queue.

#include <algorithm>
#include <cstdint>
#include <vector>

namespace pulp::host {

struct ParameterEvent {
    uint32_t param_id  = 0;
    int32_t  sample_offset = 0;   // 0..num_samples-1 within the current block
    float    value     = 0.0f;    // plain parameter domain (see HostParamInfo)
};

class ParameterEventQueue {
public:
    ParameterEventQueue() = default;

    void push(const ParameterEvent& e) { events_.push_back(e); }
    void push(ParameterEvent&& e)      { events_.push_back(std::move(e)); }

    void clear() { events_.clear(); }
    bool empty() const { return events_.empty(); }
    std::size_t size() const { return events_.size(); }

    void sort() {
        std::sort(events_.begin(), events_.end(),
            [](const ParameterEvent& a, const ParameterEvent& b) {
                return a.sample_offset < b.sample_offset;
            });
    }

    auto begin()       { return events_.begin(); }
    auto end()         { return events_.end(); }
    auto begin() const { return events_.begin(); }
    auto end()   const { return events_.end(); }

    const std::vector<ParameterEvent>& events() const { return events_; }

private:
    std::vector<ParameterEvent> events_;
};

} // namespace pulp::host
