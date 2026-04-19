#if defined(__ANDROID__)

#include <oboe/Oboe.h>
#include <pulp/audio/frame_fill.hpp>
#include <pulp/platform/android/jni.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/android_midi_fifo.hpp>
#include <android/log.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <vector>

#define PULP_LOG_TAG "Pulp"
#define PULP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, PULP_LOG_TAG, __VA_ARGS__)
#define PULP_LOGW(...) __android_log_print(ANDROID_LOG_WARN, PULP_LOG_TAG, __VA_ARGS__)
#define PULP_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, PULP_LOG_TAG, __VA_ARGS__)

namespace pulp::audio {

// Forward declaration — the ADPF hint session is in adpf_hints.cpp
class AdpfHintSession;

// ── Audio Processing Callback ─────────────────────────────────────────────
// User-provided callback for processing audio buffers.
// MUST be lock-free: no mutex, JNI, malloc, file I/O.
using AudioCallback = void(*)(float* output, const float* input,
                               int num_frames, int num_channels, void* user_data);

// ── Oboe Audio Device ─────────────────────────────────────────────────────
// Single audio backend for Android. Uses Oboe (wraps AAudio on API 26+).
// Callback-driven output with optional input. MMAP exclusive mode preferred.

class OboeDevice : public oboe::AudioStreamDataCallback,
                   public oboe::AudioStreamErrorCallback {
public:
    OboeDevice() = default;
    ~OboeDevice() { stop(); }

    // -- Configuration --

    void set_callback(AudioCallback cb, void* user_data) {
        callback_ = cb;
        user_data_ = user_data;
    }

    void set_sample_rate(int32_t rate) { requested_sample_rate_ = rate; }
    void set_buffer_size(int32_t frames) { requested_buffer_size_ = frames; }
    void set_channel_count(int32_t channels) { requested_channels_ = channels; }

    // -- Lifecycle --

    bool start() {
        if (!open_output_stream()) return false;
        if (output_stream_->requestStart() != oboe::Result::OK) return false;
        // Workstream 02 #244: open an input stream symmetrically when the
        // caller asked for input channels. Failure to open is logged but
        // non-fatal — playback continues without capture.
        if (requested_input_channels_ > 0) {
            if (!open_input_stream()) {
                PULP_LOGW("Oboe: input-stream open failed; continuing output-only");
            } else if (input_stream_->requestStart() != oboe::Result::OK) {
                PULP_LOGW("Oboe: input-stream start failed");
                input_stream_->close();
                input_stream_.reset();
                input_buffer_.clear();
                current_input_channels_ = 0;
            } else {
                // Size the persistent input buffer to the effective burst
                // size × channel count. Allocation happens here (main
                // thread) so the audio callback stays malloc-free. We
                // overprovision to 4× the negotiated burst to absorb any
                // catch-up reads when the two streams drift briefly.
                current_input_channels_ = input_stream_->getChannelCount();
                const int32_t capacity_frames = std::max(
                    current_buffer_size_ * 4, input_stream_->getFramesPerBurst() * 4);
                input_buffer_.assign(
                    static_cast<size_t>(capacity_frames) *
                        static_cast<size_t>(current_input_channels_),
                    0.0f);
                PULP_LOGI("Oboe: input buffer provisioned for %d frames × %d ch",
                          capacity_frames, current_input_channels_);
            }
        }
        return true;
    }

    void set_requested_input_channels(int32_t n) { requested_input_channels_ = n; }

    void stop() {
        if (output_stream_) {
            output_stream_->requestStop();
            output_stream_->close();
            output_stream_.reset();
        }
        if (input_stream_) {
            input_stream_->requestStop();
            input_stream_->close();
            input_stream_.reset();
        }
        input_buffer_.clear();
        input_buffer_.shrink_to_fit();
        current_input_channels_ = 0;
    }

    // -- State --

    int32_t sample_rate() const { return current_sample_rate_; }
    int32_t buffer_size() const { return current_buffer_size_; }
    int32_t channel_count() const { return current_channels_; }
    int64_t xrun_count() const { return xrun_count_.load(std::memory_order_relaxed); }
    bool is_bluetooth_active() const { return bluetooth_active_.load(std::memory_order_acquire); }

    // #244: input-stream diagnostics. short_reads increments when
    // onAudioReady drains fewer frames than requested (warm-up, transient
    // drift); read_errors increments on a hard read failure that zero-filled
    // the block. Both are useful for UI-level input-health indicators.
    int32_t input_channel_count() const { return current_input_channels_; }
    int64_t input_short_read_count() const {
        return input_short_reads_.load(std::memory_order_relaxed);
    }
    int64_t input_read_error_count() const {
        return input_read_errors_.load(std::memory_order_relaxed);
    }

    /// MIDI buffer for the current audio block. Drained from the
    /// lock-free FIFO at the start of each onAudioReady callback.
    /// The standalone adapter reads this after its audio callback
    /// to feed the Processor's MIDI input.
    const pulp::midi::MidiBuffer& midi_buffer() const { return midi_buffer_; }

    // -- Dynamic routing --

    struct PendingConfig {
        int32_t sample_rate = 0;
        int32_t buffer_size = 0;
    };

    // Set by onErrorAfterClose, read by the audio callback
    std::atomic<bool> config_changed_{false};
    std::atomic<PendingConfig> pending_config_{};
    std::atomic<bool> bluetooth_active_{false};

private:
    // -- Oboe callbacks (called on the real-time audio thread) --

    oboe::DataCallbackResult onAudioReady(
        oboe::AudioStream* stream,
        void* audio_data,
        int32_t num_frames) override {

        // Check for pending config change (lock-free)
        if (config_changed_.load(std::memory_order_acquire)) {
            auto config = pending_config_.load(std::memory_order_relaxed);
            current_sample_rate_ = config.sample_rate;
            current_buffer_size_ = config.buffer_size;
            // The user callback is responsible for calling Processor::prepare()
            // with the new sample rate when it detects the change.
            config_changed_.store(false, std::memory_order_release);
        }

        auto start = std::chrono::steady_clock::now();

        auto* output = static_cast<float*>(audio_data);

        // Drain pending MIDI events from the lock-free FIFO into the
        // per-block MIDI buffer. The FIFO is fed by the Kotlin MIDI
        // receiver thread via JNI (android_midi.cpp). This must happen
        // before the audio callback so the Processor sees MIDI events
        // at the correct sample offsets within this block.
        midi_buffer_.clear();
#ifdef PULP_HAS_MIDI
        pulp::midi::android::drain_into(
            midi_buffer_,
            std::chrono::steady_clock::now().time_since_epoch().count(),
            static_cast<double>(current_sample_rate_),
            num_frames);
#endif

        if (callback_) {
            // #244: Drain the input stream on the same callback tick. Oboe's
            // recommended full-duplex pattern is to do a non-blocking read
            // on the output-stream callback — AAudio keeps the two streams
            // in step on a shared device period in practice.
            //
            // Non-blocking = timeoutNanos 0. Short-read (fewer frames
            // available than num_frames) is expected on the first few
            // callbacks while the input stream warms up; we zero-fill the
            // tail so the Processor sees a deterministic buffer shape.
            //
            // All allocation happens in start() / on_input_config_changed();
            // this path is lock-free and malloc-free.
            const float* input_data = nullptr;
            if (input_stream_ && !input_buffer_.empty()) {
                const int32_t in_channels = current_input_channels_;
                const size_t samples_needed =
                    static_cast<size_t>(num_frames) * static_cast<size_t>(in_channels);
                float* buf = input_buffer_.data();

                auto result = input_stream_->read(buf, num_frames, /*timeoutNanos=*/0);
                if (result == oboe::Result::OK) {
                    int32_t frames_read = result.value();
                    if (frames_read < num_frames) {
                        // Zero-fill the tail so the callback sees a
                        // full-sized buffer with silence for the
                        // unavailable frames.
                        pulp::audio::zero_fill_short_read(
                            buf, frames_read, num_frames, in_channels);
                        input_short_reads_.fetch_add(1, std::memory_order_relaxed);
                    }
                    input_data = buf;
                } else {
                    // Read failure (stream disconnected mid-callback): zero
                    // the buffer and surface nullptr so the Processor knows
                    // input isn't available this block. onErrorAfterClose
                    // will trigger a reopen out-of-band.
                    std::memset(buf, 0, sizeof(float) * samples_needed);
                    input_read_errors_.fetch_add(1, std::memory_order_relaxed);
                }
            }

            callback_(output, input_data, num_frames, current_channels_, user_data_);
        } else {
            // Silence if no callback
            std::memset(output, 0, sizeof(float) * num_frames * current_channels_);
        }

        // Report actual work duration to ADPF (if available)
        auto elapsed = std::chrono::steady_clock::now() - start;
        auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
        last_callback_duration_ns_.store(elapsed_ns, std::memory_order_relaxed);

        // Track xruns
        auto xruns = stream->getXRunCount();
        if (xruns != oboe::ResultWithValue<int32_t>(oboe::Result::ErrorUnimplemented) &&
            xruns.value() > last_reported_xruns_) {
            xrun_count_.fetch_add(xruns.value() - last_reported_xruns_, std::memory_order_relaxed);
            last_reported_xruns_ = xruns.value();
        }

        return oboe::DataCallbackResult::Continue;
    }

    // Called on Oboe's error thread (NOT the audio thread) when the stream
    // is disconnected (device change, USB unplug, BT connect/disconnect).
    void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) override {
        PULP_LOGW("Oboe stream error: %s — attempting restart", oboe::convertToText(error));

        // Query new device's optimal parameters
        // (This runs on Oboe's error thread, not the audio thread — JNI calls are safe here)
        auto* env = pulp::android::get_env();
        if (!env) {
            PULP_LOGE("Failed to get JNIEnv for stream restart");
            return;
        }

        // TODO: Query AudioManager via JNI for new native sample rate / buffer size
        // For now, reopen with the same parameters
        int32_t new_sample_rate = current_sample_rate_;
        int32_t new_buffer_size = current_buffer_size_;

        // Detect Bluetooth routing
        // TODO: Query output device type from AudioManager
        // bluetooth_active_.store(is_a2dp, std::memory_order_release);

        if (new_sample_rate != current_sample_rate_ || new_buffer_size != current_buffer_size_) {
            pending_config_.store({new_sample_rate, new_buffer_size}, std::memory_order_relaxed);
            config_changed_.store(true, std::memory_order_release);
        }

        // Reopen and restart the stream
        if (open_output_stream()) {
            output_stream_->requestStart();
            PULP_LOGI("Oboe stream restarted: %dHz, %d frames",
                      current_sample_rate_, current_buffer_size_);
        } else {
            PULP_LOGE("Failed to reopen Oboe stream after error");
        }
    }

    // -- Stream creation --

    // Workstream 02 #244: symmetric input-stream opener. Runs in parallel
    // with the output stream when requested_input_channels_ > 0. Shares
    // the same low-latency / exclusive configuration so the two streams
    // stay in step with AAudio's MMAP path on Bluetooth and USB devices.
    bool open_input_stream() {
        oboe::AudioStreamBuilder builder;
        builder.setDirection(oboe::Direction::Input)
               ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
               ->setSharingMode(oboe::SharingMode::Shared)
               ->setFormat(oboe::AudioFormat::Float)
               ->setChannelCount(requested_input_channels_);

        if (requested_sample_rate_ > 0) {
            builder.setSampleRate(requested_sample_rate_);
        }
        if (requested_buffer_size_ > 0) {
            builder.setFramesPerCallback(requested_buffer_size_);
        }

        auto result = builder.openManagedStream(input_stream_);
        if (result != oboe::Result::OK) {
            PULP_LOGE("Failed to open Oboe input stream: %s",
                      oboe::convertToText(result));
            return false;
        }
        PULP_LOGI("Oboe input stream opened: %dHz, %d channels",
                  input_stream_->getSampleRate(),
                  input_stream_->getChannelCount());
        return true;
    }

    bool open_output_stream() {
        oboe::AudioStreamBuilder builder;
        builder.setDirection(oboe::Direction::Output)
               ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
               ->setSharingMode(oboe::SharingMode::Exclusive)
               ->setFormat(oboe::AudioFormat::Float)
               ->setChannelCount(requested_channels_)
               ->setDataCallback(this)
               ->setErrorCallback(this)
               ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Best);

        if (requested_sample_rate_ > 0) {
            builder.setSampleRate(requested_sample_rate_);
        }
        if (requested_buffer_size_ > 0) {
            builder.setFramesPerCallback(requested_buffer_size_);
        }

        auto result = builder.openManagedStream(output_stream_);
        if (result != oboe::Result::OK) {
            PULP_LOGE("Failed to open Oboe output stream: %s", oboe::convertToText(result));
            return false;
        }

        current_sample_rate_ = output_stream_->getSampleRate();
        current_buffer_size_ = output_stream_->getFramesPerBurst();
        current_channels_ = output_stream_->getChannelCount();

        PULP_LOGI("Oboe output stream opened: %dHz, %d frames/burst, %d channels, %s",
                  current_sample_rate_, current_buffer_size_, current_channels_,
                  output_stream_->getSharingMode() == oboe::SharingMode::Exclusive
                      ? "exclusive" : "shared");

        return true;
    }

    // -- State --

    oboe::ManagedStream output_stream_;
    oboe::ManagedStream input_stream_;

    AudioCallback callback_ = nullptr;
    void* user_data_ = nullptr;

    int32_t requested_sample_rate_ = 0;
    int32_t requested_buffer_size_ = 0;
    int32_t requested_channels_ = 2;
    int32_t requested_input_channels_ = 0;  // #244: 0 = output-only

    int32_t current_sample_rate_ = 48000;
    int32_t current_buffer_size_ = 256;
    int32_t current_channels_ = 2;
    int32_t current_input_channels_ = 0;  // #244

    std::atomic<int64_t> xrun_count_{0};
    int32_t last_reported_xruns_ = 0;
    std::atomic<int64_t> last_callback_duration_ns_{0};

    // #244: persistent input-frame buffer. Allocated in start() (main
    // thread); sized to cover 4× the burst so onAudioReady can drain
    // without allocating. Accessed only on the audio thread.
    std::vector<float> input_buffer_;
    std::atomic<int64_t> input_short_reads_{0};
    std::atomic<int64_t> input_read_errors_{0};

    // Per-block MIDI buffer, drained from the lock-free FIFO each callback.
    pulp::midi::MidiBuffer midi_buffer_;
};

} // namespace pulp::audio

#endif // __ANDROID__
