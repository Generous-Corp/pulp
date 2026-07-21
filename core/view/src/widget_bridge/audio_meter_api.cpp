// widget_bridge/audio_meter_api.cpp - live audio metering + per-frame tick for
// scripted custom draws.
//
// This is the enabler that lets a generated `ui.js` canvas draw LIVE, REACTIVE
// visuals — meters, VU needles, scopes, glow — that move with the plugin's real
// audio. Two tiny, safe JS surfaces:
//
//   getMeterLevel(ch)        -> latest RMS  for channel ch (linear, 0..1+)
//   getMeterPeak(ch)         -> latest peak for channel ch (linear, 0..1+)
//   getMeterChannelCount()   -> number of published channels (0 until a source
//                               is attached / first block processed)
//
//   onFrame(fn)              -> register a PERSISTENT per-frame callback; returns
//                               an id. Fired once per host vsync by
//                               WidgetBridge::service_frame_callbacks(), which
//                               then schedules the next paint so the canvas
//                               animates continuously. Zero overhead when no
//                               callback is registered.
//   cancelFrame(id)          -> stop a callback registered with onFrame.
//
// Thread-safety: the audio thread ONLY writes the AudioBridge's lock-free
// TripleBuffer (push_meter); getMeter* run on the UI thread (JS) and read the
// latest coherent snapshot via pop_latest_meter. No cross-thread mutation of
// WidgetBridge state. onFrame callbacks + the meter source are owned by the
// bridge and die with it, so the tick cannot deref freed state after the view
// closes (the host's poll() null-checks the bridge before ticking).

#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/audio_bridge.hpp>
#include "api_registry.hpp"

#include <algorithm>

namespace pulp::view {

void BridgeRegistrars::register_audio_meter_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // ── Meter reads ──────────────────────────────────────────────────────
    // Each call reads a fresh coherent MeterData snapshot from the lock-free
    // TripleBuffer. Reads are cheap (atomic load + small struct copy); a
    // getMeterLevel(0) and a getMeterPeak(0) in the same frame may observe two
    // successive audio-thread publications, but the skew is sub-block and
    // visually irrelevant for metering.
    register_bridge_function(api, "__getMeterLevel__", [&self](choc::javascript::ArgumentList args) {
        auto ch = args.get<int>(0, 0);
        if (!self.meter_source_) return choc::value::createFloat64(0.0);
        MeterData data;
        if (!self.meter_source_->pop_latest_meter(data)) return choc::value::createFloat64(0.0);
        if (ch < 0 || ch >= data.num_channels) return choc::value::createFloat64(0.0);
        return choc::value::createFloat64(static_cast<double>(data.rms[ch]));
    });

    register_bridge_function(api, "__getMeterPeak__", [&self](choc::javascript::ArgumentList args) {
        auto ch = args.get<int>(0, 0);
        if (!self.meter_source_) return choc::value::createFloat64(0.0);
        MeterData data;
        if (!self.meter_source_->pop_latest_meter(data)) return choc::value::createFloat64(0.0);
        if (ch < 0 || ch >= data.num_channels) return choc::value::createFloat64(0.0);
        return choc::value::createFloat64(static_cast<double>(data.peak[ch]));
    });

    register_bridge_function(api, "__getMeterChannelCount__", [&self](choc::javascript::ArgumentList) {
        if (!self.meter_source_) return choc::value::createInt32(0);
        MeterData data;
        if (!self.meter_source_->pop_latest_meter(data)) return choc::value::createInt32(0);
        return choc::value::createInt32(data.num_channels);
    });

    // ── Persistent per-frame tick ────────────────────────────────────────
    // __registerOnFrame__(id): start driving the loop. request_repaint asks the
    // host for a paint so on-demand hosts begin ticking; display-link hosts tick
    // every vsync regardless. __unregisterOnFrame__(id): stop.
    register_bridge_function(api, "__registerOnFrame__", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<int>(0, 0);
        if (id > 0) {
            // De-dup: a mis-behaving script that registers the same id twice must
            // not fire it twice per frame.
            if (std::find(self.persistent_frame_ids_.begin(), self.persistent_frame_ids_.end(), id)
                == self.persistent_frame_ids_.end()) {
                self.persistent_frame_ids_.push_back(id);
            }
            self.request_repaint();
        }
        return choc::value::createInt32(id);
    });

    register_bridge_function(api, "__unregisterOnFrame__", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<int>(0, 0);
        auto it = std::find(self.persistent_frame_ids_.begin(), self.persistent_frame_ids_.end(), id);
        if (it != self.persistent_frame_ids_.end()) self.persistent_frame_ids_.erase(it);
        return choc::value::Value();
    });

    // ── JS surface ───────────────────────────────────────────────────────
    // Idempotent install of the public globals wrapping the natives. Guarded so
    // a hot-reload rebuild (which re-runs register_api on a fresh engine) or a
    // re-eval is a no-op. The onFrame registry lives in JS (CHOC NativeFunctions
    // can't carry a JSValue callback); native tracks only the ids.
    self.engine_.evaluate(
        "if (typeof globalThis.__onFrameCallbacks__ === 'undefined') {"
        "  globalThis.__onFrameCallbacks__ = Object.create(null);"
        "  globalThis.__onFrameNextId__ = 1;"
        "  globalThis.__invokeOnFrame__ = function(id) {"
        "    var fn = __onFrameCallbacks__[id];"
        "    if (!fn) return;"
        "    try { fn(); } catch (e) {"
        "      if (typeof console !== 'undefined' && console.error) console.error('onFrame:', e);"
        "    }"
        "  };"
        "  globalThis.onFrame = function(fn) {"
        "    if (typeof fn !== 'function') return 0;"
        "    var id = __onFrameNextId__++;"
        "    __onFrameCallbacks__[id] = fn;"
        "    __registerOnFrame__(id);"
        "    return id;"
        "  };"
        "  globalThis.cancelFrame = function(id) {"
        "    if (__onFrameCallbacks__[id]) {"
        "      delete __onFrameCallbacks__[id];"
        "      __unregisterOnFrame__(id);"
        "    }"
        "  };"
        "  globalThis.getMeterLevel = function(ch) { return __getMeterLevel__(ch | 0); };"
        "  globalThis.getMeterPeak = function(ch) { return __getMeterPeak__(ch | 0); };"
        "  globalThis.getMeterChannelCount = function() { return __getMeterChannelCount__(); };"
        "}"
        "void 0;"
    );
}

} // namespace pulp::view
