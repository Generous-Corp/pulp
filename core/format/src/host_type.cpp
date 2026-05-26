#include <pulp/format/host_type.hpp>
#include <algorithm>
#include <cctype>
#include <string_view>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <fstream>
#include <unistd.h>
#endif

namespace pulp::format {

static std::string get_process_name() {
#ifdef __APPLE__
    char path[1024];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0)
        return path;
    return "";
#elif defined(_WIN32)
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    return path;
#else
    char path[1024];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len > 0) { path[len] = '\0'; return path; }
    return "";
#endif
}

static std::string to_lower(std::string_view value) {
    std::string s(value);
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

HostType host_type_from_process_name(std::string_view process_name) {
    std::string name = to_lower(process_name);

    if (name.find("logic") != std::string::npos) return HostType::LogicPro;
    if (name.find("garageband") != std::string::npos) return HostType::GarageBand;
    if (name.find("ableton") != std::string::npos || name.find("live") != std::string::npos) return HostType::AbletonLive;
    if (name.find("reaper") != std::string::npos) return HostType::Reaper;
    if (name.find("protools") != std::string::npos || name.find("pro tools") != std::string::npos) return HostType::ProTools;
    // Wavelab must be checked before Cubase / Nuendo because the
    // Steinberg Wavelab executable does NOT contain "cubase" but the
    // overlap-check still keeps Wavelab as its own classifier rather
    // than folding it into the Cubase family — its quirks diverge.
    if (name.find("wavelab") != std::string::npos) return HostType::Wavelab;
    if (name.find("cubase") != std::string::npos) return HostType::Cubase;
    if (name.find("nuendo") != std::string::npos) return HostType::Nuendo;
    if (name.find("studio one") != std::string::npos || name.find("studioone") != std::string::npos) return HostType::StudioOne;
    if (name.find("fl studio") != std::string::npos || name.find("flstudio") != std::string::npos || name.find("fl64") != std::string::npos) return HostType::FLStudio;
    if (name.find("bitwig") != std::string::npos) return HostType::Bitwig;
    if (name.find("maschine") != std::string::npos) return HostType::Maschine;
    if (name.find("audacity") != std::string::npos || name.find("tenacity") != std::string::npos) return HostType::AudacityTenacity;
    if (name.find("ardour") != std::string::npos) return HostType::Ardour;
    if (name.find("pulp") != std::string::npos) return HostType::Standalone;

    return HostType::Unknown;
}

HostType detect_host_type() {
    return host_type_from_process_name(get_process_name());
}

std::string host_type_name(HostType type) {
    switch (type) {
        case HostType::LogicPro: return "Logic Pro";
        case HostType::GarageBand: return "GarageBand";
        case HostType::AbletonLive: return "Ableton Live";
        case HostType::Reaper: return "REAPER";
        case HostType::ProTools: return "Pro Tools";
        case HostType::Cubase: return "Cubase";
        case HostType::Nuendo: return "Nuendo";
        case HostType::Wavelab: return "WaveLab";
        case HostType::StudioOne: return "Studio One";
        case HostType::FLStudio: return "FL Studio";
        case HostType::Bitwig: return "Bitwig Studio";
        case HostType::Maschine: return "Maschine";
        case HostType::AudacityTenacity: return "Audacity/Tenacity";
        case HostType::Ardour: return "Ardour";
        case HostType::Standalone: return "Pulp Standalone";
        case HostType::Other: return "Other";
        case HostType::Unknown: return "Unknown";
    }
    return "Unknown";
}

bool host_supports_resize(HostType type) {
    // Most modern DAWs support plugin window resizing
    switch (type) {
        case HostType::ProTools: return false;  // AAX has fixed sizes
        case HostType::GarageBand: return false;  // Limited UI support
        default: return true;
    }
}

bool host_supports_sidechain(HostType type) {
    switch (type) {
        case HostType::GarageBand: return false;
        case HostType::AudacityTenacity: return false;
        default: return true;
    }
}

}  // namespace pulp::format
