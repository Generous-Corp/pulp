#ifndef PULP_RUNTIME_INTERACTION_C_H
#define PULP_RUNTIME_INTERACTION_C_H

// C ABI over pulp::runtime::InteractionRouter, for providers written in Swift
// or any other language that can call C.
//
// A provider only forwards: the router's tracker owns ordering, epochs,
// validation and the cancels that follow an epoch change, a repeated press or
// a timeout. Nothing here, and nothing a caller layers on top, restates those
// rules. Platform lifecycle and permissions stay with the application.
//
// PulpInteractionEvent has the layout of pulp::runtime::InteractionEvent
// (48 bytes); interaction_c.cpp asserts every size and offset.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum PulpInteractionPhase {
    PULP_INTERACTION_PHASE_PRESS = 0,
    PULP_INTERACTION_PHASE_HOLD = 1,
    PULP_INTERACTION_PHASE_MOVE = 2,
    PULP_INTERACTION_PHASE_RELEASE = 3,
    PULP_INTERACTION_PHASE_CANCEL = 4,
    PULP_INTERACTION_PHASE_INVALID = 5,
} PulpInteractionPhase;

typedef struct PulpInteractionEvent {
    uint32_t source_id;
    uint32_t epoch;
    uint64_t sequence;
    uint64_t provider_time_ns;
    uint32_t object_id;
    uint8_t phase; // a PulpInteractionPhase value
    uint8_t reserved[3];
    float x;
    float y;
    float strength;
    float confidence;
} PulpInteractionEvent;

typedef struct PulpInteractionRouter PulpInteractionRouter;

/// Creates a router. `source_timeout_ns` of 0 disables the idle timeout.
/// Returns NULL on allocation failure. Release with pulp_interaction_router_destroy.
PulpInteractionRouter* pulp_interaction_router_create(uint64_t source_timeout_ns);
void pulp_interaction_router_destroy(PulpInteractionRouter* router);

/// Offers one event to the tracker. Returns true when it was accepted and every
/// resulting event reached both consumers; false when it was refused or a
/// consumer queue was full. Provider thread only.
bool pulp_interaction_publish(PulpInteractionRouter* router, const PulpInteractionEvent* event);

/// Cancels active interactions that have been idle past the timeout. Returns
/// true when any were cancelled. Provider thread only.
bool pulp_interaction_check_timeout(PulpInteractionRouter* router, uint64_t now_ns);

/// Cancels everything active and forgets the source.
void pulp_interaction_reset(PulpInteractionRouter* router);

/// Pops the next event for the UI or audio consumer. Each consumer thread must
/// be the only one calling its pop. Returns false when none is waiting.
bool pulp_interaction_pop_ui(PulpInteractionRouter* router, PulpInteractionEvent* out);
bool pulp_interaction_pop_audio(PulpInteractionRouter* router, PulpInteractionEvent* out);

#ifdef __cplusplus
}
#endif

#endif // PULP_RUNTIME_INTERACTION_C_H
