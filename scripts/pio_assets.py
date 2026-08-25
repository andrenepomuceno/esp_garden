"""PlatformIO pre-hook: regenerate .pio/assets before anything packs it.

`data_dir` points the filesystem image at .pio/assets rather than data/, so the
device gets the bundled, gzipped build while data/ keeps the plain sources.
That only works if the staging directory is current, and "remember to run the
script first" is not a mechanism — forgetting it ships a stale image, or an
empty one, and the failure looks like a device that lost its web UI.

So it runs here, on every invocation. It is a few milliseconds of gzip.
"""

import subprocess
import sys
from pathlib import Path

Import("env")  # noqa: F821  (injected by SCons)

ROOT = Path(env.subst("$PROJECT_DIR"))  # noqa: F821
BUILDER = ROOT / "scripts" / "build_assets.py"

result = subprocess.run([sys.executable, str(BUILDER)],
                        cwd=str(ROOT), capture_output=True, text=True)
if result.returncode != 0:
    print(result.stdout)
    print(result.stderr, file=sys.stderr)
    raise SystemExit("build_assets.py failed; refusing to pack a stale image")
print(result.stdout.strip())
