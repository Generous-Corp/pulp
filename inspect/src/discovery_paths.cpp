#include <pulp/inspect/discovery.hpp>

#include <pulp/runtime/system.hpp>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace pulp::inspect {

std::filesystem::path default_inspector_runtime_directory() {
    if (const auto configured =
            pulp::runtime::get_env("PULP_INSPECTOR_RUNTIME_DIR")) {
        return *configured;
    }
#ifdef _WIN32
    const auto base = pulp::runtime::get_env("LOCALAPPDATA")
                          .value_or(std::filesystem::temp_directory_path().string());
    return std::filesystem::path(base) / "Pulp" / "Inspector";
#else
    const auto base = pulp::runtime::get_env("TMPDIR")
                          .value_or(std::filesystem::temp_directory_path().string());
    return std::filesystem::path(base) /
           ("pulp-inspector-" + std::to_string(::geteuid()));
#endif
}

}  // namespace pulp::inspect
