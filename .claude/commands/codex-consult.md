---
name: codex-consult
description: Consult OpenAI Codex (`codex exec`) for a second opinion on a prompt. Handles the argv-hang, stdin-mode, reasoning-effort, and timeout gotchas so the invocation always works. Accepts `$ARGUMENTS` as the prompt, or reads from a provided prompt file.
---

# Codex Consult

A Pulp-side wrapper around `codex exec` that always invokes it the right way. Use this instead of calling `codex exec` ad hoc — the direct CLI silently hangs on anything past a short argv prompt.

## When to use

- You want a second opinion on a plan, review, or design choice
- You need to sanity-check assumptions about a tool/API before committing to a direction
- You want Codex to explore the repo independently (it can use the RepoPrompt MCP server from inside its session)

See `~/.claude/skills/codex/SKILL.md` for the full flag reference and gotchas.

## How to invoke

Agents: build the prompt in a file, then run the invocation below. **Never pass a long prompt as a single argv argument to `codex exec` — it hangs silently with 0% CPU.**

```bash
# 1. Write the prompt to a temp file (heredoc into `cat > /tmp/codex-prompt.txt <<'EOF' … EOF`).
# 2. Invoke:
cat /tmp/codex-prompt.txt | timeout 480 \
    codex exec -c model_reasoning_effort='"medium"' - \
    > /tmp/codex-answer.txt 2> /tmp/codex-progress.log

# 3. Read the answer:
cat /tmp/codex-answer.txt
```

Tune the reasoning effort down (`"low"`) for faster answers on simple questions, up (`"high"`, `"xhigh"`) for deep analysis — but expect multi-minute runs at xhigh.

## Prompt template

Include this framing in every Codex prompt so it knows to use RepoPrompt instead of raw shell, and knows the repo state:

```
Tooling preference: use the RepoPrompt MCP server's tools (file_search,
get_file_tree, read_file, get_code_structure) rather than raw shell.
CWD is /Users/danielraffel/Code/pulp on branch <BRANCH> at commit <SHA>.

<your question here>

Please give direct yes/no answers where possible and flag any step in
the plan that is wrong or risky.
```

## Detecting a hung run

If the background invocation isn't returning:

```bash
ps -p <codex_pid> -o pid,state,time,%cpu,rss
lsof -p <codex_pid> 2>/dev/null | grep TCP | head -3
tail -20 /tmp/codex-progress.log
```

- **Thinking**: CPU-seconds accumulating, at least one TCP connection, progress log grows with `mcp: …started / …completed` lines.
- **Wedged**: 0% CPU, no TCP, progress log idle. Kill and retry with a shorter prompt / lower reasoning effort.

## Don't do

- `codex exec "long prompt"` as a single argv argument — **always** pipe via stdin (`codex exec -`).
- Run without `timeout` — a hung codex is indistinguishable from a slow one.
- Skip the RepoPrompt framing — Codex defaults to shell when no preference is given, which is slower and noisier on Pulp's tree.
- Run concurrent Codex sessions editing the same file.
