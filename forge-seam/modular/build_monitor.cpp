#include "forge/build_monitor.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace forge_modular {

namespace {

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

std::string lowered(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

BuildLine::Kind BuildMonitor::classify(const std::string& line) {
    const auto lower = lowered(line);

    // A refusal first: it is the one a person must not miss, and its wording is
    // deliberate on the generator's side. This is the shape that reached a human
    // as silence -- "hold on -- this asks for something you don't have
    // installed", followed by the free modules that would satisfy it.
    if (contains(lower, "hold on") || contains(lower, "you don't have installed") ||
        contains(lower, "install one in rack") || contains(lower, "asks for something") ||
        // The generator refuses a second run against the same module pack --
        // easy to reach with the standalone and the plugin open together.
        // Classified as a refusal so the run reaches a verdict: as ordinary
        // progress it never terminates, and the app spins on a build that
        // exited immediately.
        contains(lower, "already running against this module pack")) {
        return BuildLine::Kind::refusal;
    }

    // A traceback is unambiguous. Checked before the generic "error", because a
    // Python traceback's first line says nothing about what broke.
    if (contains(line, "Traceback (most recent call last)") ||
        contains(lower, "fatal error") || contains(lower, "no such file")) {
        return BuildLine::Kind::error;
    }

    if (contains(lower, "asking the model") || contains(lower, "retry")) {
        return BuildLine::Kind::retry;
    }

    // A gate rejection is not a failure -- it is the pipeline working, and the
    // reason is the most useful thing on the screen while a build is running.
    if (contains(lower, "defaults to the top of its range") ||
        contains(lower, "nowhere left to turn") ||
        contains(lower, "uses no pulp dsp") ||
        contains(lower, "rejected at") ||
        contains(lower, "silent")) {
        return BuildLine::Kind::gate;
    }

    if (contains(lower, "installed \xE2\x86\x92") || contains(lower, "installed ->") ||
        contains(lower, "uses pulp dsp") || contains(lower, "open it with")) {
        return BuildLine::Kind::success;
    }

    // Anything else is narration. Unfamiliar output must not become a failure:
    // the generator's wording will drift, and a classifier that invents errors
    // from it would cry wolf on every change.
    return BuildLine::Kind::progress;
}

int stage_of(const std::string& line) {
    const auto lower = lowered(line);
    if (contains(lower, "installed \xE2\x86\x92") || contains(lower, "installed ->") ||
        contains(lower, "open it with"))
        return 4;   // Installing
    if (contains(lower, "behaviour verified") || contains(lower, "gate passed") ||
        contains(lower, "gate failed") || contains(lower, "behavioural gate"))
        return 3;   // Verifying
    if (contains(lower, "compiled") || contains(lower, "compile failed"))
        return 2;   // Building
    if (contains(lower, "generated ") || contains(lower, "manifest") ||
        contains(lower, "cpp:") || contains(lower, "panel"))
        return 1;   // Writing files
    if (contains(lower, "asking the model"))
        return 0;   // Thinking
    return -1;
}

int BuildMonitor::stage() const {
    int furthest = -1;
    for (const auto& l : lines_) {
        const int s = stage_of(l.text);
        if (s > furthest) furthest = s;
    }
    return furthest;
}

BuildOutcome BuildMonitor::outcome_of(const std::vector<BuildLine>& lines) {
    // A refusal beats a success: a run that installed something and then refused
    // the next step has still stopped, and reporting "done" would be a lie about
    // what the user asked for.
    bool refused = false, errored = false, succeeded = false;
    for (const auto& l : lines) {
        if (l.kind == BuildLine::Kind::refusal) refused = true;
        if (l.kind == BuildLine::Kind::error) errored = true;
        if (l.kind == BuildLine::Kind::success) succeeded = true;
    }
    if (refused) return BuildOutcome::refused;
    if (errored) return BuildOutcome::failed;
    if (succeeded) return BuildOutcome::done;
    return BuildOutcome::running;
}

void BuildMonitor::watch(std::string path) {
    path_ = std::move(path);
    offset_ = 0;
    lines_.clear();
}

std::vector<BuildLine> BuildMonitor::poll() {
    std::vector<BuildLine> added;
    if (path_.empty()) return added;

    std::ifstream f(path_, std::ios::binary);
    if (!f) return added;

    f.seekg(0, std::ios::end);
    const auto size = static_cast<std::uint64_t>(f.tellg());
    // A shrinking file means a new run truncated it. Start over rather than
    // seeking past the end and reading nothing for the rest of the session.
    if (size < offset_) {
        offset_ = 0;
        lines_.clear();
    }
    if (size == offset_) return added;

    f.seekg(static_cast<std::streamoff>(offset_), std::ios::beg);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos) continue;
        BuildLine bl{classify(line), line};
        lines_.push_back(bl);
        added.push_back(bl);
    }
    offset_ = size;
    return added;
}

std::string BuildMonitor::headline() const {
    // Walk backwards for the most recent line of each rank, then pick by rank.
    const BuildLine* refusal = nullptr;
    const BuildLine* error = nullptr;
    const BuildLine* gate = nullptr;
    const BuildLine* last = nullptr;
    for (const auto& l : lines_) {
        switch (l.kind) {
            // FIRST refusal, not last: a refusal opens with the reason and
            // closes with the remedy, and "install one in Rack's Library" on
            // its own does not say what for.
            case BuildLine::Kind::refusal: if (!refusal) refusal = &l; break;
            case BuildLine::Kind::error:   if (!error) error = &l; break;
            case BuildLine::Kind::gate:    gate = &l;    break;
            default: break;
        }
        last = &l;
    }
    if (refusal) return refusal->text;
    if (error) return error->text;
    if (gate) return gate->text;
    return last ? last->text : std::string{};
}

}  // namespace forge_modular
