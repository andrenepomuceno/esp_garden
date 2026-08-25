"""PlatformIO pre-hook: regenerate the staging directory before it is packed.

`data_dir` points the filesystem image away from data/, so the device gets the
bundled, gzipped build while data/ keeps the plain sources. That only works if
the staging directory is current, and "remember to run the script first" is not
a mechanism: forgetting it ships a stale image, or an empty one, and the failure
looks like a device that lost its web UI.

The output path comes from $PROJECT_DATA_DIR rather than from build_assets.py's
own default, so the location is declared once — in platformio.ini — instead of
in two places that can drift into packing one directory while writing another.

It runs ONLY for the filesystem targets. Sharing [env] means every environment
inherits this hook, including [env:native], and `pio test -e native` is the gate
CI runs before the firmware matrix: it needs no board, no data/config.json and
no gzip, and a build_assets failure there would fail the unit tests for a reason
that has nothing to do with them. `-t clean` would also have rebuilt the very
directory it was asked to remove.
"""

import subprocess
import sys
from pathlib import Path

Import("env")  # noqa: F821  (injected by SCons)

FS_TARGETS = {"buildfs", "uploadfs", "uploadfsota"}

if FS_TARGETS.intersection(COMMAND_LINE_TARGETS):  # noqa: F821
    root = Path(env.subst("$PROJECT_DIR"))  # noqa: F821
    builder = root / "scripts" / "build_assets.py"
    out = env.subst("$PROJECT_DATA_DIR")  # noqa: F821

    result = subprocess.run(
        [sys.executable, str(builder), "--out", out],
        cwd=str(root), capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout)
        print(result.stderr, file=sys.stderr)
        raise SystemExit("build_assets.py failed; refusing to pack a stale image")
    print(result.stdout.strip())
