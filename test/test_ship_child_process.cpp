// The ship scan's child-process helper: a tool it starts gets no terminal and
// a clean signal state, whatever mask and dispositions the caller holds.
#include "tools/cli/ship_child_process.hpp"

#include <catch2/catch_test_macros.hpp>

#ifndef _WIN32
#include <csignal>
#include <sys/wait.h>

namespace {

// Runs `script` under /bin/sh through spawn_quiet and returns the raw status.
int run_shell(const char* script) {
    char sh[] = "sh", c[] = "-c";
    char* argv[] = {sh, c, const_cast<char*>(script), nullptr};
    const pid_t pid = pulp::cli::spawn_quiet("/bin/sh", argv);
    REQUIRE(pid > 0);
    const int status = pulp::cli::wait_status(pid);
    REQUIRE(status != -1);
    return status;
}

} // namespace

TEST_CASE("a ship-scan tool starts with the scan signals unblocked", "[cli][ship][signals]") {
    // The scan holds these blocked while it records a child's pid, and the
    // signal handler runs with them blocked; the child must not inherit that.
    sigset_t block, previous;
    sigemptyset(&block);
    for (const int sig : pulp::cli::kShipScanSignals)
        sigaddset(&block, sig);
    REQUIRE(pthread_sigmask(SIG_BLOCK, &block, &previous) == 0);
    const int status = run_shell("kill -INT $$; exit 0");
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);

    // A blocked SIGINT would stay pending and the shell would exit 0.
    REQUIRE(WIFSIGNALED(status));
    REQUIRE(WTERMSIG(status) == SIGINT);
}

TEST_CASE("a ship-scan tool starts with the default action for an ignored signal",
          "[cli][ship][signals]") {
    // An ignored disposition survives exec; a caller run under nohup, or one
    // that ignores SIGTERM, must not hand that on to ditto or hdiutil.
    struct sigaction ignore = {}, previous = {};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    REQUIRE(sigaction(SIGTERM, &ignore, &previous) == 0);
    const int status = run_shell("kill -TERM $$; exit 0");
    sigaction(SIGTERM, &previous, nullptr);

    REQUIRE(WIFSIGNALED(status));
    REQUIRE(WTERMSIG(status) == SIGTERM);
}

TEST_CASE("a ship-scan tool reads end of file from stdin", "[cli][ship][signals]") {
    // stdin is /dev/null, so a prompt reads end of file at once instead of
    // waiting on a terminal.
    const int status = run_shell("read line && exit 3; exit 5");
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 5);
}
#endif
