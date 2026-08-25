#!/usr/bin/env python3
"""Builds the filesystem image's contents from `data/`, bundled and gzipped.

WHY THIS EXISTS

`devices.html` stopped loading. Not a corrupt file — served one at a time every
asset came back byte-identical — but under the six parallel requests a browser
makes for a page, the largest contiguous free heap block collapses from ~45 KB
to as little as 1 KB and ESPAsyncWebServer truncates whichever response it
happens to be filling. The victim changed on every reload, which is what a
resource ceiling looks like from the outside.

LittleFS is what pushed it over. Every open file carries a 4 KB cache where
SPIFFS used 256 B pages, so six concurrent file responses now cost what six
never used to. Gzipping alone cut the failures roughly in half and did not fix
them, because the pressure scales with the NUMBER of open files, not their size.

So: fewer requests. Each page's own scripts become one file, and everything the
web server hands out is compressed.

WHAT IT DOES

  data/  ->  .pio/assets/          (never in place; `data/` stays pristine)

- The scripts a page loads are read FROM THE HTML, in order, and concatenated
  into the LAST one's name. Deriving the order from the markup rather than a
  manifest here is what stops the two drifting apart, and reusing the last
  name means the route table does not change: /devices.js is already a route,
  and it now carries auth + model + render + devices.
- A script that exists only as `<name>.gz` in data/ is vendored (jQuery,
  sha256, SparkMD5, Bootstrap). Those stay separate files: they are shared
  across nine pages, and bundling one into each would cost 30 KB of flash per
  copy.
- `.html`, `.js`, `.css` and `.ico` are gzipped, and the plain twin is NOT
  emitted. AsyncFileResponse prefers an uncompressed file when both exist, so
  shipping both would waste the flash and serve the big one anyway. The upload
  handler enforces the same rule on a live device: writing one deletes the
  other.
- **`.json`, `.pem`, `.txt` and `.bin` are copied verbatim, never compressed.**
  The firmware opens those itself — ConfigFile reads /config.json, the MQTT
  client reads /thingspeak.pem — through FILESYSTEM.open(), which has no gzip
  fallback. Compressing one of them bricks the device on the next boot.

Anything it cannot do exactly right, it REFUSES. A page naming a script that is
not there, two pages whose bundles would collide, an output written twice from
different sources: each of those is a page that ships dead while the build
prints success, so each one stops the build with the page named.

The sources stay plain in `data/`: diffable in git, editable, counted by
check_lines.py, and served as-is by scripts/dev_server.py so the simulator
needs no build step.

Run:  python scripts/build_assets.py            (into .pio/assets)
      python scripts/build_assets.py --out DIR
      python scripts/build_assets.py --list     (report, write nothing)
"""

from __future__ import annotations

import argparse
import gzip
import io
import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "data"

# Compressed on the way out. Everything else is copied byte for byte, because
# something other than the web server may be reading it.
COMPRESS = {".html", ".js", ".css", ".ico"}

SCRIPT_TAG = re.compile(
    r"[ \t]*<script[^>]*\ssrc=[\"']([^\"']+)[\"'][^>]*>\s*</script>[ \t]*\n?",
    re.IGNORECASE)


class BuildError(Exception):
    pass


def squeeze(raw: bytes) -> bytes:
    # mtime=0 so identical input gives identical output: an asset that did not
    # change must not look changed to anything comparing hashes.
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", compresslevel=9, mtime=0) as f:
        f.write(raw)
    return buf.getvalue()


def is_vendored(name: str) -> bool:
    """Shipped pre-compressed and shared between pages, so never bundled."""
    return not (DATA / name).is_file() and (DATA / (name + ".gz")).is_file()


def plan_page(html: Path):
    """Returns (rewritten text, bundle name, [sources]) for one page.

    The rewrite works on the regex's own match SPANS rather than str.replace,
    so a page that lists the same src twice, or carries a commented-out script
    tag earlier in the file, cannot have the wrong occurrence rewritten.
    """
    text = html.read_text(encoding="utf-8")
    matches = list(SCRIPT_TAG.finditer(text))

    def local_name(src: str) -> str | None:
        if src.startswith(("http://", "https://", "//")):
            return None
        name = src.lstrip("/")
        return None if is_vendored(name) else name

    locals_ = [(m, local_name(m.group(1))) for m in matches]
    named = [(m, n) for m, n in locals_ if n is not None]
    if len(named) < 2:
        return text, None, []

    for m, name in named:
        if not (DATA / name).is_file():
            raise BuildError(
                "%s loads <script src=\"%s\">, and data/%s does not exist"
                % (html.name, m.group(1), name))

    sources = [n for _, n in named]
    bundle = sources[-1]

    out = []
    cursor = 0
    first = True
    for m, _ in named:
        out.append(text[cursor:m.start()])
        if first:
            tag = m.group(0)
            indent = tag[:len(tag) - len(tag.lstrip(" \t"))]
            out.append('%s<script src="%s"></script>\n' % (indent, bundle))
            first = False
        cursor = m.end()
    out.append(text[cursor:])

    return "".join(out), bundle, sources


def prepare_out_dir(out_dir: Path, expected: set) -> None:
    """Erases the staging directory, but only if we recognise everything in it.

    `--out` deletes its target before rebuilding, and deleting a directory
    somebody named by hand is how you lose data/config.json: gitignored, holding
    a real device's Wi-Fi and broker credentials, and unrecoverable from a
    masked GET. So the contents have to be entirely ours — every entry a regular
    file this build would write anyway. `--out data` then refuses on the first
    plain source it finds, `--out .` on the first subdirectory.

    A marker file would be simpler and was tried; it ends up inside the image,
    where LittleFS charges a whole 4 KB block for it.
    """
    if not out_dir.exists():
        out_dir.mkdir(parents=True)
        return

    if not out_dir.is_dir():
        raise BuildError("%s is not a directory" % out_dir)

    for entry in out_dir.iterdir():
        if not entry.is_file() or entry.name not in expected:
            raise BuildError(
                "%s holds %s, which this build does not produce. Refusing to "
                "erase a directory that is not its own output; point --out at "
                "an empty one." % (out_dir, entry.name))

    shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)


def collect():
    """Reads every page and returns (bundles, rewritten, shared)."""
    bundles: dict[str, list[str]] = {}
    owner: dict[str, str] = {}
    rewritten: dict[str, str] = {}

    for page in sorted(DATA.glob("*.html")):
        text, bundle, sources = plan_page(page)
        if bundle is None:
            continue
        if bundle in bundles and bundles[bundle] != sources:
            raise BuildError(
                "%s and %s would both write %s, from different sources (%s vs "
                "%s). Rename one page's last script."
                % (owner[bundle], page.name, bundle,
                   " + ".join(bundles[bundle]), " + ".join(sources)))
        bundles[bundle] = sources
        owner[bundle] = page.name
        rewritten[page.name] = text

    # A source in more than one bundle is emitted standalone as well. auth.js
    # is in all nine, so a browser holding cached markup still asks for it by
    # name — and a 404 there takes out the login page.
    counts: dict[str, int] = {}
    for sources in bundles.values():
        for s in sources:
            counts[s] = counts.get(s, 0) + 1
    shared = {s for s, c in counts.items() if c > 1}

    return bundles, rewritten, shared, set(counts)


def build(out_dir: Path, report_only: bool) -> int:
    if not DATA.is_dir():
        raise BuildError("no data/ directory")

    bundles, rewritten, shared, bundled = collect()

    # Everything is planned before anything is written, so the containment
    # check below knows the full output set and a refusal costs no half-built
    # directory.
    produced: dict[str, str] = {}  # output name -> source that wrote it
    plan: list = []                # (target, raw length, blob)

    for src in sorted(DATA.iterdir()):
        if not src.is_file():
            continue
        name = src.name

        # Emitted as part of a bundle rather than on its own, unless shared.
        if name in bundled and name not in bundles and name not in shared:
            continue

        if name in bundles:
            raw = b"".join(
                (DATA / s).read_bytes().rstrip() + b"\n;\n"
                for s in bundles[name])
        elif name in rewritten:
            raw = rewritten[name].encode("utf-8")
        else:
            raw = src.read_bytes()

        compress = src.suffix in COMPRESS and not name.endswith(".gz")
        blob = squeeze(raw) if compress else raw
        target = name + ".gz" if compress else name

        if target in produced:
            raise BuildError(
                "%s and %s both produce %s; one would silently overwrite "
                "the other" % (produced[target], name, target))
        produced[target] = name
        plan.append((target, len(raw), blob, name))

    if report_only:
        for target, rawlen, blob, name in plan:
            note = "  <- " + " + ".join(bundles[name]) if name in bundles else ""
            print("  %-24s %7d -> %6d B%s" % (target, rawlen, len(blob), note))
    else:
        prepare_out_dir(out_dir, set(produced))
        for target, _, blob, _ in plan:
            (out_dir / target).write_bytes(blob)
    written = len(plan)

    # Per PAGE, which is the number that matters: it is what the browser opens
    # in parallel. Reporting the total across bundles overstates it ninefold.
    worst = max((len(s) - 1 for s in bundles.values()), default=0)
    total = sum(len(s) - 1 for s in bundles.values())

    try:
        where = out_dir.relative_to(ROOT).as_posix()
    except ValueError:
        # An out-of-tree --out is legitimate; it must not turn a build that
        # fully succeeded into a traceback and a non-zero exit.
        where = str(out_dir)

    print("build_assets: %s %d files to %s"
          % ("would write" if report_only else "wrote", written, where))
    print("build_assets: %d bundle(s); up to %d fewer requests per page load, "
          "%d in total" % (len(bundles), worst, total))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default=str(ROOT / ".pio" / "assets"))
    ap.add_argument("--list", action="store_true",
                    help="report what would be built and write nothing")
    args = ap.parse_args()
    try:
        return build(Path(args.out).resolve(), args.list)
    except BuildError as e:
        print("build_assets: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
