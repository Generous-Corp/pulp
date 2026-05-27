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
    // MOTU Digital Performer's macOS process is "DP11" / "DP10" /
    // "Digital Performer"; the Windows build is just "Digital
    // Performer.exe". Match the canonical name first, then the
    // short DP-prefixed form. Pulp #3046.
    if (name.find("digital performer") != std::string::npos
        || name.find("digitalperformer") != std::string::npos
        || name == "dp11" || name == "dp10" || name == "dp12") return HostType::DigitalPerformer;
    // Mixbus32C must be checked before Ardour: Harrison Mixbus 32C is an
    // Ardour derivative whose process name carries the "mixbus" /
    // "mixbus 32c" / "mixbus32c" substring while sometimes also retaining
    // the "ardour" lineage. The quirks diverge enough (Mixbus inherits
    // the same setBusArrangements bug *plus* additional Harrison-side
    // behavior) that we classify it separately.
    if (name.find("mixbus32c") != std::string::npos
        || name.find("mixbus 32c") != std::string::npos
        || name.find("mixbus") != std::string::npos) return HostType::Mixbus32C;
    if (name.find("ardour") != std::string::npos) return HostType::Ardour;
    if (name.find("pulp") != std::string::npos) return HostType::Standalone;

    return HostType::Unknown;
}

HostType host_type_from_auv3_wrapper(std::string_view wrapper_identifier) {
    if (wrapper_identifier.empty()) return HostType::Unknown;
    std::string id = to_lower(wrapper_identifier);

    // Logic Pro family — bundle ids + XPC wrapper names. MainStage
    // historically shares the Logic Pro DAW-quirk family (auv3 cross-host
    // row 21 dual-tracked bypass, channel-strip plumbing), so we fold it
    // into HostType::LogicPro rather than introducing a separate enum.
    if (id.find("com.apple.logic") != std::string::npos) return HostType::LogicPro;
    if (id.find("com.apple.mainstage") != std::string::npos) return HostType::LogicPro;
    if (id.find("auhostingservicexpc_arrow") != std::string::npos) return HostType::LogicPro;
    // GarageBand 10+ uses bundle id `com.apple.garageband10`.
    if (id.find("com.apple.garageband") != std::string::npos) return HostType::GarageBand;
    // Reuse the executable-path classifier for anything else — many
    // wrapper identifiers happen to share the same substrings (`reaper`,
    // `cubase`, `ableton`, etc.) when the host advertises through
    // `NSExtensionContext`. The classifier already handles those.
    HostType derived = host_type_from_process_name(id);
    if (derived != HostType::Unknown) return derived;

    // Apple's generic `AUHostingService` shows up when an unspecified
    // wrapper hosts an AU v3 extension and no host bundle id is
    // advertised. Treat as Unknown so the caller falls back to its
    // executable-path heuristic.
    return HostType::Unknown;
}

#ifndef __APPLE__
std::string current_auv3_wrapper_identifier() { return {}; }
#endif

HostType detect_host_type() {
#ifdef __APPLE__
    // DAW-quirks row 22 — when running as an AU v3 extension prefer the
    // wrapper-reported identifier (`detect_host_type` on iOS / sandboxed
    // macOS extensions will otherwise classify our own `.appex`
    // executable name as Unknown).
    auto wrapper_id = current_auv3_wrapper_identifier();
    if (!wrapper_id.empty()) {
        HostType wrapped = host_type_from_auv3_wrapper(wrapper_id);
        if (wrapped != HostType::Unknown) return wrapped;
    }
#endif
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
        case HostType::Mixbus32C: return "Harrison Mixbus 32C";
        case HostType::DigitalPerformer: return "Digital Performer";
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
