#pragma once

// ExtensionsVisitor — typed plugin introspection.
//
// PluginSlot intentionally abstracts the format (VST3, AU, CLAP, LV2) so that
// host code can drive any plugin through a single interface. Sometimes,
// though, host code needs the underlying format-specific handle: a panel
// might want to surface the `clap_plugin_id`, a debugger might want the
// VST3 `IComponent*`, a CoreAudio-aware UI might want the `AudioComponent`.
// Punching that out with a `void*` getter would leak the abstraction and
// invite undefined behaviour the moment a caller cast the wrong way.
//
// Inspiration: JUCE's `ExtensionsVisitor` family. The pattern is a
// double-dispatch visitor — host code subclasses `ExtensionsVisitor`,
// overrides the format-specific `visit*` methods it cares about, and calls
// `slot.accept(visitor)`. Slots that know about a format call back into the
// matching `visit*`; slots compiled without a given format SDK can simply
// not call its visitor method, so the visitor never sees a stale handle.
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

// Tag struct identifying the underlying plugin format. Mirrors the
// PluginFormat enum but stays in this header to avoid pulling scanner.hpp
// for callers that only want the visitor pattern.
enum class ExtensionFormat {
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

struct Vst3Extension {
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

struct AudioUnitExtension {
    /// `AudioComponentInstance` (a.k.a. `AudioUnit`). Owned by the slot.
    void* component_instance = nullptr;
    /// AU component description fields, populated at load time.
    unsigned int type = 0;           ///< OSType (kAudioUnitType_*).
    unsigned int subtype = 0;        ///< OSType (manufacturer-defined).
    unsigned int manufacturer = 0;   ///< OSType (manufacturer code).
};

struct AudioUnitV3Extension {
    /// `AUAudioUnit*`. Bridged-Objective-C pointer, owned by the slot.
    void* audio_unit = nullptr;
};

struct ClapExtension {
    /// `const clap_plugin_t*` — the plugin instance. Owned by the slot.
    void* plugin = nullptr;
    /// `const clap_host_t*` — host-side struct the plugin was created
    /// against. Owned by the slot; do not free.
    void* host = nullptr;
    /// CLAP plugin id, e.g. "com.pulp.gain".
    std::string plugin_id;
};

struct Lv2Extension {
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
class ExtensionsVisitor {
public:
    virtual ~ExtensionsVisitor() = default;

    /// Called when the slot represents a format the visitor did not
    /// override, or when the slot itself does not know its own format
    /// (placeholder / unresolved slots). The slot is non-null.
    virtual void visit_unknown(const PluginSlot& /*slot*/,
                               ExtensionFormat /*format*/) {}

    virtual void visit_vst3(const PluginSlot& slot,
                            const Vst3Extension& /*ext*/) {
        visit_unknown(slot, ExtensionFormat::VST3);
    }
    virtual void visit_audio_unit(const PluginSlot& slot,
                                  const AudioUnitExtension& /*ext*/) {
        visit_unknown(slot, ExtensionFormat::AudioUnit);
    }
    virtual void visit_audio_unit_v3(const PluginSlot& slot,
                                     const AudioUnitV3Extension& /*ext*/) {
        visit_unknown(slot, ExtensionFormat::AudioUnitV3);
    }
    virtual void visit_clap(const PluginSlot& slot,
                            const ClapExtension& /*ext*/) {
        visit_unknown(slot, ExtensionFormat::CLAP);
    }
    virtual void visit_lv2(const PluginSlot& slot,
                           const Lv2Extension& /*ext*/) {
        visit_unknown(slot, ExtensionFormat::LV2);
    }
};

// ── Default no-op accept_ helpers ───────────────────────────────────────
//
// PluginSlot::accept(visitor) is the canonical entry point — its default
// implementation in plugin_slot.hpp calls visit_unknown so unknown slots
// behave sensibly. Each format-specific slot overrides accept() to
// dispatch into the matching visit_* method with a populated
// `*Extension` struct.

} // namespace pulp::host
