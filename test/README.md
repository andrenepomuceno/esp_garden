# Host tests

Unit tests that run on the development machine — no board, no `data/config.json`.

```bash
pio test -e native                      # every suite
pio test -e native -f test_accumulator  # one suite
```

`native` is the **environment**; `test_accumulator` is a **filter** matching the
directory name. `pio test -e test_accumulator` fails — there is no such
environment.

## Adding a suite

1. Create `test/test_<name>/` with a `runner.cpp` that calls your
   `run_<name>_tests()` from `main()`.
2. Add the production `.cpp` to `build_src_filter` in `[env:native]`.
   **It is an allow-list**: without a `+<...>` line the code is simply not
   compiled and the suite fails to link with "undefined reference".
   A header-only unit needs no entry — `test_pin_rules` includes
   `core/pin_rules.h` and adds nothing to the filter.

## What can be tested here, and what cannot

`[env:native]` has no Arduino core, so only code that compiles standalone is in
scope. Today that is `AccumulatorV2`, `core/segment_index.h`,
`core/probe_health.h`, `core/step_publisher.h`, `core/moisture_classifier.h`,
`core/fw_version.h`, `core/cloud_cover.h` and `core/pin_rules.h`. `segment_index.h` exists as a
separate header precisely so it can be tested: the ring arithmetic behind
`/history.json` reorders records rather than failing when it is wrong, and the
rest of `IoHistory` is inseparable from LittleFS. `cloud_cover.cpp` is split
from `cloud_model.cpp` for the same reason the moisture classifier is split
from its trainer — a wrong clearness index reports the wrong sky rather than
crashing, and `test_cloud_cover` builds its own reference table so a suite does
not start failing whenever the weather does.

`fullbot-firmware` covers the rest with a stub layer in
`test/support/native_includes/` (in-memory LittleFS/SD, a `JSONVar`
re-implementation, FreeRTOS no-ops), and that layer transplants here nearly
as-is when a suite needs it.

**One caveat worth knowing before transplanting the JSON stub.** The config
editor's worst bug so far came from `Arduino_JSON::operator[]` returning **by
value**: casting a chained subscript to `const char*` reads a buffer the
temporary already freed and yields an empty string. A hand-written `JSONVar`
stub that returns a reference does not reproduce that, so a test written against
it would pass while the device fails. Logic that depends on those semantics has
to be verified on hardware — see the byte-count check described in `CLAUDE.md`.

## Writing a regression test

Assert the behaviour the bug violated, then confirm the test actually catches it
by reverting the fix and watching that one test — and only that one — fail. Both
`setMaxLen` cases here were checked that way.
