/// @file recognition_audio_pack.cpp
/// The built-in Pulp Figma Library recognition pack. See
/// recognition_audio_pack.hpp.

#include <pulp/view/recognition_audio_pack.hpp>

#include <utility>

namespace pulp::view {

RecognitionSource pulp_audio_control_library_source() {
    // The built-in Pulp Figma Library, mirrored from
    // tools/figma-plugin/library-manifest.json. Kept in code so the C++ importer
    // can re-resolve a designer's keys without reading the JSON at runtime; a
    // unit test pins this table against the JSON so the two never drift.
    // component_set_key values are the published Figma keys; "TBD-" placeholders
    // are intentionally excluded (no real key collision possible) — there are
    // none today.
    RecognitionSource builtin;
    builtin.name = "builtin-library";
    builtin.entries = {
        {AudioWidgetType::knob, "f74264ffa9108521fb0d3398dc8f5ea88e23a84e", "Pulp / Knob", ""},
        {AudioWidgetType::fader, "1c2b727f0c0e11026512725aeb546997f16042bd", "Pulp / Fader", ""},
        {AudioWidgetType::meter, "52e1636086b855cb2d20d341d4cfa15e94151eef", "Pulp / Meter", ""},
        {AudioWidgetType::xy_pad, "9dc09d4cbf65341f12c21ece408ad653886059b9", "Pulp / XYPad", ""},
        {AudioWidgetType::waveform, "2c0797af5c939638ec6a89d893ba310a088ce46c", "Pulp / Waveform",
         ""},
        {AudioWidgetType::spectrum, "f6730821fc7557e93f904d171a45339207abf9e3", "Pulp / Spectrum",
         ""},
    };
    return builtin;
}

RecognitionResolver recognition_resolver_with_builtin_library() {
    RecognitionResolver r;
    r.add_source(pulp_audio_control_library_source());
    return r;
}

} // namespace pulp::view
