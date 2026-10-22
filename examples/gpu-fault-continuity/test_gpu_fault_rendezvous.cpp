// The run configuration of the GPU fault-continuity plug-in in hosts that
// cannot pass it environment variables (WP-K): the rendezvous file, its
// refusals, and the environment's precedence over it.

#include "gpu_fault_rendezvous.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>

#include <unistd.h>

using namespace pulp::examples::gpu_fault;
namespace fs = std::filesystem;

namespace {

constexpr std::int64_t kNow = 1'800'000'000;
constexpr const char* kNonce = "0123456789abcdef0123456789abcdef";

struct Root {
    fs::path path;
    fs::path run;
    Root() {
        path = fs::temp_directory_path() /
               ("pulp-gpu-fault-rendezvous-" + std::to_string(::getpid()) + "-" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        run = path / "runs" / kNonce;
        fs::create_directories(run);
    }
    ~Root() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    void write_active(const std::string& run_dir, const std::string& request, const std::string& ir,
                      const std::string& nonce = kNonce, std::int64_t expires = kNow + 600,
                      const std::string& schema = kRendezvousSchema) const {
        std::ofstream(path / "active.json")
            << "{\"schema\":\"" << schema << "\",\"run_dir\":\"" << run_dir << "\",\"request\":\""
            << request << "\",\"ir\":\"" << ir << "\",\"nonce\":\"" << nonce
            << "\",\"expires_unix\":" << expires << "}\n";
    }
    void write_valid() const {
        write_active(run.string(), (run / "fault-request.json").string(),
                     (run / "ir.f32").string());
    }
};

RunConfigSources sources_for(const Root& root, std::map<std::string, std::string> env = {}) {
    RunConfigSources s;
    auto shared = std::make_shared<std::map<std::string, std::string>>(std::move(env));
    s.getenv = [shared](const char* name) -> const char* {
        const auto it = shared->find(name);
        return it == shared->end() ? nullptr : it->second.c_str();
    };
    s.rendezvous_root = root.path;
    s.now_unix = kNow;
    return s;
}

} // namespace

TEST_CASE("a valid rendezvous configures the run", "[gpu-fault-continuity][rendezvous]") {
    Root root;
    root.write_valid();
    const auto config = resolve_run_config(sources_for(root));
    CHECK(config.source == "rendezvous");
    CHECK(config.run_dir == root.run);
    CHECK(config.request == root.run / "fault-request.json");
    CHECK(config.ir == root.run / "ir.f32");
    CHECK(config.nonce == kNonce);
}

TEST_CASE("the environment wins over a rendezvous", "[gpu-fault-continuity][rendezvous]") {
    Root root;
    root.write_valid();
    const auto config =
        resolve_run_config(sources_for(root, {{"PULP_GPU_FAULT_RUN_DIR", "/env/run"},
                                              {"PULP_GPU_FAULT_REQUEST", "/env/run/req.json"},
                                              {"PULP_GPU_FAULT_IR", "/env/run/ir.f32"},
                                              {"PULP_GPU_FAULT_NONCE", "fedcba9876543210"}}));
    CHECK(config.source == "env");
    CHECK(config.run_dir == "/env/run");
    CHECK(config.nonce == "fedcba9876543210");
}

TEST_CASE("no rendezvous and no environment leaves a plain convolver",
          "[gpu-fault-continuity][rendezvous]") {
    Root root;
    const auto config = resolve_run_config(sources_for(root));
    CHECK(config.source == "none");
    CHECK(config.run_dir.empty());
}

TEST_CASE("a rendezvous that fails any check is refused", "[gpu-fault-continuity][rendezvous]") {
    Root root;
    const auto req = (root.run / "fault-request.json").string();
    const auto ir = (root.run / "ir.f32").string();
    const auto refused = [&](const char* expect) {
        const auto config = resolve_run_config(sources_for(root));
        CHECK(config.source == "none");
        CHECK(config.run_dir.empty());
        CHECK(config.detail.find(expect) != std::string::npos);
    };

    SECTION("expired") {
        root.write_active(root.run.string(), req, ir, kNonce, kNow);
        refused("expired");
    }
    SECTION("wrong schema") {
        root.write_active(root.run.string(), req, ir, kNonce, kNow + 600, "other.v1");
        refused("schema");
    }
    SECTION("short nonce") {
        root.write_active(root.run.string(), req, ir, "abc");
        refused("nonce");
    }
    SECTION("run dir outside runs/") {
        const auto outside = root.path / "elsewhere";
        fs::create_directories(outside);
        root.write_active(outside.string(), (outside / "r.json").string(),
                          (outside / "ir.f32").string());
        refused("run_dir");
    }
    SECTION("run dir escaping runs/ through ..") {
        root.write_active((root.path / "runs" / ".." / "x").string(), req, ir);
        refused("run_dir");
    }
    SECTION("run dir escaping runs/ through a symlink") {
        const auto outside = root.path / "elsewhere";
        fs::create_directories(outside);
        const auto link = root.path / "runs" / "link";
        fs::create_directory_symlink(outside, link);
        root.write_active(link.string(), (link / "r.json").string(), (link / "ir.f32").string());
        refused("run_dir");
    }
    SECTION("run dir is runs/ itself") {
        root.write_active((root.path / "runs").string(), req, ir);
        refused("run_dir");
    }
    SECTION("request outside the run dir") {
        root.write_active(root.run.string(), "/etc/passwd", ir);
        refused("inside run_dir");
    }
}

TEST_CASE("the rendezvous root comes from the account, not HOME",
          "[gpu-fault-continuity][rendezvous]") {
    // Inside a sandboxed host HOME is the app's container. Clear the test
    // override and point HOME elsewhere; the root must not follow it.
    const char* saved_override = std::getenv("PULP_GPU_FAULT_RENDEZVOUS_ROOT");
    const std::string saved_home = std::getenv("HOME") ? std::getenv("HOME") : "";
    ::unsetenv("PULP_GPU_FAULT_RENDEZVOUS_ROOT");
    ::setenv("HOME", "/nonexistent/container/home", 1);
    const auto root = default_rendezvous_root();
    ::setenv("HOME", saved_home.c_str(), 1);
    if (saved_override)
        ::setenv("PULP_GPU_FAULT_RENDEZVOUS_ROOT", saved_override, 1);
    CHECK(root.string().find("/nonexistent/") == std::string::npos);
    CHECK(root.filename() == "gpu-fault-continuity");
    CHECK(root.parent_path().filename() == "Pulp");
}

TEST_CASE("the host-process event names this process", "[gpu-fault-continuity][rendezvous]") {
    const auto process = current_host_process();
    CHECK(process.pid == static_cast<int>(::getpid()));
    CHECK(process.ppid == static_cast<int>(::getppid()));
    CHECK_FALSE(process.path.empty());
    // A plain test process is responsible for itself or its launcher, and is
    // not sandboxed; -1 would mean the lookup is missing on this OS.
    CHECK(process.responsible_pid > 0);
    CHECK(process.sandboxed == 0);
    RunConfig config;
    config.source = "rendezvous";
    config.nonce = kNonce;
    const auto line = host_process_event(process, config);
    CHECK(line.find("\"event\":\"host_process\"") != std::string::npos);
    CHECK(line.find("\"config_source\":\"rendezvous\"") != std::string::npos);
    CHECK(line.find(kNonce) != std::string::npos);
}

TEST_CASE("the host process reports a start time before its claim",
          "[gpu-fault-continuity][rendezvous]") {
    auto process = current_host_process();
    process.claimed_unix_us = unix_now_us();
    // The test process started before this line ran, and not in the future.
    CHECK(process.start_unix_us > 0);
    CHECK(process.start_unix_us <= process.claimed_unix_us);
    CHECK(process.claimed_unix_us - process.start_unix_us < 24ll * 3600 * 1'000'000);
    CHECK(claim_record(process) ==
          std::to_string(process.pid) + "\n" + std::to_string(process.claimed_unix_us) + "\n");
    const auto line = host_process_event(process, RunConfig{});
    CHECK(line.find("\"start_unix_us\":" + std::to_string(process.start_unix_us)) !=
          std::string::npos);
    CHECK(line.find("\"claimed_unix_us\":" + std::to_string(process.claimed_unix_us)) !=
          std::string::npos);
}
