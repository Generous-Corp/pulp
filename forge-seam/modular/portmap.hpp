#pragma once

// What CARTOG measured, for drawing somebody else's module.
//
// A module's controls and jacks exist only in its compiled widget code: a
// plugin.json says nothing about them and the panel artwork usually does not
// draw them. CARTOG runs inside Rack, walks the widgets, and writes what it
// finds to `<Rack user dir>/forge-portmap.json`. This reads that back.
//
// Coordinates arrive in Rack's own units, which are the units the layout here
// already uses: a panel is 380 of them tall and one HP is 15 wide. So nothing
// is converted -- a millimetre conversion would be a second place for the two
// to disagree.
//
// Only for modules we did NOT make. Ours come from the manifest their panel
// was emitted from, which is always present and never needs a scan.

#include <map>
#include <string>
#include <vector>

namespace forge_modular {

/// One control or jack, as Rack draws it.
struct MappedWidget {
    int index = 0;
    std::string name;      ///< the label its author gave it
    float x = 0, y = 0;    ///< centre, in panel units
    float w = 0, h = 0;    ///< drawn size; 0 for jacks, which are not measured
    /// What Rack draws it AS — "knob", "slider", "button", "switch".
    ///
    /// Empty for a scan taken before CARTOG classified controls, which is what
    /// every existing map is. Absent means "unknown", never "knob".
    std::string kind;
};

/// One module's measured layout.
struct MappedModule {
    std::string plugin_version;
    /// Which scanner measured it. See PortMap::kScanVersion.
    int scan_version = 0;
    float width = 0, height = 0;
    std::vector<MappedWidget> params;
    std::vector<MappedWidget> inputs;
    std::vector<MappedWidget> outputs;
};

/// Every module CARTOG has measured, keyed "plugin/model".
///
/// Read once and cached: the file changes only when somebody presses SCAN,
/// and re-reading it per frame would put a file open in the paint path.
class PortMap {
public:
    /// The map for this machine, loaded on first use.
    static const PortMap& shared();

    /// `plugin/model`, or nullptr when that module has never been scanned.
    /// Absent is the normal case, not an error: a module nobody has placed in
    /// front of CARTOG cannot have been measured, and the caller draws a plain
    /// face rather than a wrong one.
    const MappedModule* find(const std::string& plugin,
                             const std::string& model) const;

    std::size_t size() const { return by_key_.size(); }

    /// Parse a map's text. Exposed so a test can feed it a fixture instead of
    /// depending on whatever this machine happens to have scanned.
    static PortMap parse(const std::string& json);

    /// What the current scanner records.
    ///
    /// Bumped whenever CARTOG starts measuring something it did not before, so
    /// an entry from an older scanner is detectable. It has to be, because a
    /// map is MERGED rather than rewritten: a module measured once is carried
    /// forward untouched by every later scan, and stays at whatever the
    /// scanner of the day knew how to record.
    ///
    /// That is not hypothetical. Fourteen of nineteen mapped modules were
    /// carried over from a scanner that recorded jacks and no controls, so
    /// Fundamental's LFO had four inputs, four outputs and not one of its
    /// knobs -- while its plugin version matched exactly, which was the only
    /// thing being compared. They read as fully measured and drew as faceless.
    ///
    ///   1  jacks only
    ///   2  controls, with their kind, size and label
    ///   3  lights and displays
    static constexpr int kScanVersion = 3;

    /// The first scanner that recorded controls at all.
    ///
    /// Below this, "no params" means nothing was looked for. At or above it,
    /// "no params" is a measurement: Fundamental's Merge and Split really do
    /// have sixteen jacks and not one knob, and telling somebody we could not
    /// measure their controls is as wrong as drawing controls they lack.
    static constexpr int kScanVersionWithParams = 2;

    /// Do we know where this module's controls are?
    ///
    /// Three states collapse into this one answer, and getting them confused
    /// puts the UNMAPPED badge on the wrong panels in both directions:
    ///
    ///   params recorded            -> known
    ///   none, by a scanner that looks -> known; it HAS none (Merge, Split)
    ///   none, by one that did not     -> unknown; nobody looked
    ///
    /// Free rather than a member so a test can ask it of a MappedModule it
    /// built, instead of through the process-wide map read off this machine.
    /// Asserting the fields separately proved nothing: the rule lived in the
    /// caller, and reverting it left every assertion green.
    static bool controls_known(const MappedModule& m) {
        return !m.params.empty() || m.scan_version >= kScanVersionWithParams;
    }

    /// Should this measured control be drawn as a knob?
    ///
    /// Yes for a knob, and yes for a control from a scan that never classified
    /// anything — every map written before CARTOG recorded `kind` has none,
    /// and refusing those would empty panels that draw correctly today. No for
    /// a slider, switch or button: the manifest path already refuses to draw
    /// those as circles, on the grounds that a fader drawn as a dial is wrong
    /// in a way a reader cannot see, and the measured path was doing exactly
    /// that.
    ///
    /// Free-standing so a test can ask the RULE, rather than reach the
    /// process-wide map this machine happens to have scanned.
    static bool draws_as_knob(const MappedWidget& w) {
        return w.kind.empty() || w.kind == "knob";
    }

    /// Why a module cannot be drawn faithfully, if it cannot.
    enum class Gap {
        none,       ///< measured, by the current scanner
        unmeasured, ///< never placed in front of CARTOG
        stale,      ///< measured, but by an older scanner or an older plugin
    };

    /// What stands between this module and a faithful drawing.
    ///
    /// `installed_version` is what the plugin declares NOW. A map entry
    /// records the version it was measured against, so an update that moves a
    /// knob is detectable rather than silently wrong -- which is the failure
    /// that matters here, because a stale position looks exactly as confident
    /// as a fresh one.
    ///
    /// An unknown installed version is treated as "not stale": we cannot
    /// compare, and refusing to draw on a comparison we could not make would
    /// throw away good data.
    Gap gap_for(const std::string& plugin, const std::string& model,
                const std::string& installed_version) const;

private:
    std::map<std::string, MappedModule> by_key_;
};

}  // namespace forge_modular
