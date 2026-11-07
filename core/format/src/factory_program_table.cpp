#include <pulp/format/factory_program_table.hpp>

#include <pulp/runtime/log.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;

namespace pulp::format {

fs::path factory_presets_dir_for_module(const fs::path& module) {
    if (module.empty())
        return {};

    // The binary sits at most two levels under Contents; walk up that far.
    fs::path dir = module.parent_path();
    for (int depth = 0; depth < 2 && !dir.empty(); ++depth) {
        if (dir.filename() == "Contents")
            return dir / "Resources" / "Presets";
        dir = dir.parent_path();
    }
    return {};
}

fs::path loaded_module_factory_presets_dir() {
#if defined(_WIN32)
    HMODULE module = nullptr;
    // Any symbol in this translation unit resolves to the image the adapter is
    // linked into (the plug-in binary), not the host executable.
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&factory_presets_dir_for_module), &module))
        return {};
    wchar_t buffer[MAX_PATH * 4] = {};
    const DWORD length = GetModuleFileNameW(module, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer))
        return {};
    return factory_presets_dir_for_module(fs::path(std::wstring(buffer, length)));
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&factory_presets_dir_for_module), &info) == 0 ||
        info.dli_fname == nullptr)
        return {};
    return factory_presets_dir_for_module(fs::path(info.dli_fname));
#endif
}

void FactoryProgramTable::build(state::StateStore& store, const std::string& manufacturer,
                                const std::string& plugin_name, const fs::path& dir) {
    programs_.clear();

    state::PresetManager presets(store, manufacturer, plugin_name);
    presets.set_factory_presets_dir(dir.empty() ? loaded_module_factory_presets_dir() : dir);

    for (const auto& info : presets.factory_presets()) {
        auto values = presets.read_values(info.path);
        if (!values) {
            runtime::log_warn("Factory preset '{}' could not be read; leaving it out",
                              info.path.string());
            continue;
        }
        programs_.push_back({info.name, std::move(*values)});
    }

    if (!programs_.empty()) {
        runtime::log_debug("{} factory preset(s) from '{}'", programs_.size(),
                           presets.factory_presets_dir().string());
    }
}

const std::string& FactoryProgramTable::name_at(std::size_t index) const noexcept {
    static const std::string empty;
    return index < programs_.size() ? programs_[index].name : empty;
}

std::span<const state::ParamSetting>
FactoryProgramTable::values_at(std::size_t index) const noexcept {
    if (index >= programs_.size())
        return {};
    return programs_[index].values;
}

} // namespace pulp::format
