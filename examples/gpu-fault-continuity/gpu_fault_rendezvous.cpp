#include "gpu_fault_rendezvous.hpp"

#ifndef PULP_GPU_FAULT_TEST_ONLY
#error "gpu_fault_rendezvous.cpp belongs only to the PULP_GPU_AUDIO_FAULT_HOOKS test plug-in"
#endif

#include <chrono>
#include <cstdlib>
#include <system_error>

#include <dlfcn.h>
#include <fcntl.h>
#include <libproc.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pulp::examples::gpu_fault {
namespace {

namespace fs = std::filesystem;

// active.json is written by the harness as a flat object of plain strings and
// integers, so a key scan is enough. well_formed() refuses what a key scan
// could misread (a repeated key, an escape sequence); anything else malformed
// fails validation.
std::string json_string(const std::string& text, const char* key) {
    const auto at = text.find(std::string("\"") + key + "\"");
    if (at == std::string::npos)
        return {};
    const auto colon = text.find(':', at);
    const auto open = colon == std::string::npos ? colon : text.find('"', colon + 1);
    const auto close = open == std::string::npos ? open : text.find('"', open + 1);
    return close == std::string::npos ? std::string() : text.substr(open + 1, close - open - 1);
}

std::int64_t json_int(const std::string& text, const char* key) {
    const auto at = text.find(std::string("\"") + key + "\"");
    if (at == std::string::npos)
        return -1;
    const auto colon = text.find(':', at);
    return colon == std::string::npos ? -1 : std::strtoll(text.c_str() + colon + 1, nullptr, 10);
}

std::size_t occurrences(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
        ++count;
    return count;
}

bool well_formed(const std::string& body) {
    if (body.find('\\') != std::string::npos)
        return false;
    for (const char* key : {"schema", "run_dir", "request", "ir", "nonce", "expires_unix"})
        if (occurrences(body, std::string("\"") + key + "\"") != 1)
            return false;
    return true;
}

// Reads `path` only if it is a regular file reached without following a
// symlink, checked on the open descriptor rather than by a separate stat.
bool read_regular_no_follow(const fs::path& path, std::string& out) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct stat st{};
    bool ok = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size < (1 << 16);
    if (ok) {
        out.resize(static_cast<std::size_t>(st.st_size));
        ok = ::read(fd, out.data(), out.size()) == static_cast<ssize_t>(out.size());
    }
    ::close(fd);
    return ok;
}

// True when `path` opens, without following a symlink, as a directory.
bool directory_no_follow(const fs::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct stat st{};
    const bool ok = ::fstat(fd, &st) == 0 && S_ISDIR(st.st_mode);
    ::close(fd);
    return ok;
}

bool is_hex(const std::string& s) {
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}

bool no_follow_is_link(const fs::path& path) {
    std::error_code ec;
    return fs::is_symlink(fs::symlink_status(path, ec));
}

// True when `path` resolves (symlinks included) to somewhere strictly inside `dir`.
bool strictly_inside(const fs::path& path, const fs::path& dir) {
    std::error_code ec;
    const fs::path p = fs::weakly_canonical(path, ec);
    if (ec)
        return false;
    const fs::path d = fs::weakly_canonical(dir, ec);
    if (ec)
        return false;
    const fs::path rel = p.lexically_relative(d);
    return !rel.empty() && *rel.begin() != ".." && rel != ".";
}

std::string escape(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '"' || c == '\\')
            out.push_back('\\');
        if (static_cast<unsigned char>(c) >= 0x20)
            out.push_back(c);
    }
    return out;
}

RunConfig from_rendezvous(const fs::path& root, std::int64_t now_unix) {
    RunConfig config;
    const fs::path active = root / "active.json";
    std::error_code ec;
    std::string body;
    if (root.empty() || !fs::exists(fs::symlink_status(active, ec))) {
        config.detail = "no rendezvous file";
        return config;
    }
    auto refuse = [&](std::string why) {
        config = RunConfig{};
        config.detail = "rendezvous refused: " + std::move(why);
        return config;
    };
    if (!read_regular_no_follow(active, body))
        return refuse("active.json is not a regular file (or is a symlink)");
    if (!well_formed(body))
        return refuse("active.json repeats a key or uses an escape sequence");
    if (json_string(body, "schema") != kRendezvousSchema)
        return refuse("schema is not " + std::string(kRendezvousSchema));
    const std::int64_t expires = json_int(body, "expires_unix");
    if (expires <= now_unix)
        return refuse("expired");
    if (expires > now_unix + kMaxRendezvousLifetimeSeconds)
        return refuse("expiry is more than " + std::to_string(kMaxRendezvousLifetimeSeconds) +
                      " s away");
    const std::string nonce = json_string(body, "nonce");
    if (nonce.size() < 16 || !is_hex(nonce))
        return refuse("nonce missing or not lowercase hex");
    const fs::path run_dir = json_string(body, "run_dir");
    const fs::path request = json_string(body, "request");
    const fs::path ir = json_string(body, "ir");
    if (no_follow_is_link(root) || no_follow_is_link(root / "runs") || no_follow_is_link(run_dir))
        return refuse("the root, runs/ and run_dir must not be symlinks");
    if (!strictly_inside(run_dir, root / "runs") || !directory_no_follow(run_dir))
        return refuse("run_dir is not a directory under " + (root / "runs").string());
    // Containment resolves symlinks, so a link from inside the run dir to
    // anywhere outside it is refused here too.
    if (!strictly_inside(request, run_dir) || !strictly_inside(ir, run_dir))
        return refuse("request and ir must resolve inside run_dir");
    config.run_dir = run_dir;
    config.request = request;
    config.ir = ir;
    config.nonce = nonce;
    config.source = "rendezvous";
    return config;
}

} // namespace

fs::path default_rendezvous_root() {
    if (const char* override_root = std::getenv("PULP_GPU_FAULT_RENDEZVOUS_ROOT"))
        return override_root;
    // getpwuid_r: hosts may construct several instances concurrently.
    passwd account{};
    passwd* found = nullptr;
    char buffer[4096];
    if (::getpwuid_r(::getuid(), &account, buffer, sizeof(buffer), &found) != 0 ||
        found == nullptr || account.pw_dir == nullptr)
        return {};
    return fs::path(account.pw_dir) / "Library" / "Application Support" / "Pulp" /
           "gpu-fault-continuity";
}

RunConfig resolve_run_config(const RunConfigSources& sources) {
    const auto env = [&](const char* name) -> fs::path {
        const char* value = sources.getenv ? sources.getenv(name) : nullptr;
        return value ? fs::path(value) : fs::path();
    };
    RunConfig config;
    config.run_dir = env("PULP_GPU_FAULT_RUN_DIR");
    if (!config.run_dir.empty()) {
        config.request = env("PULP_GPU_FAULT_REQUEST");
        config.ir = env("PULP_GPU_FAULT_IR");
        config.nonce = env("PULP_GPU_FAULT_NONCE").string();
        config.source = "env";
        return config;
    }
    return from_rendezvous(sources.rendezvous_root, sources.now_unix);
}

RunConfig resolve_run_config() {
    RunConfigSources sources;
    sources.getenv = [](const char* name) { return static_cast<const char*>(std::getenv(name)); };
    sources.rendezvous_root = default_rendezvous_root();
    sources.now_unix = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    return resolve_run_config(sources);
}

HostProcess current_host_process() {
    HostProcess process;
    process.pid = static_cast<int>(::getpid());
    process.ppid = static_cast<int>(::getppid());
    char path[PROC_PIDPATHINFO_MAXSIZE] = {};
    if (::proc_pidpath(process.pid, path, sizeof(path)) > 0)
        process.path = path;
    // Neither call has a public header; both are looked up so a missing
    // symbol degrades to "unknown" rather than a load failure inside a host.
    using ResponsibleFn = pid_t (*)(pid_t);
    if (auto fn = reinterpret_cast<ResponsibleFn>(
            ::dlsym(RTLD_DEFAULT, "responsibility_get_pid_responsible_for_pid")))
        process.responsible_pid = static_cast<int>(fn(process.pid));
    char responsible[PROC_PIDPATHINFO_MAXSIZE] = {};
    if (process.responsible_pid > 0 &&
        ::proc_pidpath(process.responsible_pid, responsible, sizeof(responsible)) > 0)
        process.responsible_path = responsible;
    proc_bsdinfo info{};
    if (::proc_pidinfo(process.pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) ==
        static_cast<int>(sizeof(info)))
        process.start_unix_us = static_cast<std::int64_t>(info.pbi_start_tvsec) * 1'000'000 +
                                static_cast<std::int64_t>(info.pbi_start_tvusec);
    using SandboxCheckFn = int (*)(pid_t, const char*, int, ...);
    if (auto fn = reinterpret_cast<SandboxCheckFn>(::dlsym(RTLD_DEFAULT, "sandbox_check")))
        process.sandboxed = fn(process.pid, nullptr, 0) != 0 ? 1 : 0;
    return process;
}

std::int64_t unix_now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string claim_record(const HostProcess& process) {
    return std::to_string(process.pid) + "\n" + std::to_string(process.claimed_unix_us) + "\n";
}

std::string host_process_event(const HostProcess& process, const RunConfig& config) {
    return "{\"event\":\"host_process\",\"pid\":" + std::to_string(process.pid) +
           ",\"ppid\":" + std::to_string(process.ppid) +
           ",\"responsible_pid\":" + std::to_string(process.responsible_pid) +
           ",\"responsible_path\":\"" + escape(process.responsible_path) + "\"" +
           ",\"sandboxed\":" + std::to_string(process.sandboxed) +
           ",\"start_unix_us\":" + std::to_string(process.start_unix_us) +
           ",\"claimed_unix_us\":" + std::to_string(process.claimed_unix_us) +
           ",\"process_path\":\"" + escape(process.path) + "\",\"config_source\":\"" +
           escape(config.source) + "\",\"nonce\":\"" + escape(config.nonce) + "\"}";
}

} // namespace pulp::examples::gpu_fault
