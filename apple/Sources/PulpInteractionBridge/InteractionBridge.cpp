#include "InteractionBridge.h"
#include <pulp/runtime/interaction_event.hpp>

extern "C" bool pulp_interaction_normalize(float point_x, float point_y, float width, float height,
                                           float* x, float* y) {
    if (!x || !y)
        return false;
    return pulp::runtime::normalize_coordinates(point_x, point_y, width, height, *x, *y);
}

extern "C" bool pulp_interaction_validate(const PulpInteractionEvent* event) {
    if (!event)
        return false;
    const auto phase = static_cast<pulp::runtime::InteractionPhase>(event->phase);
    const pulp::runtime::InteractionEvent cpp_event{
        event->source_id, event->epoch, event->sequence, event->provider_time_ns,
        event->object_id, phase,        event->x,        event->y,
        event->confidence};
    return pulp::runtime::validate_event(cpp_event);
}
#include <pulp/runtime/interaction_replay.hpp>
struct PulpInteractionRecorder {
    pulp::runtime::InteractionRecorder recorder;
};
extern "C" void* pulp_interaction_recorder_create(void) {
    return new PulpInteractionRecorder;
}
extern "C" void pulp_interaction_recorder_destroy(void* ptr) {
    delete static_cast<PulpInteractionRecorder*>(ptr);
}
extern "C" bool pulp_interaction_recorder_record(void* ptr, const PulpInteractionEvent* events,
                                                 uint8_t count) {
    if (!ptr || !events || count == 0 || count > 16)
        return false;
    // A batch is recorded whole or not at all.
    pulp::runtime::InteractionBatch<> batch;
    for (uint8_t i = 0; i < count; ++i) {
        const auto& event = events[i];
        if (!pulp_interaction_validate(&event))
            return false;
        const auto phase = static_cast<pulp::runtime::InteractionPhase>(event.phase);
        if (!batch.append({event.source_id, event.epoch, event.sequence, event.provider_time_ns,
                           event.object_id, phase, event.x, event.y, event.confidence}))
            return false;
    }
    static_cast<PulpInteractionRecorder*>(ptr)->recorder.record(batch);
    return true;
}
extern "C" uint8_t pulp_interaction_recorder_count(void* ptr) {
    return ptr ? static_cast<uint8_t>(static_cast<PulpInteractionRecorder*>(ptr)->recorder.size())
               : 0;
}
extern "C" bool pulp_interaction_recorder_event(void* ptr, uint8_t batchIndex, uint8_t eventIndex,
                                                PulpInteractionEvent* out) {
    if (!ptr || !out)
        return false;
    uint8_t current = 0;
    bool found = false;
    static_cast<PulpInteractionRecorder*>(ptr)->recorder.replay([&](const auto& batch) {
        if (found || current++ != batchIndex || eventIndex >= batch.count)
            return;
        const auto& e = batch.events[eventIndex];
        *out = {e.source_id,
                e.epoch,
                e.object_id,
                e.sequence,
                e.provider_time_ns,
                static_cast<PulpInteractionPhase>(e.phase),
                e.x,
                e.y,
                e.confidence};
        found = true;
    });
    return found;
}
