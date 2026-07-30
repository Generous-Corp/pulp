#pragma once

// What the generator is saying, turned into something a screen can show.
//
// The generator already says everything worth knowing — which gate rejected an
// attempt and why, a retry starting, a capability refusal naming the free
// modules that would satisfy it. All of it went to
// `~/Library/Application Support/Forge Modular/last-run.log`, which nobody
// opens, while the app showed one unchanging word. A real refusal reached a
// human that way and read as a hang.
//
// So: tail the log, classify each line, and let the chrome render it.
//
// The classification is deliberately conservative. A line that matches nothing
// is progress, not failure — inventing failures from unfamiliar output would be
// worse than staying quiet, because the generator's wording will drift and this
// must not start crying wolf when it does.

#include <cstdint>
#include <string>
#include <vector>

namespace forge_modular {

/// How a run ended, or that it has not.
enum class BuildOutcome {
    running,   ///< still going; no verdict yet
    done,      ///< an artifact exists
    refused,   ///< the generator declined, with a reason and usually a next step
    failed,    ///< something broke
};

/// One line, with what it means.
struct BuildLine {
    enum class Kind {
        progress,   ///< ordinary narration
        gate,       ///< a gate rejected an attempt; the reason is worth reading
        retry,      ///< trying again after a rejection
        refusal,    ///< the run stopped because something is missing
        error,      ///< a traceback or hard failure
        success,    ///< the artifact landed
    };
    Kind kind = Kind::progress;
    std::string text;
};

/// Which of the chrome's five stage chips a line implies, or -1.
///
/// Thinking / Writing files / Building / Verifying / Installing. Derived from
/// the generator's own words so the card beside the transcript agrees with it
/// rather than sitting grey through a whole run.
int stage_of(const std::string& line);

/// Reads a generator log as it grows.
///
/// Polls rather than watches: a generation lasts minutes and a second's latency
/// is invisible, while a file watcher is another thing to get wrong on three
/// platforms.
class BuildMonitor {
public:
    /// Classify one line. Static and pure so the rules can be tested without a
    /// file, a process, or a clock.
    static BuildLine::Kind classify(const std::string& line);

    /// The verdict implied by everything seen so far.
    static BuildOutcome outcome_of(const std::vector<BuildLine>& lines);

    /// Point it at a log and forget whatever came before.
    void watch(std::string path);

    /// Read whatever is new. Call it from a UI tick; returns the lines added.
    std::vector<BuildLine> poll();

    const std::vector<BuildLine>& lines() const { return lines_; }
    BuildOutcome outcome() const { return outcome_of(lines_); }

    /// The last line worth putting in a one-line status, or empty.
    ///
    /// A refusal outranks a gate rejection outranks the most recent narration:
    /// when a run has stopped, the reason it stopped is the only thing a person
    /// needs, and burying it under "step 7 of 9" is how this failed before.
    std::string headline() const;

    /// The furthest stage reached so far, or -1. Furthest rather than latest:
    /// a retry re-runs earlier stages, and a card that walks backwards reads
    /// as the build losing ground.
    int stage() const;

private:
    std::string path_;
    std::uint64_t offset_ = 0;
    std::vector<BuildLine> lines_;
};

}  // namespace forge_modular
