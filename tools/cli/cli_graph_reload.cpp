#include "cli_graph_reload.hpp"
#include "cli_common.hpp"
#include "cli_graph_reload_receipt.hpp"
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

bool GraphReloadWatch::renew() {
    std::ostringstream renewal;
    auto* previous = std::cout.rdbuf(renewal.rdbuf());
    int renewed = 1;
    try {
        renewed = cmd_control({"call", "dev.pulp.session/control@1", "--instance",
                               options_.instance, "--grant", options_.grant, "--params",
                               R"({"action":"renew"})", "--json", "--timeout-ms", "3000"});
    } catch (...) {
        std::cout.rdbuf(previous);
        renewal.str("lease renewal threw before a receipt\n");
    }
    std::cout.rdbuf(previous);
    if (renewed == 0) {
        last_renewal_refusal_.clear();
        return true;
    }
    const auto output = renewal.str();
    if (graph_reload_authority_refused(output)) {
        // Admission refused the grant itself (revoked, expired or missing
        // session control); renewing with it again cannot succeed.
        halted_ = true;
        std::cerr << "graph reload: existing controller lease renewal refused: " << output
                  << "graph reload: stopped for this watch; live graph retained. Request a new "
                     "grant with `pulp control grant-request --instance "
                  << options_.instance << " --profile develop` and restart the watch with it\n"
                  << std::flush;
        return false;
    }
    // A lease refusal repeats every renewal until the lease is reacquired; say
    // it once per distinct refusal, not every two seconds.
    const auto key = graph_reload_refusal_key(output);
    if (key != last_renewal_refusal_) {
        last_renewal_refusal_ = key;
        std::cerr << "graph reload: existing controller lease renewal refused: " << output
                  << "graph reload: live graph retained; reacquire the controller lease with "
                     "`pulp control call --instance "
                  << options_.instance
                  << " dev.pulp.session/control@1 --params '{\"action\":\"acquire\"}'`\n"
                  << std::flush;
    }
    return false;
}

void GraphReloadWatch::poll() {
    if (halted_)
        return;
    if ((polls_++ % 4) == 0)
        lease_ready_ = renew();
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
    // Confirm authority immediately before sending, so a lease lost or a
    // grant revoked since the last periodic renewal is refused here rather
    // than by admission on the reload request.
    lease_ready_ = renew();
    if (!lease_ready_)
        return;
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
    retry_budget_.offer(change.text);
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
        receipt.str("");
    }
    std::cout.rdbuf(previous);
    const auto outcome = classify_graph_reload_receipt(result, receipt.str(), options_.generation);
    if (outcome != GraphReloadOutcome::AuthorityRefused)
        std::cout << "graph reload: " << receipt.str() << std::flush;
    switch (outcome) {
    case GraphReloadOutcome::Published:
    case GraphReloadOutcome::Refused:
        retry_budget_.reset();
        watch_->acknowledge();
        return;
    case GraphReloadOutcome::StaleGeneration:
        // Not acknowledged: the file stays unconsumed, and resending with the
        // same expected generation would repeat the refusal on every change.
        halted_ = true;
        std::cerr << "graph reload: the live graph moved past generation " << options_.generation
                  << " (stale-base); graph reload is paused for this watch and the file was not "
                     "applied. Read the current generation from detail.generation of the receipt "
                     "above, then restart the watch with --graph-generation set to it\n"
                  << std::flush;
        return;
    case GraphReloadOutcome::NotApplied:
        if (!retry_budget_.record_failure()) {
            std::cerr << "graph reload: request was not applied; the change will be retried\n";
            return;
        }
        halted_ = true;
        std::cerr << "graph reload: request was refused before it ran " << kMaxNotApplied
                  << " times; graph reload stopped for this watch; live graph retained. Check "
                     "that the product is running and responsive, then restart the watch\n"
                  << std::flush;
        return;
    case GraphReloadOutcome::AuthorityRefused:
        halted_ = true;
        std::cerr << "graph reload: grant refused by admission: " << receipt.str()
                  << "graph reload: stopped for this watch; live graph retained. Request a new "
                     "grant with `pulp control grant-request --instance "
                  << options_.instance << " --profile develop` and restart the watch with it\n"
                  << std::flush;
        return;
    case GraphReloadOutcome::Unknown:
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
