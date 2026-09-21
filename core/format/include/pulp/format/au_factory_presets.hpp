#pragma once

/// @file au_factory_presets.hpp
/// The factory-preset surface both Audio Unit adapters serve to the host.
///
/// `pulp::state::PresetManager` already models factory-versus-user presets, but
/// it cannot find a bundled plug-in's factory folder by itself — that path is
/// format- and platform-specific. This header owns the Apple half: resolve the
/// folder from the loaded binary, scan it once, and hand the host a stable,
/// index-addressable table.
///
/// Both AU generations address a factory preset by number, so the index a host
/// stores in a session must mean the same preset when that session reopens.
/// The table is therefore built once per instance and sorted by name (the order
/// `PresetManager::factory_presets()` returns), never reordered by use.

/// Apple-only: the whole header is gated on __APPLE__ so it stays an empty
/// no-op on the Linux header-hygiene check.
#if defined(__APPLE__)

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <pulp/state/preset_manager.hpp>
#include <pulp/state/store.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace pulp::format::au {

/// Map a plug-in binary's path to the folder holding its factory presets.
///
/// Recognises the two Apple bundle shapes a Pulp plug-in is loaded from:
///   * `Foo.component/Contents/MacOS/Foo` → `Foo.component/Contents/Resources/Presets`
///   * `Foo.appex/Foo` (flat iOS bundle) → `Foo.appex/Presets`
///
/// Returns an empty path for a binary that is not inside a bundle — a unit-test
/// executable, a standalone build — so discovery degrades to "no factory
/// presets" instead of scanning an unrelated directory.
std::filesystem::path factory_presets_dir_for_binary(const std::filesystem::path& binary);

/// `factory_presets_dir_for_binary` applied to the image this code is linked
/// into. Empty when that image is not inside a bundle.
std::filesystem::path loaded_bundle_factory_presets_dir();

/// The host-facing factory-preset table for one plug-in instance.
///
/// Holds the `PresetManager` the adapter loads through and the `AUPreset`
/// records the AU v2 property surface hands out by pointer. Those records must
/// outlive every array the host is holding, so the table rebuilds only when a
/// caller re-points it — never in response to a host read.
class FactoryPresetTable {
  public:
    FactoryPresetTable() = default;
    ~FactoryPresetTable();

    FactoryPresetTable(const FactoryPresetTable&) = delete;
    FactoryPresetTable& operator=(const FactoryPresetTable&) = delete;

    /// Bind to the plug-in's StateStore and identity, and scan the bundle the
    /// adapter was loaded from. Safe to call with an identity whose bundle has
    /// no `Presets` folder: the table is then empty and the adapter reports no
    /// factory presets, which is what makes a host hide the menu.
    void bind(state::StateStore& store, const std::string& manufacturer,
              const std::string& plugin_name);

    /// Re-point discovery at `dir` and rebuild the table. A plug-in that keeps
    /// its factory presets somewhere other than the default bundle folder calls
    /// this straight after construction; tests use it to stage a fixture.
    void set_directory(const std::filesystem::path& dir);

    /// Number of factory presets the host can select.
    std::size_t size() const noexcept {
        return entries_.size();
    }

    /// UTF-8 display name of preset @p index, or empty when out of range.
    std::string name_at(std::size_t index) const;

    /// Load preset @p index into the bound StateStore. False when the index is
    /// out of range, nothing is bound, or the file could not be read.
    ///
    /// Deliberately does NOT take the adapter's `StateRestoreGate`: a preset
    /// carries parameter values only, so this writes the same atomics a host
    /// `SetParameter` writes and never calls
    /// `Processor::deserialize_plugin_state()`, which is the thing the gate
    /// exists to keep off a running render. If `PresetManager::load` ever grows
    /// a processor-state restore, this needs the gate and the callers need to
    /// hand it one.
    ///
    /// Main thread only — it reads a file.
    bool load(std::size_t index);

    /// The canonical `AUPreset` for @p index, owned by this table. Null when
    /// out of range. Use this rather than the host's copy when making a preset
    /// current: the host may pass a name that is not ours.
    const AUPreset* preset_at(std::size_t index) const noexcept;

    /// `AUBase::GetPresets`. Fills @p out_data with a CFArray of `AUPreset*`
    /// pointing into this table (the layout AU v2 hosts expect) and returns
    /// `noErr`; the caller owns the array. A null @p out_data is the host's
    /// "do you have any?" probe and returns `noErr` without allocating.
    /// Reports `kAudioUnitErr_InvalidProperty` when the plug-in ships none.
    OSStatus copy_presets(CFArrayRef* out_data) const;

    /// The `PresetManager` behind the table, or null before `bind`.
    state::PresetManager* preset_manager() noexcept {
        return presets_.get();
    }

  private:
    void rebuild();
    void clear_entries() noexcept;

    // The PresetManager holds the StateStore reference; the table itself
    // never touches the store directly.
    std::unique_ptr<state::PresetManager> presets_;
    // Parallel arrays, same order and length: `entries_[i].presetName` is a
    // +1 CFStringRef this table releases, and `infos_[i]` is the preset
    // `load(i)` reads.
    std::vector<AUPreset> entries_;
    std::vector<state::PresetInfo> infos_;
};

} // namespace pulp::format::au

#endif // __APPLE__
