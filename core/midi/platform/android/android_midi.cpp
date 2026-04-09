#if defined(__ANDROID__)

#include <pulp/midi/android_midi_fifo.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/platform/android/jni.hpp>
#include <pulp/runtime/spsc_queue.hpp>

#include <android/log.h>
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <atomic>
#include <stdexcept>
#include <string>
#include <vector>

#define PULP_LOG_TAG "Pulp"
#define PULP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, PULP_LOG_TAG, __VA_ARGS__)
#define PULP_LOGW(...) __android_log_print(ANDROID_LOG_WARN, PULP_LOG_TAG, __VA_ARGS__)

namespace pulp::midi {

// Forward declaration — defined in the android namespace below. We need
// this because dispatch_midi_data() calls android::push_bytes() before
// the android namespace block is entered.
namespace android {
    bool push_bytes(const std::uint8_t* bytes, int count, std::int64_t timestamp_ns);
}

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
    // Always push into the internal FIFO so audio-thread consumers
    // can drain regardless of whether a user callback is set.
    android::push_bytes(data + offset, count, timestamp);
}

} // namespace pulp::midi

// ── Android MIDI input FIFO ────────────────────────────────────────────────
// Lock-free ring buffer between the Kotlin MIDI receiver thread and the
// audio thread. Capacity sized generously — 512 events is ~17 seconds of
// typical keyboard input at 30 events/sec.

namespace pulp::midi::android {

static constexpr std::size_t kMidiFifoCapacity = 512;

static pulp::runtime::SpscQueue<AndroidMidiEvent, kMidiFifoCapacity>&
midi_fifo() {
    static pulp::runtime::SpscQueue<AndroidMidiEvent, kMidiFifoCapacity> q;
    return q;
}

// Drop counter — incremented when a push fails because the FIFO is full.
// Exposed for diagnostics; no mechanism exists for the audio thread to
// react to drops (a drop just means the player's input was lost).
static std::atomic<std::uint64_t> g_drop_count{0};

bool push_bytes(const std::uint8_t* bytes, int count, std::int64_t timestamp_ns) {
    if (!bytes || count <= 0) return false;
    AndroidMidiEvent ev;
    ev.timestamp_ns = static_cast<std::uint64_t>(timestamp_ns);
    ev.size = static_cast<std::uint8_t>(
        std::min<int>(count, static_cast<int>(ev.data.size())));
    std::copy_n(bytes, ev.size, ev.data.begin());
    if (!midi_fifo().try_push(ev)) {
        g_drop_count.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool has_pending() {
    return !midi_fifo().empty();
}

// Build a MidiEvent from raw bytes. Only handles short (1-3 byte)
// channel voice messages in Phase 1. SysEx and longer messages are
// skipped (the FIFO truncated them anyway).
static MidiEvent decode_short(const std::uint8_t* bytes, int count) {
    MidiEvent ev;
    if (count >= 3) {
        ev.message = choc::midi::ShortMessage(bytes[0], bytes[1], bytes[2]);
    } else if (count == 2) {
        ev.message = choc::midi::ShortMessage(bytes[0], bytes[1], 0);
    } else if (count == 1) {
        ev.message = choc::midi::ShortMessage(bytes[0], 0, 0);
    }
    return ev;
}

void drain_into(MidiBuffer& buffer,
                std::int64_t block_start_ns,
                double sample_rate,
                int block_size) {
    auto& fifo = midi_fifo();
    while (auto opt = fifo.try_pop()) {
        const auto& src = *opt;
        if (src.size == 0) continue;
        auto ev = decode_short(src.data.data(),
                               static_cast<int>(src.size));
        // Convert timestamp to a sample offset within the current block.
        std::int64_t delta_ns = 0;
        if (block_start_ns > 0) {
            delta_ns = static_cast<std::int64_t>(src.timestamp_ns)
                     - block_start_ns;
        }
        double sample_offset = 0.0;
        if (sample_rate > 0.0) {
            sample_offset = static_cast<double>(delta_ns) * sample_rate
                          / 1'000'000'000.0;
        }
        // Clamp to [0, block_size - 1]. Events in the past land at 0,
        // events in the future land at the end of the block (they'll be
        // slightly late, but this is simpler than buffering future
        // events and matches the latency profile of the audio thread).
        int offset = static_cast<int>(sample_offset);
        if (offset < 0) offset = 0;
        if (block_size > 0 && offset >= block_size) offset = block_size - 1;
        ev.sample_offset = offset;
        ev.timestamp = 0.0;  // filled by consumer if needed
        buffer.add(std::move(ev));
    }
}

} // namespace pulp::midi::android

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
