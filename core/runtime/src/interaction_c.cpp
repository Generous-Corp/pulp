#include <pulp/runtime/interaction_c.h>

#include <pulp/runtime/interaction_event.hpp>
#include <pulp/runtime/interaction_router.hpp>

#include <cstddef>
#include <cstring>
#include <new>
#include <type_traits>

namespace {

using pulp::runtime::InteractionEvent;

// The C struct is copied byte for byte into the C++ event, so the two must
// agree on size and on every field's offset and width.
#define PULP_INTERACTION_C_FIELD(field)                                                            \
    static_assert(offsetof(PulpInteractionEvent, field) == offsetof(InteractionEvent, field),      \
                  #field " offset differs between the C and C++ interaction events");              \
    static_assert(sizeof(PulpInteractionEvent::field) == sizeof(InteractionEvent::field),          \
                  #field " size differs between the C and C++ interaction events")
PULP_INTERACTION_C_FIELD(source_id);
PULP_INTERACTION_C_FIELD(epoch);
PULP_INTERACTION_C_FIELD(sequence);
PULP_INTERACTION_C_FIELD(provider_time_ns);
PULP_INTERACTION_C_FIELD(object_id);
PULP_INTERACTION_C_FIELD(phase);
PULP_INTERACTION_C_FIELD(reserved);
PULP_INTERACTION_C_FIELD(x);
PULP_INTERACTION_C_FIELD(y);
PULP_INTERACTION_C_FIELD(strength);
PULP_INTERACTION_C_FIELD(confidence);
#undef PULP_INTERACTION_C_FIELD
static_assert(sizeof(PulpInteractionEvent) == sizeof(InteractionEvent));
static_assert(alignof(PulpInteractionEvent) == alignof(InteractionEvent));
static_assert(std::is_standard_layout_v<PulpInteractionEvent>);
static_assert(PULP_INTERACTION_PHASE_PRESS ==
              static_cast<int>(pulp::runtime::InteractionPhase::press));
static_assert(PULP_INTERACTION_PHASE_HOLD ==
              static_cast<int>(pulp::runtime::InteractionPhase::hold));
static_assert(PULP_INTERACTION_PHASE_MOVE ==
              static_cast<int>(pulp::runtime::InteractionPhase::move));
static_assert(PULP_INTERACTION_PHASE_RELEASE ==
              static_cast<int>(pulp::runtime::InteractionPhase::release));
static_assert(PULP_INTERACTION_PHASE_CANCEL ==
              static_cast<int>(pulp::runtime::InteractionPhase::cancel));
static_assert(PULP_INTERACTION_PHASE_INVALID ==
              static_cast<int>(pulp::runtime::InteractionPhase::invalid));

InteractionEvent from_c(const PulpInteractionEvent& event) noexcept {
    InteractionEvent out;
    std::memcpy(&out, &event, sizeof(out));
    return out;
}

void to_c(const InteractionEvent& event, PulpInteractionEvent& out) noexcept {
    std::memcpy(&out, &event, sizeof(out));
}

} // namespace

struct PulpInteractionRouter {
    explicit PulpInteractionRouter(std::uint64_t timeout_ns) : router(timeout_ns) {}
    pulp::runtime::InteractionRouter<> router;
};

extern "C" {

PulpInteractionRouter* pulp_interaction_router_create(uint64_t source_timeout_ns) {
    return new (std::nothrow) PulpInteractionRouter(source_timeout_ns);
}

void pulp_interaction_router_destroy(PulpInteractionRouter* router) {
    delete router;
}

bool pulp_interaction_publish(PulpInteractionRouter* router, const PulpInteractionEvent* event) {
    if (router == nullptr || event == nullptr)
        return false;
    return router->router.publish(from_c(*event));
}

bool pulp_interaction_check_timeout(PulpInteractionRouter* router, uint64_t now_ns) {
    return router != nullptr && router->router.check_timeout(now_ns);
}

void pulp_interaction_reset(PulpInteractionRouter* router) {
    if (router != nullptr)
        router->router.reset();
}

bool pulp_interaction_pop_ui(PulpInteractionRouter* router, PulpInteractionEvent* out) {
    if (router == nullptr || out == nullptr)
        return false;
    InteractionEvent event;
    if (!router->router.try_pop_ui(event))
        return false;
    to_c(event, *out);
    return true;
}

bool pulp_interaction_pop_audio(PulpInteractionRouter* router, PulpInteractionEvent* out) {
    if (router == nullptr || out == nullptr)
        return false;
    InteractionEvent event;
    if (!router->router.try_pop_audio(event))
        return false;
    to_c(event, *out);
    return true;
}

} // extern "C"
