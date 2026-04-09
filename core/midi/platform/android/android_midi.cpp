#if defined(__ANDROID__)

#include <pulp/platform/android/jni.hpp>
#include <android/log.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <cstdint>

#define PULP_LOG_TAG "Pulp"
#define PULP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, PULP_LOG_TAG, __VA_ARGS__)
#define PULP_LOGW(...) __android_log_print(ANDROID_LOG_WARN, PULP_LOG_TAG, __VA_ARGS__)

namespace pulp::midi {

// ── Android MIDI Device Registry ──────────────────────────────────────────

struct MidiDeviceEntry {
    int id;
    std::string name;
    int transport;  // 1 = bytestream (MIDI 1.0), 2 = UMP (MIDI 2.0)
};

// Device list — accessed from main thread only (JNI callbacks).
static std::vector<MidiDeviceEntry> g_devices;
static std::mutex g_devices_mutex;  // main thread only, never audio thread

void on_device_added(int id, const std::string& name, int transport) {
    std::lock_guard lock(g_devices_mutex);
    g_devices.push_back({id, name, transport});
    PULP_LOGI("MIDI device added: %s (id=%d, transport=%d), total=%zu",
              name.c_str(), id, transport, g_devices.size());
}

void on_device_removed(int id) {
    std::lock_guard lock(g_devices_mutex);
    g_devices.erase(
        std::remove_if(g_devices.begin(), g_devices.end(),
                       [id](const MidiDeviceEntry& e) { return e.id == id; }),
        g_devices.end()
    );
    PULP_LOGI("MIDI device removed: id=%d, remaining=%zu", id, g_devices.size());
}

std::vector<MidiDeviceEntry> get_devices() {
    std::lock_guard lock(g_devices_mutex);
    return g_devices;
}

// ── MIDI Data Callback ────────────────────────────────────────────────────
// Called from the Kotlin MIDI receiver thread (NOT the audio thread).
// Data is pushed into a lock-free SPSC queue for the audio thread to consume.
//
// The actual SpscQueue wiring happens in the standalone Android adapter —
// here we just provide the JNI entry point and a callback hook.

using MidiDataCallback = void(*)(int device_id, int port, const uint8_t* data,
                                  int offset, int count, int64_t timestamp,
                                  void* user_data);

static MidiDataCallback g_midi_callback = nullptr;
static void* g_midi_callback_data = nullptr;

void set_midi_data_callback(MidiDataCallback cb, void* user_data) {
    g_midi_callback = cb;
    g_midi_callback_data = user_data;
}

static void dispatch_midi_data(int device_id, int port,
                                const uint8_t* data, int offset, int count,
                                int64_t timestamp) {
    if (g_midi_callback) {
        g_midi_callback(device_id, port, data, offset, count, timestamp,
                        g_midi_callback_data);
    }
}

} // namespace pulp::midi

// ── JNI Exports ───────────────────────────────────────────────────────────

extern "C" JNIEXPORT void JNICALL
Java_com_pulp_midi_PulpMidiManager_nativeOnDeviceAdded(
    JNIEnv* env, jobject thiz, jint id, jstring name, jint transport) {
    try {
        const char* name_str = env->GetStringUTFChars(name, nullptr);
        if (name_str) {
            pulp::midi::on_device_added(id, name_str, transport);
            env->ReleaseStringUTFChars(name, name_str);
        }
    } catch (const std::exception& e) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), e.what());
    } catch (...) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"),
                      "Unknown C++ exception in nativeOnDeviceAdded");
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_pulp_midi_PulpMidiManager_nativeOnDeviceRemoved(
    JNIEnv* env, jobject thiz, jint id) {
    try {
        pulp::midi::on_device_removed(id);
    } catch (const std::exception& e) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), e.what());
    } catch (...) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"),
                      "Unknown C++ exception in nativeOnDeviceRemoved");
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_pulp_midi_PulpMidiManager_nativeOnMidiReceived(
    JNIEnv* env, jobject thiz, jint deviceId, jint portNumber,
    jbyteArray data, jint offset, jint count, jlong timestamp) {
    try {
        jsize len = env->GetArrayLength(data);
        if (offset + count > len) {
            env->ThrowNew(env->FindClass("java/lang/IllegalArgumentException"),
                          "MIDI data bounds check failed");
            return;
        }

        jbyte* bytes = env->GetByteArrayElements(data, nullptr);
        if (!bytes) return;

        pulp::midi::dispatch_midi_data(
            deviceId, portNumber,
            reinterpret_cast<const uint8_t*>(bytes) + offset,
            0, count, static_cast<int64_t>(timestamp));

        env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);  // read-only, no copy back
    } catch (const std::exception& e) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), e.what());
    } catch (...) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"),
                      "Unknown C++ exception in nativeOnMidiReceived");
    }
}

// Virtual MIDI port receive — same path as hardware MIDI
extern "C" JNIEXPORT void JNICALL
Java_com_pulp_midi_PulpMidiService_nativeOnVirtualMidiReceived(
    JNIEnv* env, jobject thiz, jbyteArray data, jint offset, jint count, jlong timestamp) {
    try {
        jsize len = env->GetArrayLength(data);
        if (offset + count > len) return;

        jbyte* bytes = env->GetByteArrayElements(data, nullptr);
        if (!bytes) return;

        // Virtual MIDI uses device_id = -1 to distinguish from hardware
        pulp::midi::dispatch_midi_data(
            -1, 0,
            reinterpret_cast<const uint8_t*>(bytes) + offset,
            0, count, static_cast<int64_t>(timestamp));

        env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
    } catch (const std::exception& e) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), e.what());
    } catch (...) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"),
                      "Unknown C++ exception in nativeOnVirtualMidiReceived");
    }
}

#endif // __ANDROID__
