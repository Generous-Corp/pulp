#include "cli_graph_reload.hpp"
#include "cli_common.hpp"
#include <charconv>
#include <choc/text/choc_JSON.h>
#include <iostream>
#include <pulp/host/graph_file_reload.hpp>
#include <sstream>

bool parse_graph_reload_option(const std::vector<std::string>& args, std::size_t& index,
                               GraphReloadOptions& options, std::string& error) {
    const auto& flag = args[index];
    if (flag != "--graph-file" && flag != "--graph-instance" && flag != "--graph-grant" &&
        flag != "--graph-generation" && flag != "--graph-watch-polls")
        return false;
    if (index + 1 == args.size() || args[index + 1].starts_with("--")) {
        error = flag + " requires a value";
        return true;
    }
    const auto& value = args[++index];
    if (flag == "--graph-file")
        options.file = value;
    else if (flag == "--graph-instance")
        options.instance = value;
    else if (flag == "--graph-grant")
        options.grant = value;
    else {
        std::uint64_t number = 0;
        auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
            number > 9007199254740991ULL ||
            (flag == "--graph-watch-polls" && (number == 0 || number > 120))) {
            error = flag + " requires a bounded unsigned integer";
            return true;
        }
        if (flag == "--graph-generation") {
            options.generation = number;
            options.generation_set = true;
        } else
            options.watch_polls = static_cast<unsigned>(number);
    }
    return true;
}
bool validate_graph_reload_options(const GraphReloadOptions& options, std::string& error) {
    if (options.file.empty()) {
        if (!options.instance.empty() || !options.grant.empty() || options.generation_set ||
            options.watch_polls)
            error = "graph options require --graph-file";
    } else if (options.instance.empty() || options.grant.empty() || !options.generation_set)
        error = "--graph-file requires --graph-instance, --graph-grant and --graph-generation";
    else if (options.file.extension() != ".pat" && options.file.extension() != ".pulpgraph")
        error = "--graph-file requires .pat or .pulpgraph";
    return error.empty();
}
GraphReloadWatch::GraphReloadWatch(GraphReloadOptions options)
    : options_(std::move(options)),
      watch_(std::make_unique<pulp::host::GraphFileWatch>(options_.file)) {
    const auto seeded = watch_->poll();
    if (seeded.code != pulp::host::GraphFileChange::Code::Unchanged &&
        seeded.code != pulp::host::GraphFileChange::Code::Pending)
        report_file_refusal(static_cast<int>(seeded.code));
}
GraphReloadWatch::~GraphReloadWatch() = default;

void GraphReloadWatch::report_file_refusal(int value) const {
    using Code = pulp::host::GraphFileChange::Code;
    const auto code = static_cast<Code>(value);
    const char* name = "unreadable";
    const char* fix = "check that the file exists and is readable";
    if (code == Code::TooLarge) {
        name = "too-large";
        fix = "reduce the graph file to 1 MiB or less";
    } else if (code == Code::UnsupportedFormat) {
        name = "unsupported-format";
        fix = "use a .pat or .pulpgraph file";
    }
    std::cerr << "graph reload: graph file " << name << " (" << options_.file.string()
              << "): " << fix << "; live graph retained\n"
              << std::flush;
}

namespace {
enum class ReceiptOutcome { Published, Refused, NotApplied, Unknown };

// Completed and typed refusals are terminal: the executor ran and the result
// is final. A failure without the executor's typed code was refused before it
// ran (admission, grant, deadline before start), so the change is retried.
// Anything else may or may not have published.
ReceiptOutcome classify_receipt(int exit_code, const std::string& receipt,
                                std::uint64_t& generation) {
    try {
        const auto response = choc::json::parse(receipt);
        if (!response["state"].isString())
            return ReceiptOutcome::Unknown;
        const auto state = response["state"].getString();
        const auto detail = response["detail"];
        const bool typed = detail.isObject() && detail["code"].isString();
        if ((state == "completed" || state == "completed_after_revocation") && exit_code == 0) {
            if (detail.isObject() && detail["generation"].isInt())
                generation = static_cast<std::uint64_t>(detail["generation"].getInt64());
            return ReceiptOutcome::Published;
        }
        if (state == "failed" && typed)
            return ReceiptOutcome::Refused;
        if (state == "failed" || state == "cancelled")
            return ReceiptOutcome::NotApplied;
        return ReceiptOutcome::Unknown;
    } catch (...) {
        return ReceiptOutcome::Unknown;
    }
}
} // namespace

void GraphReloadWatch::poll() {
    if (halted_)
        return;
    if ((polls_++ % 4) == 0) {
        lease_ready_ = false;
        std::ostringstream renewal;
        auto* previous = std::cout.rdbuf(renewal.rdbuf());
        int renewed = 1;
        try {
            renewed = cmd_control({"call", "dev.pulp.session/control@1", "--instance",
                                   options_.instance, "--grant", options_.grant, "--params",
                                   R"({"action":"renew"})", "--json", "--timeout-ms", "3000"});
        } catch (...) {
            std::cout.rdbuf(previous);
            std::cerr << "graph reload: lease renewal failed; live graph retained\n";
            return;
        }
        std::cout.rdbuf(previous);
        if (renewed != 0) {
            std::cerr << "graph reload: existing controller lease renewal refused: "
                      << renewal.str() << std::flush;
            return;
        }
        lease_ready_ = true;
    }
    if (!lease_ready_)
        return;
    const auto change = watch_->poll();
    using Code = pulp::host::GraphFileChange::Code;
    if (change.code == Code::Unchanged || change.code == Code::Pending)
        return;
    if (change.code != Code::Changed) {
        report_file_refusal(static_cast<int>(change.code));
        return;
    }
    auto params = choc::value::createObject("");
    params.addMember("text", change.text);
    params.addMember("format", change.format == pulp::host::GraphFileFormat::Notation
                                   ? "notation"
                                   : "pulpgraph");
    params.addMember("expected_generation", static_cast<std::int64_t>(options_.generation));
    const auto json = choc::json::toString(params, false);
    if (json.size() > 512u * 1024u) {
        // Deterministic for these contents: do not resend until the file changes.
        watch_->acknowledge();
        std::cerr << "graph reload: serialized request exceeds 512 KiB; reduce the graph file; "
                     "live graph retained\n";
        return;
    }
    // Use the same enrollment, grant, deadline, receipt and admission path as
    // `pulp control call`; the watch loop has no direct graph authority.
    std::ostringstream receipt;
    auto* previous = std::cout.rdbuf(receipt.rdbuf());
    int result = 1;
    try {
        result = cmd_control({"call", "dev.pulp.graph/file.reload@1", "--instance",
                              options_.instance, "--grant", options_.grant, "--params", json,
                              "--json", "--timeout-ms", "3000"});
    } catch (...) {
        std::cout.rdbuf(previous);
        std::cerr << "graph reload: control request failed before a receipt; the change will be "
                     "retried; live graph retained\n";
        return;
    }
    std::cout.rdbuf(previous);
    std::cout << "graph reload: " << receipt.str() << std::flush;
    switch (classify_receipt(result, receipt.str(), options_.generation)) {
    case ReceiptOutcome::Published:
    case ReceiptOutcome::Refused:
        watch_->acknowledge();
        return;
    case ReceiptOutcome::NotApplied:
        std::cerr << "graph reload: request was not applied; the change will be retried\n";
        return;
    case ReceiptOutcome::Unknown:
        halted_ = true;
        std::cerr << "graph reload: outcome unknown; the live graph may already hold this file. "
                     "Graph reload is paused for this watch. Read the current generation from "
                     "detail.generation of the next refused `pulp control call "
                     "dev.pulp.graph/file.reload@1` receipt, then restart with "
                     "--graph-generation set to it\n"
                  << std::flush;
        return;
    }
}
