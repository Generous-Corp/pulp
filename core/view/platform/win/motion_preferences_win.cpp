#ifdef _WIN32
#include <pulp/view/motion_preferences.hpp>
#include <windows.h>

namespace pulp::view::platform {

MotionPolicy detect_win_motion_policy() {
    // SPI_GETCLIENTAREAANIMATION: TRUE when client-area animations are
    // enabled; FALSE when the user has asked the OS to disable them.
    BOOL animations_enabled = TRUE;
    BOOL ok = SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0,
                                    &animations_enabled, 0);
    if (!ok) return MotionPolicy::Full;
    return animations_enabled ? MotionPolicy::Full : MotionPolicy::Reduced;
}

} // namespace pulp::view::platform
#endif
