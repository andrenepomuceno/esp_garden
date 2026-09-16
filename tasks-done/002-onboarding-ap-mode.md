# Onboarding: an unconfigured board raises its own AP

**In flight** — agent, isolated worktree, based on `bac7ad4`.

Operator's ask: first power-up with no usable config raises `espgarden-<id>` /
`espgarden`, the user connects, sets the fundamentals and loads a board template;
everything else is configured normally once on the LAN.

This also retires the field that bricks boards: the firmware knows its own
`ESP.getEfuseMac() % 0x10000`, so the written `id` cannot be wrong.

**Done means:** AP mode is a separate route table decided once in `webSetup()`;
`tasksSetup()` does not block 120 s with nobody to ping; the POST refuses before
writing, naming the check, rather than after a reboot; templates are filtered by
chip family; and a provisioning marker keeps a board that has ever reached the
network structurally outside the fallback rule, so a router outage can never put
the live garden on an AP with a published password.

**Known cost, stated not hidden:** an unauthenticated config write on a
fixed-password AP. Anyone in radio range during the onboarding window can claim
the board.

---
**Done** — `eb897c7`, merged as `28a830a`. FW_VERSION 2.15.0.

Marker is `/provisioned.pending`; `onboarding::decide()` is the only thing in the
tree that can return `Mode::Portal`, host-tested as a truth table over all eight
inputs. A configured board does not even spend the probation.

**It found a real defect on the way:** `loadFile()` returns early before parsing
`ota`, leaving the constructor's `"admin"`/`"password"` in place, and `main.cpp`
handed them to `UserStore::load()` anyway — so a board with no config and no
users seeded a real ADMIN account with a compiled password. Confirmed by reading
`config.cpp:82` against the early returns at 324/330/339/347. Harmless only
because such a board could not associate; onboarding removes exactly that.
