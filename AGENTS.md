# AGENTS.md

## Git workflow

- **Always commit changes when you complete a requested task.** Do not leave
  finished work uncommitted, and do not wait to be asked.
- Before committing, inspect `git status` and `git diff`; stage only the files
  that belong to the completed task, and never commit secrets or build output.
- Write **detailed** commit messages: a conventional-commit subject (matching
  the repo style, e.g. `feat(plugin): ...`, `fix(build): ...`, `docs: ...`,
  `build(headers): ...`) followed by a descriptive body that explains the what
  and why, and lists the concrete changes. Terse, one-line commit messages are
  reserved for the user to write; the agent must always write detailed ones.
- Never force-push, amend pushed commits, or use interactive rebase unless
  explicitly requested.
- If a commit fails (e.g. a hook rejects it), fix the issue and create a new
  commit rather than amending the failed one.
