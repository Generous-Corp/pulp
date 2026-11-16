// The CLI `graph --json` code for a graph or notation file is the C++
// GraphPatchDiagnostic code name for the same file. Each row feeds one input to
// the C++ loader and to the shipped CLI and requires the same spelling, so a
// renamed or rewritten code in either place fails here.

#include "test_cli_shellout_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <choc/text/choc_JSON.h>
#include <pulp/host/graph_patch.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/signal_graph_runtime.hpp>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
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
        {"midi port alias", "midi-alias.pat",
         "# flow 1\nkeys : pulp/midi_in\nsink : pulp/midi_out\nkeys.midi >> sink.midi\n", Code::Ok},
        {"midi alias without a midi port", "midi-alias-refused.pat",
         "# flow 1\ninput : pulp/audio_in\nout : pulp/audio_out\ninput.midi >> out.audio\n",
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

    // fmt on a saved document prints the same typed result.
    const auto saved = dir / "ok.pulpgraph";
    pulp::host::SignalGraph graph;
    const auto in = graph.add_input_node(1, "Input");
    const auto gain = graph.add_gain_node("Gain");
    const auto out = graph.add_output_node(1, "Output");
    REQUIRE(graph.connect(in, 0, gain, 0));
    REQUIRE(graph.connect(gain, 0, out, 0));
    std::ofstream(saved, std::ios::binary) << pulp::host::GraphSerializer::to_saved_document(graph);
    const auto fmt_doc = run_pulp({"graph", "fmt", saved.string(), "--json"});
    REQUIRE(fmt_doc.exit_code == 0);
    REQUIRE(cli_code(fmt_doc) == ok);
}

TEST_CASE("graph CLI file size and readability codes agree across subcommands",
          "[cli][shellout][graph][parity]") {
    if (!fs::exists(pulp_binary()))
        SKIP("pulp not built");
    const auto dir = unique_temp_dir("pulp-graph-code-parity-files");
    fs::create_directories(dir);

    // One byte over the serializer limit, sparse so the test writes nothing.
    const auto oversize = dir / "oversize.pulpgraph";
    std::ofstream(oversize, std::ios::binary).close();
    fs::resize_file(oversize, pulp::host::GraphSerializer::max_file_bytes + 1);
    REQUIRE(pulp::host::load_graph_patch_file(oversize).diagnostic.code == Code::ResourceLimit);
    const std::string limit = pulp::host::graph_patch_code_name(Code::ResourceLimit);
    for (const std::string op : {"validate", "inspect", "report"}) {
        const auto cli = run_pulp({"graph", op, oversize.string(), "--json"});
        INFO(op << ": " << cli.stdout_output << cli.stderr_output);
        REQUIRE(cli.exit_code == 1);
        REQUIRE(cli_code(cli) == limit);
    }

    // A path that exists but is not a readable regular file.
    const auto directory = dir / "directory.pulpgraph";
    fs::create_directories(directory);
    REQUIRE(pulp::host::load_graph_patch_file(directory).diagnostic.code == Code::FileUnreadable);
    const std::string unreadable = pulp::host::graph_patch_code_name(Code::FileUnreadable);
    for (const std::string op : {"validate", "inspect"}) {
        const auto cli = run_pulp({"graph", op, directory.string(), "--json"});
        INFO(op << ": " << cli.stdout_output << cli.stderr_output);
        REQUIRE(cli.exit_code == 1);
        REQUIRE(cli_code(cli) == unreadable);
    }

    // The probe names a missing file the same way, keeping graph_file_limit
    // for its own 1 MiB size bound.
    const auto probe = [&](const fs::path& path) {
        return run_pulp({"graph", "probe", path.string(), "--json", "--input",
                         (dir / "in.wav").string(), "--source-node", "source", "--source-port",
                         "audio", "--destination-node", "out", "--destination-port", "audio"});
    };
    const auto missing = probe(dir / "absent.pulpgraph");
    INFO(missing.stdout_output << missing.stderr_output);
    REQUIRE(missing.exit_code == 2);
    REQUIRE(cli_code(missing) == unreadable);
    const auto large = dir / "large.pulpgraph";
    std::ofstream(large, std::ios::binary).close();
    fs::resize_file(large, 1024 * 1024 + 1);
    const auto too_large = probe(large);
    INFO(too_large.stdout_output << too_large.stderr_output);
    REQUIRE(too_large.exit_code == 2);
    REQUIRE(cli_code(too_large) == "graph_file_limit");
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

    // Only the graph section counts: a generic code such as `saved` must not
    // pass because another command documents the same word.
    const auto section_begin = doc.find("\n### graph\n");
    REQUIRE(section_begin != std::string::npos);
    const auto section_end = doc.find("\n### ", section_begin + 1);
    REQUIRE(section_end != std::string::npos);
    const auto graph_doc = doc.substr(section_begin, section_end - section_begin);
    REQUIRE(graph_doc.find("`missing_option`") != std::string::npos);

    std::set<std::string> emitted;
    const std::regex pattern(R"re((?:fail|json_error)\("([A-Za-z_]+)"|code = "([A-Za-z_]+)")re");
    for (std::sregex_iterator it(source.begin(), source.end(), pattern), end; it != end; ++it)
        emitted.insert((*it)[1].matched ? (*it)[1].str() : (*it)[2].str());

    // Control: the scan must see codes this file is known to emit.
    CHECK(emitted.size() >= 20);
    CHECK(emitted.count("prepare_failed") == 1);
    CHECK(emitted.count("unprepared_latency") == 1);
    CHECK(emitted.count("RestoreFailed") == 1);

    // Probe refusal codes are returned from probe_refusal() as string pairs, not
    // through a literal fail("...") call, so scan them separately.
    const auto refusal_begin = source.find("probe_refusal(");
    const auto refusal_end = source.find("\n}\n", refusal_begin);
    REQUIRE(refusal_begin != std::string::npos);
    REQUIRE(refusal_end != std::string::npos);
    const auto refusals = source.substr(refusal_begin, refusal_end - refusal_begin);
    const std::regex refusal_pattern(R"re(return \{"([A-Za-z]+)",)re");
    std::set<std::string> refusal_codes;
    for (std::sregex_iterator it(refusals.begin(), refusals.end(), refusal_pattern), end; it != end;
         ++it)
        refusal_codes.insert((*it)[1].str());
    CHECK(refusal_codes.count("Busy") == 1);
    CHECK(refusal_codes.size() >= 6);
    emitted.insert(refusal_codes.begin(), refusal_codes.end());

    for (const auto& code : emitted) {
        INFO(code);
        CHECK(graph_doc.find("`" + code + "`") != std::string::npos);
    }
}

TEST_CASE("graph diff compares saved documents by logical identity",
          "[cli][shellout][graph][parity]") {
    if (!fs::exists(pulp_binary()))
        SKIP("pulp not built");
    const auto dir = unique_temp_dir("pulp-graph-diff");
    fs::create_directories(dir);

    pulp::host::SignalGraph graph;
    const auto input = graph.add_input_node(1, "Input");
    const auto gain = graph.add_gain_node("Gain");
    const auto output = graph.add_output_node(1, "Output");
    REQUIRE(graph.connect(input, 0, gain, 0));
    REQUIRE(graph.connect(gain, 0, output, 0));
    const auto before = dir / "before.pulpgraph";
    std::ofstream(before, std::ios::binary)
        << pulp::host::GraphSerializer::to_saved_document(graph);
    const auto legacy = dir / "legacy.pulpgraph";
    std::ofstream(legacy, std::ios::binary)
        << R"({"format_version":5,"nodes":[],"connections":[]})";

    REQUIRE(graph.set_node_gain(gain, 0.5f));
    const auto trim = graph.add_gain_node("Trim");
    REQUIRE(graph.connect(gain, 0, trim, 0));
    const auto after = dir / "after.pulpgraph";
    std::ofstream(after, std::ios::binary) << pulp::host::GraphSerializer::to_saved_document(graph);

    const auto changed = run_pulp({"graph", "diff", before.string(), after.string(), "--json"});
    INFO(changed.stdout_output << changed.stderr_output);
    REQUIRE(changed.exit_code == 0);
    const auto report = choc::json::parse(changed.stdout_output);
    CHECK(std::string(report["code"].getString()) == "Ok");
    CHECK_FALSE(report["equal"].getBool());
    REQUIRE(report["nodes"]["added"].size() == 1);
    CHECK(std::string(report["nodes"]["added"][0].getString()) == "node:gain:Trim:0");
    REQUIRE(report["nodes"]["changed"].size() == 1);
    CHECK(std::string(report["nodes"]["changed"][0]["fields"][0].getString()) == "gain");
    REQUIRE(report["connections"]["added"].size() == 1);

    const auto same = run_pulp({"graph", "diff", before.string(), before.string(), "--json"});
    REQUIRE(same.exit_code == 0);
    CHECK(choc::json::parse(same.stdout_output)["equal"].getBool());

    const auto text = run_pulp({"graph", "diff", before.string(), after.string()});
    REQUIRE(text.exit_code == 0);
    CHECK(text.stdout_output.find("+ node node:gain:Trim:0") != std::string::npos);

    // A document without the identity envelope has no authored identity to
    // compare by.
    const auto refused = run_pulp({"graph", "diff", legacy.string(), after.string(), "--json"});
    REQUIRE(refused.exit_code == 1);
    const auto refusal = choc::json::parse(refused.stdout_output);
    CHECK(std::string(refusal["code"].getString()) ==
          pulp::host::graph_patch_code_name(Code::InvalidDocument));
    CHECK(std::string(refusal["refusal"].getString()) == "logical_identity_missing");
    CHECK(std::string(refusal["side"].getString()) == "before");

    const auto missing =
        run_pulp({"graph", "diff", before.string(), (dir / "absent.pulpgraph").string(), "--json"});
    REQUIRE(missing.exit_code == 1);
    CHECK(cli_code(missing) == pulp::host::graph_patch_code_name(Code::FileUnreadable));

    const auto one = run_pulp({"graph", "diff", before.string(), "--json"});
    REQUIRE(one.exit_code == 2);
    CHECK(cli_code(one) == "missing_input");
}
