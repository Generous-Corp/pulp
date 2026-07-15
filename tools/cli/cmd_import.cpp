// pulp import — read an existing audio-plugin project read-only and emit a
// Pulp migration scaffold.
//
// Framework importers are vendor-specific add-on tools that live in their own
// private repos. The Pulp SDK owns only the generalised substrate: a discovery
// index of known frameworks (DATA, the one place real markers/vendor names
// appear), a JSON-over-stdio SPI to drive an installed importer, and the
// emission step (the SDK writes files; the importer only proposes a plan).
//
// This translation unit names NO framework and NO vendor: framework identity
// is runtime DATA loaded from tools/import/known-frameworks.json.
//
// This file is the command front-end: argument parsing + dispatch only. The
// SPI-verb orchestration (detect / inspect / emit + their shared helpers)
// lives in import_run.{hpp,cpp}.
//
// Subcommands:
//   pulp import detect <dir>
//   pulp import inspect --from <fw> <dir> [opts]
//   pulp import emit    --from <fw> <dir> --output <out> [opts]
//   pulp import <dir>                       (alias for detect)

#include "cli_common.hpp"
#include "import_run.hpp"
#include "importer_git_install.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

namespace run = pulp::cli::import_run;

void print_usage() {
    std::cout <<
        "Usage: pulp import <command> [options]\n\n"
        "Commands:\n"
        "  detect <dir>                 Rank known-framework candidates for a project\n"
        "  inspect --from <fw> <dir>    Run an importer's SPI analyze → write ProjectIR\n"
        "  emit    --from <fw> <dir> --output <out>\n"
        "                               analyze → emit → materialise a buildable scaffold\n"
        "  install <url>                Clone an add-on importer from a git URL and register it\n"
        "  uninstall <id>               Remove an installed importer by id\n"
        "  <dir>                        Alias for `detect <dir>`\n\n"
        "install options:\n"
        "  --accept-importer-terms      Accept the importer's terms non-interactively (CI)\n"
        "  --force                      Reinstall even if the importer is already present\n\n"
        "inspect / emit options:\n"
        "  --from <framework>           Framework id (see `pulp import detect`)\n"
        "  --framework-path <path>      The user's own framework checkout (read-only)\n"
        "  --extra-include <dir>        Extra include dir for the importer (repeatable)\n"
        "  -o, --output-ir <file>       inspect: write ProjectIR JSON to <file>\n"
        "  --report <file.md>           inspect: write a human report\n"
        "  --output <dir>               emit: scaffold output directory\n"
        "  --importer-cmd <cmd>         Override importer resolution with a command\n"
        "  --accept-importer-terms      Accept the importer's terms non-interactively (CI)\n";
}

}  // namespace

int cmd_import(const std::vector<std::string>& args) {
    if (args.empty()) {
        print_usage();
        return 0;
    }

    const std::string& first = args[0];
    if (first == "-h" || first == "--help" || first == "help") {
        print_usage();
        return 0;
    }

    // Install / uninstall drive an add-on importer by git URL — a distinct
    // argument shape from the detect/inspect/emit verbs, so dispatch them first.
    if (first == "install")
        return pulp::cli::import_install::run_import_install(
            {args.begin() + 1, args.end()});
    if (first == "uninstall")
        return pulp::cli::import_install::run_import_uninstall(
            {args.begin() + 1, args.end()});

    // Subcommand detection. `detect`, `inspect`, `emit` are explicit; any other
    // first token that is an existing path is treated as `detect <dir>`.
    std::string sub;
    size_t arg_start = 1;
    if (first == "detect" || first == "inspect" || first == "emit") {
        sub = first;
    } else {
        sub = "detect";
        arg_start = 0;  // first token is the directory
    }

    run::ImportOptions o;
    std::vector<std::string> positionals;
    for (size_t i = arg_start; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto need = [&](const char* flag) -> std::string {
            if (i + 1 >= args.size()) {
                std::cerr << "pulp import: " << flag << " requires a value\n";
                return std::string{};
            }
            return args[++i];
        };
        if (a == "--from") o.from = need("--from");
        else if (a == "--framework-path") o.framework_path = need("--framework-path");
        else if (a == "--extra-include") o.extra_includes.push_back(need("--extra-include"));
        else if (a == "-o" || a == "--output-ir") o.ir_out = need(a.c_str());
        else if (a == "--report") o.report = need("--report");
        else if (a == "--output") o.output = need("--output");
        else if (a == "--importer-cmd") o.importer_cmd = need("--importer-cmd");
        else if (a == "--accept-importer-terms") o.accept_importer_terms = true;
        // Terms-text override (paired with --importer-cmd, which has no registry
        // entry to carry the terms). Mostly for tests + power users.
        else if (a == "--importer-terms-text") o.terms_text = need("--importer-terms-text");
        else if (a == "--importer-terms-version") o.terms_version = need("--importer-terms-version");
        else if (!a.empty() && a[0] == '-') {
            std::cerr << "pulp import: unknown option '" << a << "'\n";
            return 2;
        } else {
            positionals.push_back(a);
        }
    }

    if (!positionals.empty()) o.dir = positionals.front();
    if (o.dir.empty()) o.dir = fs::current_path();

    if (sub == "detect") return run::run_detect(o.dir);
    if (sub == "inspect") return run::run_inspect(o);
    if (sub == "emit") return run::run_emit(o);

    print_usage();
    return 2;
}
