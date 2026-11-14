// The CLI `graph --json` code for a graph or notation file is the C++
// GraphPatchDiagnostic code name for the same file. Each row feeds one input to
// the C++ loader and to the shipped CLI and requires the same spelling, so a
// renamed or rewritten code in either place fails here.

#include "test_cli_shellout_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <choc/text/choc_JSON.h>
#include <pulp/host/graph_patch.hpp>

#include <filesystem>
#include <regex>
#include <set>
#include <sstream>
#include <fstream>
#include <string>
#include <vector>

using namespace pulp_test_cli;
using Code = pulp::host::GraphPatchDiagnostic::Code;

namespace {

struct Row {
    const char* name;
    const char* file; // file name, extension selects notation vs saved graph
    const char* text; // nullptr: the file is not created
    Code expected;
};

constexpr const char* kGoodPat = "# flow 1\n"
                                 "input : pulp/audio_in\n"
                                 "out : pulp/audio_out\n"
                                 "input.audio >> out.audio\n";

const std::vector<Row>& rows() {
    static const std::vector<Row> table{
        {"valid notation", "ok.pat", kGoodPat, Code::Ok},
        {"malformed notation", "syntax.pat", "this is not notation\n", Code::SyntaxError},
        {"unknown port", "port.pat",
         "# flow 1\ninput : pulp/audio_in\nout : pulp/audio_out\ninput.typo >> out.audio\n",
         Code::UnknownPort},
        {"unsupported contract", "contract.pat",
         "# flow 1\ninput : pulp/audio_in\nx : pulp/does_not_exist\ninput.audio >> x.audio\n",
         Code::UnsupportedContract},
        {"bad json", "bad.pulpgraph", "{broken", Code::InvalidDocument},
        {"invalid document", "doc.pulpgraph", R"({"format_version":1,"nodes":5})",
         Code::InvalidDocument},
        {"missing file", "missing.pulpgraph", nullptr, Code::FileUnreadable},
        {"missing notation file", "missing.pat", nullptr, Code::FileUnreadable},
    };
    return table;
}

std::string cli_code(const ProcessResult& result) {
    return std::string(choc::json::parse(result.stdout_output)["code"].getString());
}

} // namespace

TEST_CASE("graph CLI --json codes equal the C++ diagnostic code names",
          "[cli][shellout][graph][parity]") {
    if (!fs::exists(pulp_binary()))
        SKIP("pulp not built");
    const auto dir = unique_temp_dir("pulp-graph-code-parity");
    fs::create_directories(dir);
    for (const auto& row : rows()) {
        DYNAMIC_SECTION(row.name) {
            const auto path = dir / row.file;
            if (row.text) {
                std::ofstream(path, std::ios::binary) << row.text;
            }
            const auto cpp = pulp::host::load_graph_patch_file(path);
            REQUIRE(cpp.diagnostic.code == row.expected);
            const std::string name = pulp::host::graph_patch_code_name(cpp.diagnostic.code);

            for (const std::string op : {"validate", "inspect"}) {
                const auto cli = run_pulp({"graph", op, path.string(), "--json"});
                INFO(op << ": " << cli.stdout_output << cli.stderr_output);
                // Notation files are strict for every operation. Saved graphs
                // are strict for validate only; inspect uses the tolerant
                // loader, which accepts documents the strict loader refuses
                // for contract reasons, so only the shared failure classes
                // below are compared there.
                const bool notation = path.extension() == ".pat";
                if (op == "inspect" && !notation && row.expected != Code::InvalidDocument &&
                    row.expected != Code::FileUnreadable)
                    continue;
                if (row.expected == Code::Ok) {
                    REQUIRE(cli.exit_code == 0);
                    if (op == "validate")
                        REQUIRE(cli_code(cli) == name);
                    continue;
                }
                REQUIRE(cli.exit_code == 1);
                REQUIRE(cli_code(cli) == name);
            }
        }
    }
}

TEST_CASE("graph CLI success code and fmt use the C++ spelling", "[cli][shellout][graph][parity]") {
    if (!fs::exists(pulp_binary()))
        SKIP("pulp not built");
    const auto dir = unique_temp_dir("pulp-graph-code-parity-ok");
    fs::create_directories(dir);
    const auto path = dir / "ok.pat";
    std::ofstream(path, std::ios::binary) << kGoodPat;
    const std::string ok = pulp::host::graph_patch_code_name(Code::Ok);
    REQUIRE(ok == "Ok");
    const auto validate = run_pulp({"graph", "validate", path.string(), "--json"});
    REQUIRE(validate.exit_code == 0);
    REQUIRE(cli_code(validate) == ok);
    const auto fmt = run_pulp({"graph", "fmt", path.string(), "--json"});
    REQUIRE(fmt.exit_code == 0);
    REQUIRE(cli_code(fmt) == ok);
}

namespace {
std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}
} // namespace

TEST_CASE("every CLI graph code the source emits is documented", "[cli][graph][docs]") {
    const std::filesystem::path root = PULP_CLI_SOURCE_ROOT;
    const auto source = read_text(root / "tools/cli/cmd_graph.cpp");
    const auto doc = read_text(root / "docs/reference/cli.md");
    REQUIRE_FALSE(source.empty());
    REQUIRE_FALSE(doc.empty());

    std::set<std::string> emitted;
    const std::regex pattern(R"re((?:fail|json_error)\("([a-z_]+)"|code = "([a-z_]+)")re");
    for (std::sregex_iterator it(source.begin(), source.end(), pattern), end; it != end; ++it)
        emitted.insert((*it)[1].matched ? (*it)[1].str() : (*it)[2].str());

    // Control: the scan must see codes this file is known to emit.
    CHECK(emitted.size() >= 20);
    CHECK(emitted.count("prepare_failed") == 1);
    CHECK(emitted.count("unprepared_latency") == 1);

    for (const auto& code : emitted) {
        INFO(code);
        CHECK(doc.find("`" + code + "`") != std::string::npos);
    }
}
