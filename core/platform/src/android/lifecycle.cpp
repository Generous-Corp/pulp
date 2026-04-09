#if defined(__ANDROID__)

#include <android/log.h>

#define PULP_LOG_TAG "Pulp"
#define PULP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, PULP_LOG_TAG, __VA_ARGS__)

namespace pulp::android {

// Tiered memory release for Low Memory Killer survival.
// Called from Java_com_pulp_PulpActivity_nativeOnMemoryPressure.
//
// Level 0 (moderate): App went to background. Release non-essential caches.
// Level 1 (aggressive): System under memory pressure. Drop textures, samples.
// Level 2 (emergency): About to be killed. Release everything non-essential.
void on_memory_pressure(int level) {
    PULP_LOGI("on_memory_pressure: level=%d", level);

    switch (level) {
    case 0:
        // Moderate — app just backgrounded
        // TODO: Purge Skia Graphite shader compilation cache
        // TODO: DrawBatcher::instance().flush_and_release()
        break;

    case 1:
        // Aggressive — system running low
        // TODO: TextureAtlas::instance().evict_unused_pages()
        // TODO: SampleCache::instance().release_unused()
        break;

    case 2:
        // Emergency — about to be killed
        // TODO: Release all GPU caches, sample buffers, pre-allocated DSP buffers
        break;
    }
}

} // namespace pulp::android

#endif // __ANDROID__
