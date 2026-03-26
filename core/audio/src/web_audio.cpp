// Web Audio API integration for WASM builds
// Provides AudioSystem and AudioDevice implementations using the Web Audio API
// via Emscripten's JavaScript interop.
//
// Architecture:
//   C++ (AudioDevice) ←→ Emscripten ←→ AudioWorkletProcessor (JS)
//   The JS AudioWorklet runs in a separate thread and pulls samples
//   from a SharedArrayBuffer that C++ writes into.

#ifdef __EMSCRIPTEN__

#include <pulp/audio/device.hpp>
#include <pulp/runtime/log.hpp>
#include <emscripten.h>
#include <emscripten/bind.h>
#include <vector>
#include <cstring>
#include <mutex>

namespace pulp::audio {

// ── Web Audio Device ────────────────────────────────────────────────────

class WebAudioDevice : public AudioDevice {
public:
    WebAudioDevice() = default;

    bool open(const DeviceConfig& config) override {
        config_ = config;
        buffer_.resize(config.output_channels * config.buffer_size, 0.0f);
        output_ptrs_.resize(config.output_channels);
        input_ptrs_.resize(config.input_channels);

        // Create AudioContext via JS
        EM_ASM({
            if (!window._pulpAudioCtx) {
                window._pulpAudioCtx = new AudioContext({
                    sampleRate: $0,
                    latencyHint: 'interactive'
                });
            }
        }, static_cast<int>(config.sample_rate));

        open_ = true;
        runtime::log_info("WebAudio: opened ({}Hz, {} out channels, buffer {})",
                          config.sample_rate, config.output_channels, config.buffer_size);
        return true;
    }

    bool start(AudioCallback callback) override {
        callback_ = std::move(callback);

        // Create a ScriptProcessorNode (deprecated but widely supported)
        // For production, this should use AudioWorklet, but ScriptProcessor
        // works for initial validation without SharedArrayBuffer/COOP headers.
        EM_ASM({
            var ctx = window._pulpAudioCtx;
            if (!ctx) return;

            var bufSize = $0;
            var outCh = $1;

            // Create script processor
            var proc = ctx.createScriptProcessor(bufSize, 0, outCh);
            proc.onaudioprocess = function(e) {
                // Call into C++ to fill the output buffer
                Module._pulp_web_audio_process(bufSize, outCh);

                // Copy from C++ buffer to Web Audio output
                var ptr = Module._pulp_web_audio_buffer_ptr();
                for (var ch = 0; ch < outCh; ch++) {
                    var output = e.outputBuffer.getChannelData(ch);
                    var offset = ptr / 4 + ch * bufSize;
                    output.set(Module.HEAPF32.subarray(offset, offset + bufSize));
                }
            };

            proc.connect(ctx.destination);
            window._pulpAudioNode = proc;

            // Resume context (browsers require user gesture)
            if (ctx.state === 'suspended') {
                document.addEventListener('click', function() {
                    ctx.resume();
                }, { once: true });
            }
        }, config_.buffer_size, config_.output_channels);

        running_ = true;
        runtime::log_info("WebAudio: started");
        return true;
    }

    void stop() override {
        if (!running_) return;
        EM_ASM({
            if (window._pulpAudioNode) {
                window._pulpAudioNode.disconnect();
                window._pulpAudioNode = null;
            }
        });
        running_ = false;
    }

    void close() override {
        stop();
        open_ = false;
    }

    bool is_open() const override { return open_; }

    DeviceInfo info() const override {
        return {"web-audio", "Web Audio API", config_.output_channels, config_.input_channels};
    }

    // Called from JS to fill the output buffer
    void process(int buffer_size, int out_channels) {
        if (!callback_) return;

        // Set up channel pointers
        for (int ch = 0; ch < out_channels; ++ch) {
            output_ptrs_[ch] = buffer_.data() + ch * buffer_size;
            std::memset(output_ptrs_[ch], 0, buffer_size * sizeof(float));
        }

        BufferView<float> output(output_ptrs_.data(), out_channels, buffer_size);
        BufferView<const float> input(nullptr, 0, 0); // No input for now

        CallbackContext ctx;
        ctx.sample_rate = config_.sample_rate;
        ctx.buffer_size = buffer_size;
        ctx.sample_position = sample_position_;

        callback_(input, output, ctx);
        sample_position_ += buffer_size;
    }

    float* buffer_ptr() { return buffer_.data(); }

private:
    DeviceConfig config_;
    AudioCallback callback_;
    std::vector<float> buffer_;
    std::vector<float*> output_ptrs_;
    std::vector<const float*> input_ptrs_;
    uint64_t sample_position_ = 0;
    bool open_ = false;
    bool running_ = false;
};

// Global instance for JS interop
static WebAudioDevice* g_web_device = nullptr;

// ── Web Audio System ────────────────────────────────────────────────────

class WebAudioSystem : public AudioSystem {
public:
    std::vector<DeviceInfo> enumerate_devices() override {
        return {{"web-audio", "Web Audio API", 2, 0}};
    }

    std::unique_ptr<AudioDevice> create_device(const std::string&) override {
        auto device = std::make_unique<WebAudioDevice>();
        g_web_device = device.get();
        return device;
    }
};

// ── Emscripten exports ──────────────────────────────────────────────────

extern "C" {

EMSCRIPTEN_KEEPALIVE
void _pulp_web_audio_process(int buffer_size, int out_channels) {
    if (g_web_device) g_web_device->process(buffer_size, out_channels);
}

EMSCRIPTEN_KEEPALIVE
float* _pulp_web_audio_buffer_ptr() {
    return g_web_device ? g_web_device->buffer_ptr() : nullptr;
}

} // extern "C"

// Factory override for WASM builds
std::unique_ptr<AudioSystem> create_audio_system() {
    return std::make_unique<WebAudioSystem>();
}

} // namespace pulp::audio

#endif // __EMSCRIPTEN__
