# Integrate the three parallel streams

**Mine.** Blocked on 002 and 003 reporting.

`main` is at `061eb3f` (FW_VERSION 2.14.0). Both worktrees branched from
`bac7ad4`, so both need rebasing, and 002 will have bumped FW_VERSION to a number
that now collides.

All three touch `CLAUDE.md`, `README.md` and `scripts/dev_server.py`, so the
merge is manual rather than a fast-forward.

**Done means:** one linear history on `main`, one FW_VERSION, gates re-run on the
merged tree (not on any branch in isolation) — `check_lines`, `pio test -e
native`, six envs — and CLAUDE.md's Unverified list carrying all three entries
without either agent's wording contradicting the other's.

Also fold in here: the one-line convention note for `tasks-inbox/` in CLAUDE.md,
deliberately deferred to avoid a three-way conflict on a file two agents are
editing.
