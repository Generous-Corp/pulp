#include <pulp/format/ios_audio_session.hpp>

#include <pulp/runtime/log.hpp>

namespace pulp::format {
namespace {

IosAudioSessionListener& current_listener() {
    static IosAudioSessionListener listener;
    return listener;
}

} // namespace

void set_ios_audio_session_listener(IosAudioSessionListener listener) {
    current_listener() = std::move(listener);
}

bool emit_ios_audio_session_event(const PulpIosAudioSessionEvent& event) {
    const auto& l = current_listener();
    if (!l) return false;
    l(event);
    return true;
}

std::string_view to_string(PulpIosAudioEvent event) {
    switch (event) {
        case PULP_IOS_AUDIO_EVENT_NONE:               return "none";
        case PULP_IOS_AUDIO_EVENT_INTERRUPTION_BEGAN: return "interruption_began";
        case PULP_IOS_AUDIO_EVENT_INTERRUPTION_ENDED: return "interruption_ended";
        case PULP_IOS_AUDIO_EVENT_ROUTE_CHANGED:      return "route_changed";
        case PULP_IOS_AUDIO_EVENT_MEDIA_SERVICES_RESET: return "media_services_reset";
        case PULP_IOS_AUDIO_EVENT_SILENCE_SECONDARY_AUDIO_BEGAN:
            return "silence_secondary_audio_began";
        case PULP_IOS_AUDIO_EVENT_SILENCE_SECONDARY_AUDIO_ENDED:
            return "silence_secondary_audio_ended";
    }
    return "unknown";
}

} // namespace pulp::format

// ── C ABI entry points ────────────────────────────────────────────────────

extern "C" {

static PulpIosAudioSessionCallback g_callback = nullptr;
static void* g_user_data = nullptr;

void pulp_ios_audio_session_set_callback(PulpIosAudioSessionCallback cb,
                                         void* user_data) {
    g_callback = cb;
    g_user_data = user_data;
}

void pulp_ios_audio_session_emit(const PulpIosAudioSessionEvent* event) {
    if (!event) return;
    // Prefer the C++ listener when set — it's the modern path. The C
    // callback remains as a lower-level hook for non-C++ consumers (e.g.
    // tests written in plain C).
    if (pulp::format::emit_ios_audio_session_event(*event)) return;
    if (g_callback) g_callback(event, g_user_data);
}

}  // extern "C"
