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
    // A file of exactly the bound is read and judged as a document instead.
    fs::resize_file(large, 1024 * 1024);
    const auto at_limit = probe(large);
    INFO(at_limit.stdout_output << at_limit.stderr_output);
    REQUIRE(at_limit.exit_code == 2);
    REQUIRE(cli_code(at_limit) != "graph_file_limit");
    REQUIRE(cli_code(at_limit) != unreadable);
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

TEST_CASE("graph save --saved-identity refuses a plain document that loses an edge",
          "[cli][shellout][graph][parity]") {
    if (!fs::exists(pulp_binary()))
        SKIP("pulp not built");
    const auto dir = unique_temp_dir("pulp-graph-lossy-envelope");
    fs::create_directories(dir);

    pulp::host::SignalGraph graph;
    const auto input = graph.add_input_node(1, "Input");
    const auto gain = graph.add_gain_node("Gain");
    const auto output = graph.add_output_node(1, "Output");
    REQUIRE(graph.connect(input, 0, gain, 0));
    REQUIRE(graph.connect(gain, 0, output, 0));
    const auto document = pulp::host::GraphSerializer::to_json(graph);
    const auto good = dir / "good.pulpgraph";
    std::ofstream(good, std::ios::binary) << document;

    // The tolerant loader skips an edge whose destination does not exist.
    auto lossy_text = document;
    const auto connections = lossy_text.find("\"connections\"");
    REQUIRE(connections != std::string::npos);
    const auto dest = lossy_text.find("\"dest_node\": ", connections);
    REQUIRE(dest != std::string::npos);
    const auto value_end = lossy_text.find_first_of(",\n}", dest + 13);
    lossy_text.replace(dest, value_end - dest, "\"dest_node\": 99");
    const auto lossy = dir / "lossy.pulpgraph";
    std::ofstream(lossy, std::ios::binary) << lossy_text;

    // Control: the intact plain document gains the envelope.
    const auto stamped = dir / "stamped.pulpgraph";
    const auto ok = run_pulp({"graph", "save", good.string(), "--output", stamped.string(),
                              "--saved-identity", "--json"});
    INFO(ok.stdout_output << ok.stderr_output);
    REQUIRE(ok.exit_code == 0);
    CHECK(read_text(stamped).find("identity_schema") != std::string::npos);

    // A plain save keeps its partial-load behaviour.
    const auto plain_out = dir / "plain-out.pulpgraph";
    REQUIRE(run_pulp({"graph", "save", lossy.string(), "--output", plain_out.string()}).exit_code ==
            0);

    const auto refused_out = dir / "refused.pulpgraph";
    const auto refused = run_pulp({"graph", "save", lossy.string(), "--output",
                                   refused_out.string(), "--saved-identity", "--json"});
    INFO(refused.stdout_output << refused.stderr_output);
    REQUIRE(refused.exit_code == 1);
    CHECK(cli_code(refused) == pulp::host::graph_patch_code_name(Code::InvalidDocument));
    CHECK(refused.stdout_output.find("connection_not_restored") != std::string::npos);
    CHECK_FALSE(fs::exists(refused_out));
}

TEST_CASE("graph save and fmt keep the saved-identity envelope that diff reads",
          "[cli][shellout][graph][parity]") {
    if (!fs::exists(pulp_binary()))
        SKIP("pulp not built");
    const auto dir = unique_temp_dir("pulp-graph-envelope");
    fs::create_directories(dir);

    pulp::host::SignalGraph graph;
    const auto input = graph.add_input_node(1, "Input");
    const auto gain = graph.add_gain_node("Gain");
    const auto output = graph.add_output_node(1, "Output");
    REQUIRE(graph.connect(input, 0, gain, 0));
    REQUIRE(graph.connect(gain, 0, output, 0));
    const auto plain = dir / "plain.pulpgraph";
    std::ofstream(plain, std::ios::binary) << pulp::host::GraphSerializer::to_json(graph);
    const auto envelope = dir / "envelope.pulpgraph";
    std::ofstream(envelope, std::ios::binary)
        << pulp::host::GraphSerializer::to_saved_document(graph);

    // Two files written only by the CLI can be diffed.
    const auto first = dir / "first.pulpgraph";
    const auto from_plain = run_pulp({"graph", "save", plain.string(), "--output", first.string(),
                                      "--saved-identity", "--json"});
    INFO(from_plain.stdout_output << from_plain.stderr_output);
    REQUIRE(from_plain.exit_code == 0);
    CHECK(read_text(first).find("identity_schema") != std::string::npos);
    const auto second = dir / "second.pulpgraph";
    REQUIRE(run_pulp({"graph", "save", first.string(), "--output", second.string()}).exit_code ==
            0);
    const auto cli_diff = run_pulp({"graph", "diff", first.string(), second.string(), "--json"});
    INFO(cli_diff.stdout_output << cli_diff.stderr_output);
    REQUIRE(cli_diff.exit_code == 0);
    CHECK(choc::json::parse(cli_diff.stdout_output)["equal"].getBool());

    // Control: a plain save of a plain file stays plain, so diff refuses it
    // and names the way to write the envelope.
    const auto still_plain = dir / "still-plain.pulpgraph";
    REQUIRE(
        run_pulp({"graph", "save", plain.string(), "--output", still_plain.string()}).exit_code ==
        0);
    CHECK(read_text(still_plain).find("identity_schema") == std::string::npos);
    const auto refused =
        run_pulp({"graph", "diff", still_plain.string(), first.string(), "--json"});
    REQUIRE(refused.exit_code == 1);
    const auto refusal = choc::json::parse(refused.stdout_output);
    // A simple plain graph keeps an older format, which the inspector
    // refuses by version rather than by the missing envelope.
    const std::string reason(refusal["refusal"].getString());
    CHECK((reason == "logical_identity_missing" || reason == "invalid_format_version"));
    REQUIRE(refusal.hasObjectMember("hint"));
    CHECK(std::string(refusal["hint"].getString()).find("--saved-identity") != std::string::npos);

    // fmt and canonicalize rewrite an envelope document in place and keep it.
    for (const std::string op : {"fmt", "canonicalize"}) {
        INFO(op);
        const auto copy = dir / (op + ".pulpgraph");
        fs::copy_file(envelope, copy, fs::copy_options::overwrite_existing);
        const auto rewritten = run_pulp({"graph", op, copy.string(), "--json"});
        INFO(rewritten.stdout_output << rewritten.stderr_output);
        REQUIRE(rewritten.exit_code == 0);
        CHECK(read_text(copy).find("identity_schema") != std::string::npos);
        const auto same = run_pulp({"graph", "diff", envelope.string(), copy.string(), "--json"});
        INFO(same.stdout_output << same.stderr_output);
        REQUIRE(same.exit_code == 0);
        CHECK(choc::json::parse(same.stdout_output)["equal"].getBool());
        // Byte stable on a second run.
        const auto once = read_text(copy);
        REQUIRE(run_pulp({"graph", op, copy.string()}).exit_code == 0);
        CHECK(read_text(copy) == once);
    }

    // Subgraph records cannot be rewritten without their templates, so the
    // file is refused and left unchanged rather than stripped.
    auto with_records = read_text(envelope);
    const auto brace = with_records.find('{');
    REQUIRE(brace != std::string::npos);
    with_records.insert(brace + 1, R"("subgraphs":[],)");
    const auto records = dir / "records.pulpgraph";
    std::ofstream(records, std::ios::binary) << with_records;
    for (const std::string op : {"fmt", "canonicalize"}) {
        const auto refused_rewrite = run_pulp({"graph", op, records.string(), "--json"});
        INFO(op << ": " << refused_rewrite.stdout_output << refused_rewrite.stderr_output);
        REQUIRE(refused_rewrite.exit_code == 1);
        CHECK(cli_code(refused_rewrite) == "subgraph_records_unsupported");
        CHECK(read_text(records) == with_records);
    }
    const auto saved_records = dir / "records-out.pulpgraph";
    const auto refused_save =
        run_pulp({"graph", "save", records.string(), "--output", saved_records.string(), "--json"});
    REQUIRE(refused_save.exit_code == 1);
    CHECK(cli_code(refused_save) == "subgraph_records_unsupported");
    CHECK_FALSE(fs::exists(saved_records));

    // Subgraph records with no identity envelope are refused too: the
    // inspector reports the missing envelope first, which must not let the
    // rewrite fall through to the plain writer.
    for (const char* text :
         {R"({"format_version":4,"nodes":[],"connections":[],"subgraphs":[]})",
          R"({"format_version":5,"nodes":[],"connections":[],"subgraphs":[]})"}) {
        const auto bare = dir / "records-bare.pulpgraph";
        std::ofstream(bare, std::ios::binary) << text;
        for (const std::string op : {"fmt", "canonicalize"}) {
            const auto refused_bare = run_pulp({"graph", op, bare.string(), "--json"});
            INFO(op << ": " << refused_bare.stdout_output << refused_bare.stderr_output);
            REQUIRE(refused_bare.exit_code == 1);
            CHECK(cli_code(refused_bare) == "subgraph_records_unsupported");
            CHECK(read_text(bare) == text);
        }
        const auto bare_out = dir / "records-bare-out.pulpgraph";
        const auto refused_bare_save =
            run_pulp({"graph", "save", bare.string(), "--output", bare_out.string(), "--json"});
        REQUIRE(refused_bare_save.exit_code == 1);
        CHECK(cli_code(refused_bare_save) == "subgraph_records_unsupported");
        CHECK_FALSE(fs::exists(bare_out));
    }

    // A document the shape preflight refuses (runaway nesting) is refused by
    // the loader under the same bounds; the rewrite helper must not parse it
    // first, so it exits cleanly with the typed document code, file unchanged.
    {
        const std::string deep = R"({"format_version":5,"subgraphs":)" + std::string(200000, '[') +
                                 std::string(200000, ']') + "}";
        const auto deep_path = dir / "deep.pulpgraph";
        std::ofstream(deep_path, std::ios::binary) << deep;
        for (const std::string op : {"fmt", "canonicalize", "save"}) {
            INFO(op);
            std::vector<std::string> argv{"graph", op, deep_path.string(), "--json"};
            if (op == "save") {
                argv.push_back("--output");
                argv.push_back((dir / "deep-out.pulpgraph").string());
            }
            const auto refused_deep = run_pulp(argv);
            INFO(refused_deep.stdout_output << refused_deep.stderr_output);
            REQUIRE(refused_deep.exit_code == 1);
            CHECK(cli_code(refused_deep) != "subgraph_records_unsupported");
            CHECK(read_text(deep_path) == deep);
        }
        CHECK_FALSE(fs::exists(dir / "deep-out.pulpgraph"));
    }

    // A missing input keeps the typed file code on the rewrite path.
    const auto missing = run_pulp({"graph", "fmt", (dir / "absent.pulpgraph").string(), "--json"});
    REQUIRE(missing.exit_code == 1);
    CHECK(cli_code(missing) == pulp::host::graph_patch_code_name(Code::FileUnreadable));
}
