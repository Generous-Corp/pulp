// Portable conformance runner for the timeline fixture corpus.
//
// One binary validates the whole corpus with no Catch2 and no desktop
// dependencies, so the same executable runs under desktop ctest, on an Android
// emulator via `adb push`, and compiled to WASM. It links only pulp::timeline
// and pulp::interchange, both of which sit on the portable floor. The runner
// lives inside core/interchange rather than beside the corpus so that claim is
// checked rather than asserted: the interchange dependency floor gate scans
// this file, so reaching for a view, host, or format header fails CI.
//
// A corpus entry is a document plus a sibling `.expect` manifest stating what
// the document is: its schema envelope version, identity, structural counts,
// the interchange concepts it uses, and the ordered identities of every
// collection in its arrangement. Without the manifest a fixture's meaning lives
// only in whichever test happens to load it.
//
// Counts alone cannot state meaning. A count says three tracks survived; it
// cannot say which three, nor in what order — so a decoder that drops an
// authored ordering, remaps identities, or permutes a collection still reports
// the same count, and idempotence holds because both directions of the round
// trip agree on the same wrong answer. Recording the ordered identities is what
// closes that gap: they are the part of a document a count is structurally
// unable to see.
//
// The corpus is enumerated by an index file rather than by walking directories,
// because directory iteration is unreliable in the WASM and emulator lanes this
// binary exists to serve, and because an explicit index makes an unreferenced
// fixture a visible omission rather than a silent one.

#include <pulp/interchange/census.hpp>
#include <pulp/interchange/concept.hpp>
#include <pulp/timeline/model.hpp>
#include <pulp/timeline/schema_registry.hpp>
#include <pulp/timeline/serialize.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Expectation {
    std::string key;
    std::string value;
};

struct Failure {
    std::string fixture;
    std::string detail;
};

std::string read_file(const std::string& path, bool& ok) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.good()) {
        ok = false;
        return {};
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    ok = true;
    return buffer.str();
}

/// Parses a line-oriented `<key> <value>` file: one pair per line, `#`
/// comments and blank lines ignored. Deliberately not JSON — the runner must
/// stay dependency-free, and a flat text manifest diffs readably when a
/// conformance failure lands in review.
std::vector<Expectation> parse_key_value_lines(std::string_view text) {
    std::vector<Expectation> parsed;
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        const auto newline = text.find('\n', cursor);
        auto line = text.substr(cursor, newline == std::string_view::npos ? newline
                                                                          : newline - cursor);
        cursor = newline == std::string_view::npos ? text.size() : newline + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.remove_suffix(1);
        while (!line.empty() && line.front() == ' ')
            line.remove_prefix(1);
        if (line.empty() || line.front() == '#')
            continue;
        const auto space = line.find(' ');
        if (space == std::string_view::npos) {
            parsed.push_back({std::string(line), {}});
            continue;
        }
        auto value = line.substr(space + 1);
        while (!value.empty() && value.front() == ' ')
            value.remove_prefix(1);
        parsed.push_back({std::string(line.substr(0, space)), std::string(value)});
    }
    return parsed;
}

std::string find_expectation(const std::vector<Expectation>& expectations, std::string_view key) {
    for (const auto& entry : expectations)
        if (entry.key == key)
            return entry.value;
    return {};
}

bool has_expectation(const std::vector<Expectation>& expectations, std::string_view key) {
    for (const auto& entry : expectations)
        if (entry.key == key)
            return true;
    return false;
}

void expect_equal(std::vector<std::string>& mismatches, const std::vector<Expectation>& expected,
                  std::string_view key, const std::string& actual) {
    if (!has_expectation(expected, key)) {
        mismatches.push_back(std::string(key) + ": missing from manifest (actual " + actual + ")");
        return;
    }
    const auto declared = find_expectation(expected, key);
    if (declared != actual)
        mismatches.push_back(std::string(key) + ": expected " + declared + ", got " + actual);
}

std::string to_text(std::uint64_t value) { return std::to_string(value); }

/// Renders the structural counts and identity that `peek_project_summary`
/// validates without constructing a Project.
void collect_summary(std::vector<std::pair<std::string, std::string>>& out,
                     const pulp::timeline::ProjectSnapshotSummary& summary) {
    out.emplace_back("schema_version", to_text(summary.schema_version));
    out.emplace_back("project_id", to_text(summary.project_id.value));
    // The authored project name is deliberately NOT recorded. Every value here
    // round-trips through a whitespace-delimited text line, and an authored name
    // is arbitrary user text: a trailing space is silently trimmed on read, and a
    // newline would corrupt the file outright. Either way the manifest generated
    // from a document would then fail to verify against that same document — a
    // gate raising a false failure on a legal input, which is as useless as one
    // that cannot fail at all. Identity is already carried by project_id, and a
    // renamed fixture is not a conformance regression.
    out.emplace_back("next_item_id", to_text(summary.next_item_id));
    out.emplace_back("root_sequence_id", to_text(summary.root_sequence_id.value));
    const auto& counts = summary.counts;
    out.emplace_back("counts.assets", to_text(counts.assets));
    out.emplace_back("counts.sequences", to_text(counts.sequences));
    out.emplace_back("counts.tracks", to_text(counts.tracks));
    out.emplace_back("counts.clips", to_text(counts.clips));
    out.emplace_back("counts.notes", to_text(counts.notes));
    out.emplace_back("counts.device_placements", to_text(counts.device_placements));
    out.emplace_back("counts.automation_lanes", to_text(counts.automation_lanes));
    out.emplace_back("counts.automation_points", to_text(counts.automation_points));
    out.emplace_back("counts.take_lanes", to_text(counts.take_lanes));
    out.emplace_back("counts.takes", to_text(counts.takes));
    out.emplace_back("counts.take_comp_segments", to_text(counts.take_comp_segments));
    out.emplace_back("counts.midi_lanes", to_text(counts.midi_lanes));
    out.emplace_back("counts.midi_lane_points", to_text(counts.midi_lane_points));
}

/// Records every concept the census found, keyed by its stable generated id so
/// the manifest never depends on enum ordering.
void collect_census(std::vector<std::pair<std::string, std::string>>& out,
                    const pulp::interchange::ConceptCensus& census) {
    for (const auto concept_value : census.present()) {
        out.emplace_back("concept." + std::string(pulp::interchange::concept_id(concept_value)),
                         to_text(census.count(concept_value)));
    }
}

/// Accumulates one ordered identity list as a single manifest value.
///
/// Comma-separated because a manifest value may not contain whitespace, and
/// identities are decimal integers, so no separator escaping is possible or
/// needed.
class IdentityOrder {
  public:
    void add(pulp::timeline::ItemId id) {
        if (!text_.empty())
            text_ += ',';
        text_ += to_text(id.value);
    }
    bool empty() const noexcept { return text_.empty(); }
    const std::string& text() const noexcept { return text_; }

  private:
    std::string text_;
};

/// Records `order` under `key`, omitting empty collections.
///
/// An empty collection has no writable value — the manifest format cannot
/// express one — so the key is left out, matching how `concept.` keys appear
/// only for concepts the document actually uses. Omission is not a blind spot
/// in either direction: a collection that empties out drops a key the manifest
/// still declares, and one that fills up adds a key the manifest never
/// declared. The runner already fails on both.
void record_order(std::vector<std::pair<std::string, std::string>>& out, std::string key,
                  const IdentityOrder& order) {
    if (order.empty())
        return;
    out.emplace_back(std::move(key), order.text());
}

/// Records the ordered identities of every collection in the arrangement.
///
/// The boundary is the arrangement spine — Project, Sequence, Scene, Track —
/// and stops there. Everything below is leaf content (notes, automation points,
/// MIDI lane points, takes, comp segments) whose size the counts already state
/// and whose identities would bloat a manifest without stating anything the
/// spine does not already pin. Two distinct classes of order live here and both
/// matter:
///
///   Authored orders — `track_order`, `scenes`, `device_chain` — exist only in
///   the document. Nothing else reconstructs them, and `track_order` in
///   particular degrades silently: an empty authored order is legal and makes
///   the sequence adopt the identity order of `tracks()`, so a decoder that
///   drops it produces a plausible arrangement rather than an error.
///
///   That fallback bounds what this can prove. `track_order()` presents the
///   identity order for a sequence that never recorded one, so the manifest
///   cannot tell "no authored order" apart from "authored order equals identity
///   order" — a document in either state is unchanged by the drop and stays
///   green. A fixture whose authored order is deliberately NOT the identity
///   order is what makes the check able to fail at all, which is why the corpus
///   carries one and a CLI case asserts the two orders still differ.
///
///   Value-derived orders — `markers`, `regions`, `clips` — are sorted by
///   position, so their identity order is a cheap proxy for the positions
///   themselves: a lost or collapsed position reorders the list.
///
/// Canonical identity orders (`tracks`, `automation_lanes`, `take_lanes`) carry
/// no ordering information beyond the id set, but the id set is itself
/// unstated by a count: three tracks with remapped identities still count three.
void collect_identity_orders(std::vector<std::pair<std::string, std::string>>& out,
                             const pulp::timeline::Project& project) {
    IdentityOrder assets;
    for (const auto& asset : project.assets())
        assets.add(asset.id);
    record_order(out, "order.assets", assets);

    IdentityOrder sequences;
    for (const auto& sequence : project.sequences())
        sequences.add(sequence.id());
    record_order(out, "order.sequences", sequences);

    for (const auto& sequence : project.sequences()) {
        const std::string prefix = "order.sequence." + to_text(sequence.id().value) + ".";

        IdentityOrder tracks;
        for (const auto& track : sequence.tracks())
            tracks.add(track.id());
        record_order(out, prefix + "tracks", tracks);

        IdentityOrder authored_tracks;
        for (const auto track_id : sequence.track_order())
            authored_tracks.add(track_id);
        record_order(out, prefix + "track_order", authored_tracks);

        IdentityOrder markers;
        for (const auto& marker : sequence.markers())
            markers.add(marker.id);
        record_order(out, prefix + "markers", markers);

        IdentityOrder regions;
        for (const auto& region : sequence.regions())
            regions.add(region.id);
        record_order(out, prefix + "regions", regions);

        IdentityOrder scenes;
        for (const auto& scene : sequence.scenes())
            scenes.add(scene.id);
        record_order(out, prefix + "scenes", scenes);

        for (const auto& scene : sequence.scenes()) {
            IdentityOrder slots;
            for (const auto& slot : scene.slots)
                slots.add(slot.id);
            record_order(out, "order.scene." + to_text(scene.id.value) + ".slots", slots);
        }

        for (const auto& track : sequence.tracks()) {
            const std::string track_prefix = "order.track." + to_text(track.id().value) + ".";

            IdentityOrder clips;
            for (const auto& clip : track.clips())
                clips.add(clip.id());
            record_order(out, track_prefix + "clips", clips);

            IdentityOrder devices;
            for (const auto& placement : track.device_chain())
                devices.add(placement.id);
            record_order(out, track_prefix + "device_chain", devices);

            IdentityOrder automation_lanes;
            for (const auto& lane : track.automation_lanes())
                automation_lanes.add(lane.id());
            record_order(out, track_prefix + "automation_lanes", automation_lanes);

            IdentityOrder take_lanes;
            for (const auto& lane : track.take_lanes())
                take_lanes.add(lane.id());
            record_order(out, track_prefix + "take_lanes", take_lanes);
        }
    }
}

struct FixtureOutcome {
    bool ok = false;
    std::vector<std::string> mismatches;
    std::vector<std::pair<std::string, std::string>> observed;
};

FixtureOutcome check_fixture(const std::string& corpus_root, const std::string& relative_path,
                             const pulp::timeline::SchemaRegistry& registry, bool update) {
    FixtureOutcome outcome;
    const std::string document_path = corpus_root + "/" + relative_path;
    bool read_ok = false;
    auto json = read_file(document_path, read_ok);
    if (!read_ok) {
        outcome.mismatches.push_back("cannot read document " + document_path);
        return outcome;
    }

    auto summary = pulp::timeline::peek_project_summary(json, registry);
    if (!summary) {
        outcome.mismatches.push_back(
            "peek_project_summary failed (persistence error " +
            to_text(static_cast<std::uint64_t>(summary.error().code)) + ")");
        return outcome;
    }
    collect_summary(outcome.observed, summary.value());

    auto project = pulp::timeline::deserialize_project(json, registry);
    if (!project) {
        outcome.mismatches.push_back(
            "deserialize_project failed (persistence error " +
            to_text(static_cast<std::uint64_t>(project.error().code)) + ")");
        return outcome;
    }
    collect_census(outcome.observed, pulp::interchange::census(project.value()));
    collect_identity_orders(outcome.observed, project.value());

    // Round-trip identity is asserted as serialize idempotence rather than as a
    // byte-compare against the fixture on disk: a fixture pinned at an older
    // schema version re-serializes at the current version by design, so the
    // fixture bytes are deliberately not the expected output.
    auto first = pulp::timeline::serialize_project(project.value(), registry);
    if (!first) {
        outcome.mismatches.push_back(
            "serialize_project failed (persistence error " +
            to_text(static_cast<std::uint64_t>(first.error().code)) + ")");
        return outcome;
    }
    auto reloaded = pulp::timeline::deserialize_project(first.value().json, registry);
    if (!reloaded) {
        outcome.mismatches.push_back("re-deserialize of serialized output failed");
        return outcome;
    }
    auto second = pulp::timeline::serialize_project(reloaded.value(), registry);
    if (!second || second.value().json != first.value().json) {
        outcome.mismatches.push_back("serialization is not idempotent across a round trip");
        return outcome;
    }

    if (update) {
        outcome.ok = true;
        return outcome;
    }

    const std::string expect_path = document_path + ".expect";
    bool expect_ok = false;
    auto expect_text = read_file(expect_path, expect_ok);
    if (!expect_ok) {
        outcome.mismatches.push_back("cannot read manifest " + expect_path +
                                     " (run with --update to generate)");
        return outcome;
    }
    const auto expected = parse_key_value_lines(expect_text);
    for (const auto& [key, value] : outcome.observed)
        expect_equal(outcome.mismatches, expected, key, value);

    // A manifest entry with no observed counterpart means the document lost a
    // concept or an entity. Silently passing that would make the corpus blind to
    // exactly the regression it exists to catch.
    for (const auto& entry : expected) {
        bool found = false;
        for (const auto& [key, value] : outcome.observed) {
            if (key == entry.key) {
                found = true;
                break;
            }
        }
        if (!found)
            outcome.mismatches.push_back(entry.key + ": declared in manifest but not observed");
    }

    outcome.ok = outcome.mismatches.empty();
    return outcome;
}

/// A manifest line is `<key> <value>`, so neither may contain whitespace or the
/// file cannot be read back. Every value recorded today is an integer and every
/// key is a generated concept id, but that is a property of today's vocabulary
/// rather than a guarantee: a future concept id containing a space would
/// otherwise write a manifest that silently fails to verify against the very
/// document it came from. Fail loudly here instead.
bool manifest_token_is_writable(const std::string& token) {
    if (token.empty())
        return false;
    for (const auto character : token) {
        if (character == ' ' || character == '\t' || character == '\n' || character == '\r')
            return false;
    }
    return true;
}

bool write_manifest(const std::string& path,
                    const std::vector<std::pair<std::string, std::string>>& observed) {
    for (const auto& [key, value] : observed) {
        if (!manifest_token_is_writable(key) || !manifest_token_is_writable(value)) {
            std::fprintf(stderr,
                         "refusing to write unreadable manifest entry '%s' = '%s' "
                         "(keys and values may not be empty or contain whitespace)\n",
                         key.c_str(), value.c_str());
            return false;
        }
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.good())
        return false;
    stream << "# Generated by pulp-fixture-runner --update. Edit the fixture, not this file.\n";
    for (const auto& [key, value] : observed)
        stream << key << ' ' << value << '\n';
    return stream.good();
}

void print_usage() {
    std::fputs("usage: pulp-fixture-runner --corpus <dir> [--update] [--verbose]\n"
               "  --corpus <dir>  corpus root containing corpus.index\n"
               "  --update        regenerate each fixture's .expect manifest\n"
               "  --verbose       print every fixture checked, not only failures\n",
               stdout);
}

} // namespace

int main(int argc, char** argv) {
    std::string corpus_root;
    bool update = false;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--corpus") == 0 && i + 1 < argc) {
            corpus_root = argv[++i];
        } else if (std::strcmp(argv[i], "--update") == 0) {
            update = true;
        } else if (std::strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            print_usage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (corpus_root.empty()) {
        print_usage();
        return 2;
    }

    auto registry = pulp::timeline::make_builtin_timeline_registry();
    if (!registry) {
        std::fprintf(stderr, "failed to build built-in timeline registry (schema error %u for %s)\n",
                     static_cast<unsigned>(registry.error().code),
                     registry.error().type_name.c_str());
        return 1;
    }

    const std::string index_path = corpus_root + "/corpus.index";
    bool index_ok = false;
    auto index_text = read_file(index_path, index_ok);
    if (!index_ok) {
        std::fprintf(stderr, "cannot read corpus index %s\n", index_path.c_str());
        return 1;
    }

    // The index states each entry's kind, because the corpus holds project
    // documents, single-entity fragments, and raw opaque payloads, and nothing
    // in the files distinguishes them. Decoding a fragment or a payload as a
    // project fails with InvalidSchema, which is correct refusal rather than a
    // corpus defect — so the kind has to be declared, not guessed.
    std::vector<std::string> fixtures;
    std::vector<std::string> skipped;
    for (const auto& entry : parse_key_value_lines(index_text)) {
        if (entry.value.empty()) {
            std::fprintf(stderr, "corpus index entry missing a kind: %s\n", entry.key.c_str());
            return 1;
        }
        if (entry.key == "document") {
            fixtures.push_back(entry.value);
        } else if (entry.key == "fragment" || entry.key == "payload" || entry.key == "ignore") {
            // Still open the file. Reporting an entry as skipped while never
            // touching it means renaming or deleting it degrades the index into
            // a comment and the run stays green — the precise blindness this
            // index exists to prevent.
            bool present = false;
            read_file(corpus_root + "/" + entry.value, present);
            if (!present) {
                std::fprintf(stderr, "corpus index lists a missing %s: %s\n", entry.key.c_str(),
                             entry.value.c_str());
                return 1;
            }
            skipped.push_back(entry.key + " " + entry.value);
        } else {
            std::fprintf(stderr, "corpus index entry has unknown kind '%s': %s\n",
                         entry.key.c_str(), entry.value.c_str());
            return 1;
        }
    }

    if (fixtures.empty()) {
        std::fprintf(stderr, "corpus index %s lists no documents\n", index_path.c_str());
        return 1;
    }

    std::vector<Failure> failures;
    for (const auto& relative_path : fixtures) {
        auto outcome = check_fixture(corpus_root, relative_path, registry.value(), update);
        if (update && outcome.ok) {
            const std::string expect_path = corpus_root + "/" + relative_path + ".expect";
            if (!write_manifest(expect_path, outcome.observed)) {
                failures.push_back({relative_path, "cannot write " + expect_path});
                continue;
            }
            std::fprintf(stdout, "updated %s.expect\n", relative_path.c_str());
            continue;
        }
        if (outcome.ok) {
            if (verbose)
                std::fprintf(stdout, "ok %s\n", relative_path.c_str());
            continue;
        }
        for (const auto& mismatch : outcome.mismatches)
            failures.push_back({relative_path, mismatch});
    }

    if (!failures.empty()) {
        for (const auto& failure : failures)
            std::fprintf(stderr, "FAIL %s: %s\n", failure.fixture.c_str(),
                         failure.detail.c_str());
        std::fprintf(stderr, "pulp-fixture-runner: %zu failure(s) across %zu fixture(s)\n",
                     failures.size(), fixtures.size());
        return 1;
    }

    // Report what was NOT validated. A corpus runner that prints only its
    // successes reads as full coverage when it is partial.
    for (const auto& entry : skipped)
        std::fprintf(stdout, "skip %s (no entity-level validation yet)\n", entry.c_str());
    std::fprintf(stdout, "pulp-fixture-runner: %zu document(s) verified, %zu entry(s) skipped\n",
                 fixtures.size(), skipped.size());
    return 0;
}
