#pragma once

// NativeHandleVisitor — typed plugin introspection.
//
// PluginSlot intentionally abstracts the format (VST3, AU, CLAP, LV2) so that
// host code can drive any plugin through a single interface. Sometimes,
// though, host code needs the underlying format-specific handle: a panel
// might want to surface the `clap_plugin_id`, a debugger might want the
// VST3 `IComponent*`, a CoreAudio-aware UI might want the `AudioComponent`.
// Punching that out with a `void*` getter would leak the abstraction and
// invite undefined behavior the moment a caller cast the wrong way.
//
// So the escape hatch is a double-dispatch visitor instead. Host code
// subclasses `NativeHandleVisitor`, overrides only the `visit_*` methods for
// the formats it can actually handle, and calls `slot.accept(visitor)`. The
// slot — which is the only thing that knows its own format — calls back into
// the matching `visit_*` with a populated handle struct. Two properties fall
// out of that, and they are the whole reason for the indirection:
//
//   * The cast is made by the code that knows the true type, so a caller can
//     never mis-cast a `void*` it was handed.
//   * A slot compiled WITHOUT a given format SDK simply never calls that
//     format's visitor method, so a visitor cannot be handed a stale or
//     half-initialized handle for a format that is not in the build.
//
// The format-specific handle structs deliberately avoid pulling in the
// format SDK headers — handles are exposed as `void*` typedefs so that
// callers that *do* link against the SDK can `static_cast<>` them back to
// the concrete type (e.g. `static_cast<const clap_plugin_t*>(handle)`)
// while non-SDK call sites can still link the header.
//
// Default `visit_*` implementations fall through to `visit_unknown(slot)`,
// letting hosts opt-in to specific format dispatches without overriding
// every method.

#include <string>
#include <string_view>

namespace pulp::host {

class PluginSlot;

// Which plugin format a handle came from. Deliberately a separate enum from
// scanner.hpp's PluginFormat (same members) so that a caller who only wants the
// visitor does not have to pull the scanner in.
enum class NativeHandleFormat {
    Unknown,
    VST3,
    AudioUnit,
    AudioUnitV3,
    CLAP,
    LV2,
};

// ── Per-format handle descriptors ───────────────────────────────────────
//
// All native handles are exposed as `void*` so this header is SDK-free.
// Callers reinterpret_cast back to the documented concrete type. Each
// struct carries a small handful of identifying metadata so that visitor
// code can perform a sanity check (format / IID / category) before
// reaching into the handle.

struct Vst3NativeHandle {
    /// `Steinberg::Vst::IComponent*` — the audio-processing side of the
    /// plugin. Lifetime is tied to the owning PluginSlot.
    void* component = nullptr;
    /// `Steinberg::Vst::IAudioProcessor*` — non-null when the plugin
    /// implements the audio-processor interface (every effect/instrument).
    void* audio_processor = nullptr;
    /// `Steinberg::Vst::IEditController*` — non-null for plugins that
    /// expose a controller. May alias `component` on combined plugins.
    void* edit_controller = nullptr;
    /// FUID string of the plugin's processor class, e.g.
    /// "ABCDEF01-2345-6789-ABCD-EF0123456789".
    std::string class_id;
};

struct AudioUnitNativeHandle {
    /// `AudioComponentInstance` (a.k.a. `AudioUnit`). Owned by the slot.
    void* component_instance = nullptr;
    /// AU component description fields, populated at load time.
    unsigned int type = 0;           ///< OSType (kAudioUnitType_*).
    unsigned int subtype = 0;        ///< OSType (manufacturer-defined).
    unsigned int manufacturer = 0;   ///< OSType (manufacturer code).
};

struct AudioUnitV3NativeHandle {
    /// `AUAudioUnit*`. Bridged-Objective-C pointer, owned by the slot.
    void* audio_unit = nullptr;
};

struct ClapNativeHandle {
    /// `const clap_plugin_t*` — the plugin instance. Owned by the slot.
    void* plugin = nullptr;
    /// `const clap_host_t*` — host-side struct the plugin was created
    /// against. Owned by the slot; do not free.
    void* host = nullptr;
    /// CLAP plugin id, e.g. "com.pulp.gain".
    std::string plugin_id;
};

struct Lv2NativeHandle {
    /// `LilvInstance*` if the loader uses lilv; otherwise opaque to the
    /// host (loader-specific).
    void* instance = nullptr;
    /// LV2 plugin URI, e.g. "https://example.com/plugins/gain".
    std::string uri;
};

// ── Visitor base class ──────────────────────────────────────────────────
//
// Subclass and override only the visit_* methods relevant to your work.
// The default fallthrough to `visit_unknown` means a visitor that only
// cares about CLAP can ignore every other adapter without explicit
// no-ops.
class NativeHandleVisitor {
public:
    virtual ~NativeHandleVisitor() = default;

    /// Called when the slot represents a format the visitor did not
    /// override, or when the slot itself does not know its own format
    /// (placeholder / unresolved slots). The slot is non-null.
    virtual void visit_unknown(const PluginSlot& /*slot*/,
                               NativeHandleFormat /*format*/) {}

    virtual void visit_vst3(const PluginSlot& slot,
                            const Vst3NativeHandle& /*ext*/) {
        visit_unknown(slot, NativeHandleFormat::VST3);
    }
    virtual void visit_audio_unit(const PluginSlot& slot,
                                  const AudioUnitNativeHandle& /*ext*/) {
        visit_unknown(slot, NativeHandleFormat::AudioUnit);
    }
    virtual void visit_audio_unit_v3(const PluginSlot& slot,
                                     const AudioUnitV3NativeHandle& /*ext*/) {
        visit_unknown(slot, NativeHandleFormat::AudioUnitV3);
    }
    virtual void visit_clap(const PluginSlot& slot,
                            const ClapNativeHandle& /*ext*/) {
        visit_unknown(slot, NativeHandleFormat::CLAP);
    }
    virtual void visit_lv2(const PluginSlot& slot,
                           const Lv2NativeHandle& /*ext*/) {
        visit_unknown(slot, NativeHandleFormat::LV2);
    }
};

// ── Entry point ─────────────────────────────────────────────────────────
//
// PluginSlot::accept(visitor) is the canonical entry point. Its default
// implementation in plugin_slot.hpp calls visit_unknown, so a slot that has
// not (or cannot) identify itself still behaves sensibly. Each format-specific
// slot overrides accept() to dispatch into the matching visit_* method with a
// populated `*NativeHandle` struct.

// ── Deprecated spellings ────────────────────────────────────────────────
//
// Kept so existing host code keeps compiling; every alias names the identical
// type. The new names say what the values are — native, format-specific
// handles — rather than the vaguer "extension".


} // namespace pulp::host
