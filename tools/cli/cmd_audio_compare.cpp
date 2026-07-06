// cmd_audio_compare.cpp — `pulp audio compare` (advisory, agent-facing).
//
// Thin orchestrator: locate the opt-in Audio Quality Lab managed tool and
// forward the `compare` verb to it. All measurement/DSP lives in the tool
// (tools/audio/quality-lab); the CLI never links numpy/soundfile/FFT.

#include "cmd_audio_compare.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <pulp/platform/child_process.hpp>

#include "tool_registry.hpp"

namespace fs = std::filesystem;

namespace {

constexpr const char* kToolId = "audio-quality-lab";

// Options that consume the following token as their value — used only to count
// positionals for a friendly local arity error; all args are forwarded verbatim.
bool takes_value(const std::string& opt) {
    return opt == "--profile" || opt == "--reference-role"
        || opt == "--threshold" || opt == "--json" || opt == "--align";
}

void print_compare_usage() {
    std::cout <<
        "pulp audio compare — advisory before/after audio judgment (agent-facing; not a gate)\n\n"
        "Usage:\n"
        "  pulp audio compare <reference.wav> <candidate.wav> [options]\n\n"
        "Options:\n"
        "  --profile <tonal-balance|added-hf|noise-roughness|graininess|stereo-width|transient-integrity>   measurement axis (default: tonal-balance)\n"
        "  --reference-role <peer|golden>       golden enables regression_suspected (default: peer)\n"
        "  --align <none|latency|varispeed:R|stretch:R|pitch:S>  time-align first: latency trims a\n"
        "                                       constant delay/offset; varispeed:R undoes a declared tape-speed\n"
        "                                       change (resample); stretch:R declares a pitch-preserving\n"
        "                                       time-stretch; pitch:S a duration-preserving shift (S semitones).\n"
        "                                       Each refuses if the audio doesn't support it (default: none)\n"
        "  --threshold <t>                      materiality override (axis default otherwise)\n"
        "  --json <path>                        write the full quality_lab.compare.v1 report JSON\n\n"
        "Delegates to the opt-in Audio Quality Lab tool. Advisory: exits non-zero ONLY when it\n"
        "could not measure (invalid input), never for a judgment. For an exact/null/spectral gate\n"
        "diff with a pass/fail exit, use `pulp audio validate compare` instead.\n";
}

// Walk up from cwd for tools/packages/tool-registry.json (mirrors import_run.cpp).
fs::path find_tool_registry() {
    fs::path cwd = fs::current_path();
    while (true) {
        auto p = cwd / "tools" / "packages" / "tool-registry.json";
        if (fs::exists(p)) return p;
        if (cwd.has_parent_path() && cwd.parent_path() != cwd)
            cwd = cwd.parent_path();
        else
            break;
    }
    return {};
}

// Resolve the quality-lab descriptor from the registry, or a minimal fallback
// (locate_tool needs only id + install_method to find the venv wrapper), so the
// command works both inside a checkout and from a standalone install.
pulp::cli::tools::ToolDescriptor resolve_quality_lab() {
    auto reg_path = find_tool_registry();
    if (!reg_path.empty()) {
        auto [reg, err] = pulp::cli::tools::load_tool_registry(reg_path);
        if (err.empty()) {
            auto it = reg.tools.find(kToolId);
            if (it != reg.tools.end()) return it->second;
        }
    }
    pulp::cli::tools::ToolDescriptor t;
    t.id = kToolId;
    t.install_method = "python_pip";
    t.module = "quality_lab.cli";
    return t;
}

}  // namespace

int cmd_audio_compare(const std::vector<std::string>& args) {
    if (!args.empty() && (args[0] == "-h" || args[0] == "--help")) {
        print_compare_usage();
        return 0;
    }

    // Light local parse for a crisp arity error before we spawn anything.
    int positionals = 0;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (!a.empty() && a.front() == '-') {
            if (takes_value(a)) ++i;  // skip its value
            continue;
        }
        ++positionals;
    }
    if (positionals < 2) {
        std::cerr << "pulp audio compare: need <reference.wav> and <candidate.wav>.\n\n";
        print_compare_usage();
        return 2;  // could-not-measure (bad invocation), matching the tool's invalid code
    }

    auto tool = resolve_quality_lab();
    auto loc = pulp::cli::tools::locate_tool(tool);
    if (!loc.found) {
        std::cerr <<
            "Audio Quality Lab is not installed (opt-in developer tool).\n"
            "Install it (needs a Pulp source checkout + network), then retry:\n"
            "  pulp tool install audio-quality-lab\n";
        return 1;  // setup error, distinct from a measurement result
    }

    // Forward the whole invocation to the tool's `compare` verb. The tool owns
    // level-matching, the axis kernels, the evidence envelope, verdict, and the
    // advisory exit code (2 == invalid). We passthrough its output + exit code.
    std::vector<std::string> argv;
    argv.reserve(args.size() + 1);
    argv.emplace_back("compare");
    for (const auto& a : args) argv.push_back(a);

    auto result = pulp::platform::exec(loc.path.string(), argv, /*timeout_ms=*/180000);
    if (!result.stdout_output.empty()) std::cout << result.stdout_output;
    if (!result.stderr_output.empty()) std::cerr << result.stderr_output;
    if (result.timed_out) {
        std::cerr << "pulp audio compare: the quality-lab tool timed out.\n";
        return 1;
    }
    return result.exit_code;
}
