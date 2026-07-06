#pragma once

// Offline `pulp design <verb>` subcommand handlers — the pure-data transforms
// (lint, diff, compile, lint-adherence, record, gallery, handoff, variants,
// tweak) that operate on design artifacts without launching the live GPU design
// tool. The live-tool launcher stays in cmd_design.cpp and dispatches to these.

#include <string>
#include <vector>

namespace pulp::cli::design {

int run_lint(const std::vector<std::string>& rest);
int run_diff(const std::vector<std::string>& rest);
int run_compile(const std::vector<std::string>& rest);
int run_lint_adherence(const std::vector<std::string>& rest);
int run_record(const std::vector<std::string>& rest);
int run_gallery(const std::vector<std::string>& rest);
int run_handoff(const std::vector<std::string>& rest);
int run_variants(const std::vector<std::string>& rest);
int run_tweak(const std::vector<std::string>& rest);

}  // namespace pulp::cli::design
