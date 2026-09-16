# `src/tasks.cpp` is at 998 of 1000 lines

Two agents grew it independently — SHT40 added an `ambient` task, onboarding
added `tasksSetupOnboarding()` — and neither could see the other. The gate is
green by **two lines**, which means the next change to this file breaks the build
for a reason that has nothing to do with that change.

CLAUDE.md is explicit about why the gate prints the largest files on success:
*"the useful signal is the file three commits away from crossing."* This is zero
commits away.

**The split is not free.** `tasks.cpp` kept every `DECLARE_TASK` and every
handler through the last split **because the ordering inside `tasksSetup()` is
load-bearing** — register and enable before `g_criticalRunner.start()`, relay
safe-init before anything else. Whatever comes out must not be able to reorder
those.

**Done means:** a seam that leaves the registration order in one readable place,
six envs still building, and the file comfortably under — not at 990.
