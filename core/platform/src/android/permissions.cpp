#if defined(__ANDROID__)

#include "permissions_internal.hpp"

#include <pulp/platform/android/jni.hpp>
#include <android/log.h>
#include <stdexcept>

#define PULP_LOG_TAG "Pulp"
#define PULP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, PULP_LOG_TAG, __VA_ARGS__)

namespace pulp::android {

// Permission request callbacks from Kotlin.
// The actual permission request UI is handled by Kotlin's ActivityResultContracts.
// These JNI callbacks notify the C++ side of the result. The cross-platform
// pulp::platform permissions backend (permissions_backend.cpp) subscribes
// here via set_permission_callback().

static PermissionCallback g_permission_callback = nullptr;

void set_permission_callback(PermissionCallback cb) {
    g_permission_callback = cb;
}

} // namespace pulp::android

extern "C" JNIEXPORT void JNICALL
Java_com_pulp_PulpActivity_nativeOnPermissionResult(
    JNIEnv* env, jobject thiz, jint permission, jboolean granted) {
    try {
        PULP_LOGI("Permission result: %d granted=%d", permission, granted);
        if (pulp::android::g_permission_callback) {
            pulp::android::g_permission_callback(
                static_cast<pulp::android::Permission>(permission), granted);
        }
    } catch (const std::exception& e) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), e.what());
    } catch (...) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"),
                      "Unknown C++ exception in nativeOnPermissionResult");
    }
}

#endif // __ANDROID__
