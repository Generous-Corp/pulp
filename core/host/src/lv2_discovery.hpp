#pragma once

// What the LV2 loader learns about a bundle before it instantiates anything.
// Both functions read the bundle the way every LV2 host does: through lilv,
// starting from manifest.ttl and following rdfs:seeAlso, so a description split
// across files is merged and a bundle without a manifest describes nothing.

#include <cstdint>
#include <string>
#include <vector>

namespace pulp::host::detail {

struct Lv2PortRole {
    int index = -1;
    std::string symbol;
    std::string name;
    bool is_input = false;
    bool is_audio = false;
    bool is_control = false;
    bool is_atom = false;  ///< atom:AtomPort, the port kind that carries events.
    bool is_cv = false;
    /// lv2:connectionOptional. A required port of any other kind makes the
    /// plugin unhostable: LV2 requires every port to be connected before run().
    bool optional = false;
    float min_value = 0.0f;
    float max_value = 1.0f;
    float default_value = 0.0f;
    // Control-port properties.
    bool stepped = false;          ///< lv2:toggled, lv2:integer, or lv2:enumeration.
    bool hidden = false;           ///< lv2:notOnGUI.
    bool reports_latency = false;  ///< Output control port carrying the plugin's latency.
    bool free_wheeling = false;    ///< Input designated lv2:freeWheeling (offline render).
    // Atom-port properties.
    bool supports_midi = false;       ///< atom:supports midi:MidiEvent.
    bool supports_time = false;       ///< atom:supports time:Position.
    bool supports_patch = false;      ///< atom:supports patch:Message.
    bool designated_control = false;  ///< lv2:designation lv2:control.
    std::uint32_t minimum_size = 0;   ///< rsz:minimumSize, bytes.
};

/// A property the plugin exposes as a parameter through patch:writable (the
/// host may set it) or patch:readable (the plugin only reports it). Values
/// travel as patch:Set messages on an atom port, not through a control port.
struct Lv2PatchParameter {
    std::string uri;
    std::string label;
    bool writable = false;
    float min_value = 0.0f;
    float max_value = 1.0f;
    float default_value = 0.0f;
    bool stepped = false;  ///< rdfs:range atom:Int, atom:Long, or atom:Bool.
};

/// Ports of the plugin `plugin_uri` (the bundle's first plugin when empty), in
/// index order. Empty when the bundle describes no such plugin.
std::vector<Lv2PortRole> discover_lv2_ports(const std::string& bundle_dir,
                                            const std::string& plugin_uri = {});

/// The plugin's lv2:binary as a file path, or empty when the bundle names none.
std::string resolve_lv2_binary(const std::string& bundle_dir,
                               const std::string& plugin_uri = {});

} // namespace pulp::host::detail
