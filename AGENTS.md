# AGENTS.md

## Git workflow

- **Always commit changes when you complete a requested task.** Do not leave
  finished work uncommitted, and do not wait to be asked.
- Before committing, inspect `git status` and `git diff`; stage only the files
  that belong to the completed task, and never commit secrets or build output.
- Write conventional-commit messages that match the existing repo style, e.g.
  `feat(plugin): ...`, `fix(build): ...`, `docs: ...`. Keep the subject concise
  and add a short body explaining the change when it is non-trivial.
- Never force-push, amend pushed commits, or use interactive rebase unless
  explicitly requested.
- If a commit fails (e.g. a hook rejects it), fix the issue and create a new
  commit rather than amending the failed one.
