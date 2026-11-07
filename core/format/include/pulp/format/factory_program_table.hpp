#pragma once

/// @file factory_program_table.hpp
/// A plug-in's factory presets as an index-addressable, preloaded program table.
///
/// A format that lets the host pick a preset by number (the VST3 program list)
/// can be told to switch from the audio thread, where the preset file cannot be
/// read. This table reads every factory preset once, on the main thread, and
/// keeps the parameter values in memory so selecting a program later is a
/// plain batch of parameter writes.
///
/// The numbering is the one `PresetManager::factory_presets()` returns (sorted
/// by name), so a program index a host stores in a session keeps naming the
/// same preset when the session reopens.

#include <pulp/state/preset_manager.hpp>
#include <pulp/state/store.hpp>

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace pulp::format {

/// Map a plug-in module's path to the folder holding its factory presets.
///
/// A bundled plug-in keeps them in `<Bundle>/Contents/Resources/Presets`, and
/// its binary sits one or two levels below `Contents` (`Contents/MacOS/Foo`,
/// `Contents/x86_64-win/Foo.vst3`, `Contents/x86_64-linux/Foo.so`). Returns an
/// empty path for a module outside such a bundle, so discovery degrades to "no
/// factory presets" instead of scanning an unrelated directory.
std::filesystem::path factory_presets_dir_for_module(const std::filesystem::path& module);

/// `factory_presets_dir_for_module` applied to the image this code is linked
/// into. Empty when that image is not inside a bundle.
std::filesystem::path loaded_module_factory_presets_dir();

class FactoryProgramTable {
  public:
    /// Scan @p dir (or the loaded module's bundle folder when @p dir is empty)
    /// and cache every readable preset's parameter values. A preset file that
    /// cannot be read is left out. Main thread only.
    ///
    /// Must be called after the processor has defined its parameters: a preset
    /// is matched to parameters by name.
    void build(state::StateStore& store, const std::string& manufacturer,
               const std::string& plugin_name, const std::filesystem::path& dir = {});

    /// Number of programs the host can select.
    std::size_t size() const noexcept {
        return programs_.size();
    }

    /// UTF-8 name of program @p index, or empty when out of range.
    const std::string& name_at(std::size_t index) const noexcept;

    /// The parameter values program @p index sets, or an empty span when out of
    /// range. Valid until the next `build`. Safe to read from any thread once
    /// `build` has returned.
    std::span<const state::ParamSetting> values_at(std::size_t index) const noexcept;

  private:
    struct Program {
        std::string name;
        std::vector<state::ParamSetting> values;
    };
    std::vector<Program> programs_;
};

} // namespace pulp::format
