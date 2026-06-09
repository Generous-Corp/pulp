// pulp import — SPI-verb orchestration (detect / inspect / emit).
//
// This unit owns the verb runners and their shared helpers (framework-index
// resolution, importer resolution, the SPI request/response envelope handling,
// payload builders, the clean-room output gate, and scaffold materialisation).
// `cmd_import.cpp` is reduced to argument parsing + dispatch into these
// entry points.
//
// Like `cmd_import.cpp`, this translation unit names NO framework and NO
// vendor: framework identity is runtime DATA loaded from
// tools/import/known-frameworks.json.

#pragma once

#include "cli_common.hpp"

#include <string>
#include <vector>

namespace pulp::cli::import_run {

// Options shared by the inspect and emit verbs. Populated by the arg parser in
// cmd_import.cpp and consumed by the runners below.
struct ImportOptions {
    std::string from;          // framework id (required for inspect/emit)
    fs::path dir;              // project dir (required)
    fs::path output;           // emit: output dir
    fs::path ir_out;           // inspect: -o IR.json
    fs::path report;           // --report MD
    std::string framework_path;
    std::vector<std::string> extra_includes;
    std::string importer_cmd;
    bool accept_importer_terms = false;  // non-interactive IMPORTER_TERMS accept

    // Terms text override for `--importer-cmd` / test use: when no tool-registry
    // entry carries the terms (e.g. an explicit importer command), this supplies
    // the gate body directly. Empty in the normal registry-resolved path.
    std::string terms_text;
    std::string terms_version;
    std::string vendor_id;
};

// `pulp import detect <dir>` — rank known-framework candidates for a project.
int run_detect(const fs::path& dir);

// `pulp import inspect --from <fw> <dir>` — run the importer's SPI `analyze`
// verb and write/print the resulting ProjectIR.
int run_inspect(const ImportOptions& o);

// `pulp import emit --from <fw> <dir> --output <out>` — analyze → emit →
// clean-room scan → materialise a buildable scaffold + provenance marker.
int run_emit(const ImportOptions& o);

}  // namespace pulp::cli::import_run
