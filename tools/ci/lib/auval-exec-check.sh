#!/usr/bin/env bash

# Did launchd fail to EXECUTE the AU validation worker at all?
#
# The worker has to run inside the user's GUI session, so it is started through
# a launchd agent. launchd may be unable to execute it — most commonly because
# the checkout lives somewhere launchd is not permitted to read (a worktree
# under /Volumes needs Full Disk Access for launchd). The directory stats fine
# from the shell, so the problem is invisible until the exec fails, and the only
# evidence is bash's message on the agent's stderr.
#
# That is not a validation result. Callers use this to report a SKIP instead of
# waiting out the status-file deadline and calling it a timeout.
#
# The predicate is deliberately narrow: the stderr must name the worker script
# AND carry an exec-failure message. A plug-in that genuinely fails validation
# writes a status file, which callers check first, so this cannot swallow a real
# failure.
auval_worker_exec_failed() {
    local stderr_log="$1"
    local script_path="$2"

    [[ -n "$stderr_log" && -n "$script_path" ]] || return 1
    [[ -s "$stderr_log" ]] || return 1
    grep -Fq -- "$script_path" "$stderr_log" 2>/dev/null || return 1
    grep -Eq 'Operation not permitted|cannot execute|No such file or directory' \
        -- "$stderr_log" 2>/dev/null
}
