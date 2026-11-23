#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum PulpInteractionPhase : uint8_t {
    PulpInteractionPhaseInvalid = 0,
    PulpInteractionPhasePress = 1,
    PulpInteractionPhaseHold = 2,
    PulpInteractionPhaseMove = 3,
    PulpInteractionPhaseRelease = 4,
    PulpInteractionPhaseCancel = 5
} PulpInteractionPhase;
typedef struct PulpInteractionEvent {
    uint32_t source_id, epoch, object_id;
    uint64_t sequence, provider_time_ns;
    PulpInteractionPhase phase;
    float x, y, confidence;
} PulpInteractionEvent;
bool pulp_interaction_normalize(float point_x, float point_y, float width, float height, float* x,
                                float* y);
bool pulp_interaction_validate(const PulpInteractionEvent* event);
#ifdef __cplusplus
}
#endif
#ifdef __cplusplus
extern "C" {
#endif
void* pulp_interaction_recorder_create(void);
void pulp_interaction_recorder_destroy(void* recorder);
bool pulp_interaction_recorder_record(void* recorder, const PulpInteractionEvent* events,
                                      uint8_t count);
uint8_t pulp_interaction_recorder_count(void* recorder);
bool pulp_interaction_recorder_event(void* recorder, uint8_t batch, uint8_t index,
                                     PulpInteractionEvent* out_event);
#ifdef __cplusplus
}
#endif
