// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/child_process.hpp>
#include <pulp/platform/progress_parser.hpp>

#include <string>
#include <vector>

using namespace pulp::platform;

TEST_CASE("ProgressParser parses basic event", "[progress_parser]") {
    std::vector<ProgressEvent> events;
    ProgressParser parser([&](const ProgressEvent& e) { events.push_back(e); });

    parser.feed_line("PROGRESS:DOWNLOAD_START:0:ambient sounds");
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].type == "DOWNLOAD_START");
    REQUIRE(events[0].payload == "0:ambient sounds");
}

TEST_CASE("ProgressParser ignores non-progress lines", "[progress_parser]") {
    std::vector<ProgressEvent> events;
    ProgressParser parser([&](const ProgressEvent& e) { events.push_back(e); });

    parser.feed_line("Some regular output");
    parser.feed_line("");
    parser.feed_line("Downloading file...");
    REQUIRE(events.empty());
}

TEST_CASE("ProgressParser handles type-only (no payload)", "[progress_parser]") {
    std::vector<ProgressEvent> events;
    ProgressParser parser([&](const ProgressEvent& e) { events.push_back(e); });

    parser.feed_line("PROGRESS:DONE");
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].type == "DONE");
    REQUIRE(events[0].payload.empty());
}

TEST_CASE("ProgressParser handles empty callbacks for matching lines",
          "[progress_parser][edge][issue-640]") {
    ProgressParser parser(nullptr);

    REQUIRE_NOTHROW(parser.feed_line("PROGRESS:START:payload"));
    REQUIRE_NOTHROW(parser.feed_line("PROGRESS:DONE"));
    REQUIRE_NOTHROW(parser.feed_line("regular output"));
}

TEST_CASE("ProgressParser preserves empty type and payload boundaries",
          "[progress_parser][edge][issue-640]") {
    std::vector<ProgressEvent> events;
    ProgressParser parser([&](const ProgressEvent& e) { events.push_back(e); });

    parser.feed_line("PROGRESS::payload");
    parser.feed_line("PROGRESS:EMPTY_PAYLOAD:");

    REQUIRE(events.size() == 2);
    REQUIRE(events[0].type.empty());
    REQUIRE(events[0].payload == "payload");
    REQUIRE(events[1].type == "EMPTY_PAYLOAD");
    REQUIRE(events[1].payload.empty());
}

TEST_CASE("ProgressParser preserves payload delimiters after the first colon",
          "[progress_parser][edge][issue-640]") {
    std::vector<ProgressEvent> events;
    ProgressParser parser([&](const ProgressEvent& e) { events.push_back(e); });

    parser.feed_line("PROGRESS:ERROR:5:Network timeout: retry queued");
    parser.feed_line("PROGRESS:PATH:/tmp/audio:cache/sample.wav:");

    REQUIRE(events.size() == 2);
    REQUIRE(events[0].type == "ERROR");
    REQUIRE(events[0].payload == "5:Network timeout: retry queued");
    REQUIRE(events[1].type == "PATH");
    REQUIRE(events[1].payload == "/tmp/audio:cache/sample.wav:");
}

TEST_CASE("ProgressParser emits repeated empty type-only events deterministically",
          "[progress_parser][edge][issue-640]") {
    std::vector<ProgressEvent> events;
    ProgressParser parser([&](const ProgressEvent& e) { events.push_back(e); });

    parser.feed_line("PROGRESS:");
    parser.feed_line("PROGRESS:");

    REQUIRE(events.size() == 2);
    REQUIRE(events[0].type.empty());
    REQUIRE(events[0].payload.empty());
    REQUIRE(events[1].type.empty());
    REQUIRE(events[1].payload.empty());
}

TEST_CASE("ProgressParser handles multiple events", "[progress_parser]") {
    std::vector<ProgressEvent> events;
    ProgressParser parser([&](const ProgressEvent& e) { events.push_back(e); });

    parser.feed_line("PROGRESS:OVERALL:3:16");
    parser.feed_line("Some log output");
    parser.feed_line("PROGRESS:SAMPLE_COMPLETE:2:/tmp/sample.wav");
    parser.feed_line("PROGRESS:ERROR:5:Network timeout");

    REQUIRE(events.size() == 3);
    REQUIRE(events[0].type == "OVERALL");
    REQUIRE(events[0].payload == "3:16");
    REQUIRE(events[1].type == "SAMPLE_COMPLETE");
    REQUIRE(events[1].payload == "2:/tmp/sample.wav");
    REQUIRE(events[2].type == "ERROR");
    REQUIRE(events[2].payload == "5:Network timeout");
}

TEST_CASE("ProgressParser rejects malformed prefix variants", "[progress_parser][edge]") {
    // Prefixes that look like PROGRESS but aren't: must not emit events.
    std::vector<ProgressEvent> events;
    ProgressParser parser([&](const ProgressEvent& e) { events.push_back(e); });

    parser.feed_line("PROGRESSsomething");      // no colon separator
    parser.feed_line("progress:type");          // lowercase, not the sentinel
    parser.feed_line(" PROGRESS:type");         // leading whitespace
    parser.feed_line("PROGRES:type");           // typo
    REQUIRE(events.empty());
}

TEST_CASE("ProgressParser handles lone PROGRESS: prefix with empty type", "[progress_parser][edge]") {
    // The implementation parses PROGRESS:<type>[:<payload>]. "PROGRESS:"
    // alone means a zero-length type — either drop or emit with "".
    // Accept both; just make sure we don't crash or spray events.
    std::vector<ProgressEvent> events;
    ProgressParser parser([&](const ProgressEvent& e) { events.push_back(e); });

    parser.feed_line("PROGRESS:");
    REQUIRE(events.size() <= 1);
    if (events.size() == 1) {
        REQUIRE(events[0].type.empty());
        REQUIRE(events[0].payload.empty());
    }
}

TEST_CASE("ProgressParser drops whitespace-only and empty-string inputs", "[progress_parser][edge]") {
    std::vector<ProgressEvent> events;
    ProgressParser parser([&](const ProgressEvent& e) { events.push_back(e); });

    parser.feed_line("");
    parser.feed_line(" ");
    parser.feed_line("\t");
    parser.feed_line("   \t   ");
    REQUIRE(events.empty());
}

TEST_CASE("ProgressParser works with ChildProcess line callback", "[progress_parser]") {
    std::vector<ProgressEvent> events;
    ProgressParser parser([&](const ProgressEvent& e) { events.push_back(e); });

    ProcessOptions opts;
    opts.timeout_ms = 5000;
    opts.on_stdout_line = [&](std::string_view line) { parser.feed_line(line); };

#ifdef _WIN32
    auto r = ChildProcess::run("cmd", {"/c",
        "echo PROGRESS:START:test& echo normal output& echo PROGRESS:END"}, opts);
#else
    auto r = ChildProcess::run("/bin/sh", {"-c",
        "echo PROGRESS:START:test; echo normal output; echo PROGRESS:END"}, opts);
#endif

    REQUIRE(r.exit_code == 0);
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].type == "START");
    REQUIRE(events[0].payload == "test");
    REQUIRE(events[1].type == "END");
}
