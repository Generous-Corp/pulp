#if defined(__ANDROID__)

#include <pulp/platform/android/jni.hpp>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <stdexcept>
#include <string>
#include <vector>

#define PULP_LOG_TAG "Pulp"
#define PULP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, PULP_LOG_TAG, __VA_ARGS__)
#define PULP_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, PULP_LOG_TAG, __VA_ARGS__)

namespace pulp::android {

// ── Asset Reader ──────────────────────────────────────────────────────────
// Reads bundled assets from the APK via AAssetManager.
// Thread-safe: AAssetManager is safe to use from any thread.

static AAssetManager* g_asset_manager = nullptr;

void init_asset_manager(JNIEnv* env, jobject context) {
    auto cls = env->GetObjectClass(context);
    auto mid = env->GetMethodID(cls, "getAssets", "()Landroid/content/res/AssetManager;");
    auto java_am = env->CallObjectMethod(context, mid);
    if (java_am) {
        g_asset_manager = AAssetManager_fromJava(env, java_am);
        PULP_LOGI("Asset manager initialized");
    }
    env->DeleteLocalRef(java_am);
    env->DeleteLocalRef(cls);
}

std::vector<uint8_t> read_asset(const std::string& path) {
    if (!g_asset_manager) {
        PULP_LOGE("read_asset: asset manager not initialized");
        return {};
    }

    AAsset* asset = AAssetManager_open(g_asset_manager, path.c_str(), AASSET_MODE_BUFFER);
    if (!asset) {
        PULP_LOGE("read_asset: failed to open '%s'", path.c_str());
        return {};
    }

    auto length = AAsset_getLength(asset);
    std::vector<uint8_t> data(static_cast<size_t>(length));
    AAsset_read(asset, data.data(), length);
    AAsset_close(asset);
    return data;
}

// ── App Data Directory ────────────────────────────────────────────────────
// Returns the app-private files directory (always writable with native I/O).
// Must be queried from Kotlin on startup via context.filesDir.

static std::string g_app_data_dir;

void set_app_data_dir(const std::string& dir) {
    g_app_data_dir = dir;
    PULP_LOGI("App data dir: %s", dir.c_str());
}

const std::string& app_data_dir() {
    return g_app_data_dir;
}

} // namespace pulp::android

// JNI exports for file provider initialization
extern "C" JNIEXPORT void JNICALL
Java_com_pulp_PulpFileProvider_nativeInitAssets(JNIEnv* env, jobject thiz, jobject context) {
    try {
        pulp::android::init_asset_manager(env, context);
    } catch (const std::exception& e) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), e.what());
    } catch (...) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"),
                      "Unknown C++ exception in nativeInitAssets");
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_pulp_PulpFileProvider_nativeSetAppDataDir(JNIEnv* env, jobject thiz, jstring dir) {
    try {
        const char* dir_str = env->GetStringUTFChars(dir, nullptr);
        if (dir_str) {
            pulp::android::set_app_data_dir(dir_str);
            env->ReleaseStringUTFChars(dir, dir_str);
        }
    } catch (const std::exception& e) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), e.what());
    } catch (...) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"),
                      "Unknown C++ exception in nativeSetAppDataDir");
    }
}

#endif // __ANDROID__
