// Signal-aware shellouts for the `pulp dev` / `pulp loop` watch loop.
#pragma once

#include <string>

// Handlers only record the stop request. Child cancellation, joining and lease
// retirement run in ordinary control flow after the watch observes it.
class WatchSignalScope {
  public:
    WatchSignalScope();
    ~WatchSignalScope();
    WatchSignalScope(const WatchSignalScope&) = delete;
    WatchSignalScope& operator=(const WatchSignalScope&) = delete;
    bool ok() const;

  private:
    using Handler = void (*)(int);
    Handler previous_interrupt_;
    Handler previous_terminate_;
};

// The SIGINT/SIGTERM number recorded since the scope was installed, or 0.
int requested_watch_stop() noexcept;

// Run a shell command as an owned, cancellable child. std::system ignores
// SIGINT process-wide while it waits, so watch shellouts use this instead.
// Returns 128 + signal when a stop was requested.
int run_watch_command(const std::string& command, const std::string& label = {});
