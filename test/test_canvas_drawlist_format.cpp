// Drawlist formatting — a RecordingCanvas drawlist as readable, diffable text.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include <pulp/canvas/drawlist_format.hpp>
#include <pulp/canvas/recording_canvas.hpp>

using pulp::canvas::Color;
using pulp::canvas::draw_command_name;
using pulp::canvas::DrawCommand;
using pulp::canvas::format_command;
using pulp::canvas::format_commands;
using pulp::canvas::RecordingCanvas;

namespace {

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("a command formats as its name and its whole payload", "[canvas][drawlist-format]") {
    DrawCommand c;
    c.type = DrawCommand::Type::fill_rect;
    c.f[0] = 10.0f;
    c.f[1] = 20.0f;
    c.f[2] = 100.0f;
    c.f[3] = 50.0f;

    const std::string line = format_command(c);
    CHECK(contains(line, "fill_rect"));
    CHECK(contains(line, "10.000 20.000 100.000 50.000"));
}

TEST_CASE("every float slot is printed, including the zeros", "[canvas][drawlist-format]") {
    // Printing only the slots a command "uses" would need a per-command arity
    // table, and a wrong entry there would HIDE a parameter rather than fail.
    DrawCommand c;
    c.type = DrawCommand::Type::save;
    const std::string line = format_command(c);
    int zeros = 0;
    for (size_t i = line.find("0.000"); i != std::string::npos; i = line.find("0.000", i + 1)) {
        ++zeros;
    }
    CHECK(zeros >= 6);
}

TEST_CASE("colour prints as floats so an HDR channel is not truncated",
          "[canvas][drawlist-format]") {
    DrawCommand c;
    c.type = DrawCommand::Type::set_fill_color;
    c.color = Color::rgba(2.5f, 0.0f, 0.0f, 1.0f); // above 1.0 on purpose

    const std::string line = format_command(c);
    // Hex would have clamped or wrapped this into a plausible wrong colour.
    CHECK(contains(line, "2.500"));
    CHECK_FALSE(contains(line, "#"));
}

TEST_CASE("opaque black is printed rather than treated as absent", "[canvas][drawlist-format]") {
    // {0,0,0,1} is the struct default AND a colour a command can legitimately
    // carry, so skipping it as "unset" would hide a real value.
    DrawCommand c;
    c.type = DrawCommand::Type::set_fill_color;
    c.color = Color::rgba(0.0f, 0.0f, 0.0f, 1.0f);
    CHECK(contains(format_command(c), "rgba(0.000 0.000 0.000 1.000)"));
}

TEST_CASE("text is quoted and escaped so it cannot forge a second command",
          "[canvas][drawlist-format]") {
    DrawCommand c;
    c.type = DrawCommand::Type::fill_text;
    c.text = "he said \"hi\"\nfill_rect 0 0 0 0";

    const std::string line = format_command(c);
    // The embedded newline must not split the line; a golden reader counts
    // lines, so an un-escaped newline would invent a command.
    CHECK(line.find('\n') == std::string::npos);
    CHECK(contains(line, "\\\""));
    CHECK(contains(line, "\\n"));
}

TEST_CASE("the variable-length payload is reported by size", "[canvas][drawlist-format]") {
    DrawCommand c;
    c.type = DrawCommand::Type::set_line_dash;
    c.floats = {4.0f, 2.0f, 4.0f};
    CHECK(contains(format_command(c), "+3 floats"));
}

TEST_CASE("a drawlist formats one command per line", "[canvas][drawlist-format]") {
    RecordingCanvas rc;
    rc.set_fill_color(Color::rgba(1.0f, 0.0f, 0.0f, 1.0f));
    rc.fill_rect(0.0f, 0.0f, 10.0f, 10.0f);

    const std::string text = format_commands(rc.commands());
    REQUIRE_FALSE(text.empty());
    CHECK(text.back() == '\n');

    size_t lines = 0;
    for (char ch : text) {
        if (ch == '\n')
            ++lines;
    }
    CHECK(lines == rc.commands().size());
}

TEST_CASE("an empty drawlist formats as nothing", "[canvas][drawlist-format]") {
    RecordingCanvas rc;
    CHECK(format_commands(rc.commands()).empty());

    // Control: the same formatter is not simply returning empty -- one
    // command produces output.
    rc.fill_rect(0.0f, 0.0f, 1.0f, 1.0f);
    CHECK_FALSE(format_commands(rc.commands()).empty());
}

TEST_CASE("the format sees position, which a histogram comparison cannot",
          "[canvas][drawlist-format]") {
    // The documented blind spot of the pixel scorer: "a design with every
    // element in the wrong place scores identically to a correct one -- same
    // pixels, different arrangement." Same ops, different coordinates.
    RecordingCanvas correct;
    correct.fill_rect(0.0f, 0.0f, 10.0f, 10.0f);
    correct.fill_rect(50.0f, 50.0f, 10.0f, 10.0f);

    RecordingCanvas swapped;
    swapped.fill_rect(50.0f, 50.0f, 10.0f, 10.0f);
    swapped.fill_rect(0.0f, 0.0f, 10.0f, 10.0f);

    CHECK(format_commands(correct.commands()) != format_commands(swapped.commands()));
}

TEST_CASE("the format sees material, which a block-mean comparison cannot",
          "[canvas][drawlist-format]") {
    // The other documented blind spot: a flattened gradient "matches its own
    // mean exactly". Two fills whose mean colour is identical.
    RecordingCanvas bright;
    bright.set_fill_color(Color::rgba(1.0f, 0.0f, 0.0f, 1.0f));
    bright.fill_rect(0.0f, 0.0f, 10.0f, 10.0f);

    RecordingCanvas dim;
    dim.set_fill_color(Color::rgba(0.5f, 0.0f, 0.0f, 1.0f));
    dim.fill_rect(0.0f, 0.0f, 10.0f, 10.0f);

    CHECK(format_commands(bright.commands()) != format_commands(dim.commands()));
}

TEST_CASE("every command type has a distinct name", "[canvas][drawlist-format]") {
    // The switch behind draw_command_name has no default, so a new Type fails
    // to compile rather than formatting as something unhelpful. This pins the
    // other half: no two types may share a name, or a golden could not tell
    // them apart.
    std::vector<std::string> seen;
    for (int i = 0; i <= static_cast<int>(DrawCommand::Type::draw_sksl); ++i) {
        const auto name = std::string(draw_command_name(static_cast<DrawCommand::Type>(i)));
        CHECK_FALSE(name.empty());
        CHECK(name != "unknown");
        for (const auto& previous : seen) {
            CHECK(previous != name);
        }
        seen.push_back(name);
    }
    CHECK(seen.size() >= 80);
}
