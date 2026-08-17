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

namespace {
std::string lower_of(const std::string& s) {
    std::string out = s;
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
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
        contains(lower, "already running against this module pack") ||
        contains(lower, "generation cancelled by user") ||
        // The curation gates: the request is understood and declined because
        // no validated capability backs it. Nothing is sent to the model, so
        // the run ends here and the wording IS the answer -- as unmatched
        // progress it left the app spinning on a build that stopped seconds in.
        contains(lower, "no curated per-sample dsp capability matches this module request.") ||
        contains(lower, "the curated shortlist has helpers but lacks the requested core") ||
        contains(lower, "this request matches only generic module helpers and no direct") ||
        contains(lower, "the requested structure has no port-complete installed module") ||
        contains(lower, "the refinement base does not pass current static checks")) {
        return BuildLine::Kind::refusal;
    }

    // A traceback is unambiguous. Checked before the generic "error", because a
    // Python traceback's first line says nothing about what broke.
    if (contains(line, "Traceback (most recent call last)") ||
        contains(lower, "fatal error") || contains(lower, "no such file") ||
        // The generator's own way of ENDING without an artifact:
        //
        //   patch.py:  raise SystemExit(f"gave up after {n} attempts")
        //
        // Without it a build that gave up read as progress, the outcome stayed
        // `running`, and the app watched a dead build forever — the same fault
        // the success rule above was written to fix, from the other side.
        //
        // "PATCH GATE FAILED" is deliberately NOT here. It is an attempt
        // ending, not a build ending: the generator retries, and most runs
        // that print it go on to produce a patch. outcome_of ranks errored
        // ABOVE succeeded, so counting it would make a recovered run report
        // failed and hide the artifact it had just built — verified against a
        // real log with two gate failures and a patch at the end.
        // Every way the two generators END without an artifact. Each is a
        // `raise SystemExit(...)` in patch.py or generate.py, and each one
        // missing here is a build the app watches forever: the outcome stays
        // `running`, the stage never resolves, and there is nothing to open.
        //
        // "model call failed" matters most — it is what a machine whose model
        // CLI cannot reach its credential prints, which is the state of any
        // SSH session and of a locked keychain.
        //
        // tools/rack/test_generator_endings.py keeps this list and the
        // generators' own SystemExit strings in step.
        contains(lower, "gave up after") ||
        contains(lower, "model call failed") ||
        contains(lower, "generation stop failed") ||
        contains(lower, "model cli is not logged in") ||
        contains(lower, "could not fetch the library catalog") ||
        contains(lower, "could not fetch the module index") ||
        contains(lower, "is not sound") ||
        contains(lower, "did not contain both a json") ||
        contains(lower, "duplicate addmodel") ||
        // A prerequisite this Mac does not have -- the model CLI, or Apple's
        // Command Line Tools. Both are a real stop, and both are things the
        // person can fix in a minute once they are told.
        contains(lower, "is missing something") ||
        contains(lower, "is not installed, so nothing can be generated") ||
        // Kept deliberately even though no generator prints it during a BUILD:
        // it is raised at import/setup, before a run starts, and matching it
        // costs nothing. Removing it as "orphaned" made the app read that
        // ending as ordinary progress.
        contains(lower, "rack sdk not found") ||
        contains(lower, "the rack sdk is not installed") ||
        contains(lower, "could not download the rack sdk") ||
        contains(lower, "unknown setting") ||
        contains(lower, "two manifests claim") ||
        // Argument handling that reaches a watched log, because the app builds
        // the command line: a rejected flag ends the run with nothing to open.
        contains(lower, "--base requires an existing .vcv path") ||
        contains(lower, "--response-file requires a saved response path") ||
        contains(lower, "--retries requires a non-negative integer") ||
        contains(lower, "--keep-on-fail is disabled: a failed attempt must not poison the") ||
        // Evidence and prompt-record I/O. These reserve a file BEFORE the model
        // call, so failing here means the run stopped without spending anything.
        contains(lower, "cannot read refinement base") ||
        contains(lower, "cannot read saved model response") ||
        contains(lower, "cannot reserve generation evidence before the model call") ||
        contains(lower, "cannot reserve the model response before the call") ||
        contains(lower, "cannot write requested output") ||
        contains(lower, "cannot write generated patches directory") ||
        contains(lower, "model prompt record exhausted its") ||
        contains(lower, "refusing to overwrite model prompt record") ||
        contains(lower, "saved model response is empty") ||
        contains(lower, "the codex prompt has malformed inventory boundary markers;") ||
        contains(lower, "the codex prompt is too large to send inline and has no") ||
        contains(lower, "the curated dsp capability selection was not valid json") ||
        // An install that cannot proceed: a missing pack, a toolchain that did
        // not land, or a generator copy shipped without its payload.
        contains(lower, "the module pack at") ||
        contains(lower, "the toolchain could not be installed, so no module can be built.") ||
        contains(lower, "the toolchain reported success but") ||
        contains(lower, "this copy of the generator is incomplete, so a module cannot be")) {
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
        contains(lower, "uses pulp dsp") || contains(lower, "open it with") ||
        // patch.py's finish line: "built 8 modules, 10 cables -> <path>". The
        // two generators word success differently, and reading only the module
        // one left every patch build stuck at "running" forever -- so the
        // skeleton never resolved and Open in Rack never appeared, while the
        // patch itself was sitting on disk.
        (contains(lower, "built ") && contains(lower, " modules"))) {
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
        // A refusal is a BLOCK, not a line.
        //
        // The generator prints a header, then the modules that would satisfy
        // the request, then what to do:
        //
        //   hold on — this asks for something you don't have installed:
        //   no arpeggiator module is installed. These would do it:
        //       free  Ahornberg/Tracker            Tracker
        //       free  AmalgamatedHarmonics/Arp31   Arp 3.1 - Chord
        //   install one in Rack's Library, then ask again —
        //
        // Only the first and last lines matched a rule, so the app showed
        // "you don't have something" and "go install one" with the three free
        // downloads in between silently dropped. A refusal with no options is
        // a dead end; the options are the whole value of it. Everything from
        // the header to the closing line is carried as part of the refusal.
        if (in_refusal_ && bl.kind != BuildLine::Kind::refusal)
            bl.kind = BuildLine::Kind::refusal;
        if (bl.kind == BuildLine::Kind::refusal) {
            in_refusal_ = true;
            if (contains(lower_of(line), "then ask again") ||
                contains(lower_of(line), "install one in rack"))
                in_refusal_ = false;          // the closing line ends the block
        }
        lines_.push_back(bl);
        added.push_back(bl);
    }
    offset_ = size;
    return added;
}

std::vector<std::string> BuildMonitor::closing_block(
    const std::vector<BuildLine>& lines) {
    // From the LAST ending, not the first. A run can print a recoverable
    // ending-shaped line early -- a model call that failed and was retried --
    // and starting there would hand back the whole rest of the transcript.
    std::size_t start = lines.size();
    for (std::size_t i = lines.size(); i-- > 0;) {
        if (lines[i].kind == BuildLine::Kind::error ||
            lines[i].kind == BuildLine::Kind::refusal) {
            start = i;
            break;
        }
    }
    std::vector<std::string> out;
    for (std::size_t i = start; i < lines.size(); ++i)
        out.push_back(lines[i].text);
    return out;
}

std::string format_failure_report(const RunFailure& f) {
    std::string out = "Forge Modular";
    if (!f.app_version.empty()) out += " " + f.app_version;
    out += ": a build failed.\n\n";
    if (!f.request.empty()) out += "Asked for: " + f.request + "\n";
    if (!f.headline.empty()) out += "Stopped because: " + f.headline + "\n";
    // The patch FIRST among the paths. It is the one thing here a person can
    // act on in ten seconds, and burying it under a log path is how the whole
    // handover gets missed.
    if (!f.artifact.empty())
        out += "Unfinished patch: " + f.artifact + "\n";
    if (!f.log_path.empty()) out += "Run log: " + f.log_path + "\n";
    // The transcript, whole. This is the part the user asked for by name:
    // "i didn't even get a way to copy the prompt output". A summary of a
    // failure is the thing they already had.
    if (!f.log_text.empty())
        out += "\n----- the run -----\n" + f.log_text +
               (f.log_text.back() == '\n' ? "" : "\n");
    return out;
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
