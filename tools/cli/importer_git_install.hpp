// SPDX-License-Identifier: MIT
//
// importer_git_install — install / uninstall an add-on importer from a git URL.
//
// This is the URL-driven install path behind `pulp import install <url>` and
// `pulp import uninstall <id>`. It is deliberately distinct from the
// registry+artifact install in importer_install.cpp: there is NO shipped
// registry entry, NO per-platform pinned artifact, and NO sha256 pin. An
// importer is simply a git repository the user names by URL, and everything the
// SDK needs to know about it — its id, its SPI window, and its terms of use —
// is read from a `tool.json` and a terms file INSIDE the cloned repo, never
// from anything the SDK ships.
//
// Privacy invariant (firm): the SDK only ever knows the URL it was handed. A
// clone that fails is surfaced with git's own error and a single neutral,
// URL-agnostic message. The SDK never states, infers, or records whether a
// given repository exists, or whether it is public or private — a user with
// access and a user without get the same SDK-authored failure text.
//
// Vendor-agnostic: this unit names NO framework and NO vendor. Framework
// identity is runtime DATA carried by the cloned repo's tool.json + detection
// index; the SDK code only knows the SHAPE of those.
#pragma once

#include "import_terms.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace pulp::cli::import_install {

namespace fs = std::filesystem;

// The subset of a cloned importer's tool.json the SDK needs. All fields are
// opaque DATA the repo supplies; the SDK never invents them.
struct ToolManifest {
    bool parsed = false;
    std::string error;          // non-empty when the tool.json is missing/invalid

    std::string id;             // importer tool id (install key)
    std::string category;       // must be "importer"
    std::string display_name;
    std::string pinned_version;
    int spi_min = 0;            // SPI version window the importer speaks
    int spi_max = 0;
    std::string sdk_min;        // optional Pulp SDK bounds (semver strings)
    std::string sdk_max;
    std::string terms_version;  // bumping it re-prompts (via the terms hash)
    std::string terms_file;     // repo-relative path to the terms body (e.g. TERMS.md)
};

// Parse the tool.json body an importer repo ships. Pure — unit-tested directly.
ToolManifest parse_tool_manifest(const std::string& tool_json_text);

struct InstallResult {
    bool ok = false;
    std::string error;             // SDK-authored, URL-agnostic on a fetch failure
    std::string git_output;        // git's own stderr, surfaced verbatim on fetch failure
    std::string importer_id;
    std::string installed_version;
    fs::path install_dir;          // ~/.pulp/tools/<id>
    fs::path record_path;          // ~/.pulp/importers/<id>.json
    bool terms_prompted = false;   // did the interactive gate render a prompt this run?

    // Where the flow stopped. `Fetch` is the only stage that must stay
    // URL-agnostic (the privacy invariant); the later stages run only after a
    // successful clone, so they may reference the repo's own contents.
    enum class Stage { Ok, Fetch, Manifest, SpiWindow, Terms, Io };
    Stage stage = Stage::Ok;
};

// Clone `url` with the user's own git, read its tool.json + terms file, enforce
// the SPI version window, run the accept-to-run terms gate, and install the
// tree under ~/.pulp/tools/<id>/ with an install record under
// ~/.pulp/importers/<id>.json.
//
//   accept_terms_flag — skip the interactive prompt for CI (still records it)
//   force             — reinstall even when an up-to-date record exists
//   io                — terms-gate console (cin/cout + interactive for the CLI;
//                       injected string streams for tests)
//
// Honors $PULP_HOME for the install root and the acceptance store, and
// $PULP_SDK_VERSION for the version-window check (mirrors importer_install).
InstallResult install_from_git(const std::string& url,
                               bool accept_terms_flag,
                               bool force,
                               import_terms::GateIo io);

// `pulp import install <url> [--accept-importer-terms] [--force]` wrapper:
// parses args, drives install_from_git against the interactive console, prints
// the outcome, and returns a process exit code.
int run_import_install(const std::vector<std::string>& args);

// `pulp import uninstall <id>` wrapper: removes the install tree, record, and
// any installed skill via tools::uninstall_importer. Returns a process exit code.
int run_import_uninstall(const std::vector<std::string>& args);

}  // namespace pulp::cli::import_install
