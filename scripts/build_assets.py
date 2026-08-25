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
  sha256, SparkMD5). Those stay separate files: they are shared across nine
  pages, and bundling one into each would cost 30 KB of flash per copy.
- `.html`, `.js`, `.css` and `.ico` are gzipped, and the plain twin is NOT
  emitted. AsyncFileResponse prefers an uncompressed file when both exist, so
  shipping both would waste the flash and serve the big one anyway.
- **`.json`, `.pem`, `.txt` and `.bin` are copied verbatim, never compressed.**
  The firmware opens those itself — ConfigFile reads /config.json, the MQTT
  client reads /thingspeak.pem — through FILESYSTEM.open(), which has no gzip
  fallback. Compressing one of them bricks the device on the next boot.

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
    """Returns (rewritten html text, bundle name, [sources]) for one page."""
    text = html.read_text(encoding="utf-8")
    tags = [(m.group(0), m.group(1)) for m in SCRIPT_TAG.finditer(text)]

    local = [src for _, src in tags
             if not src.startswith(("http://", "https://", "//"))
             and not is_vendored(src.lstrip("/"))]
    if len(local) < 2:
        return text, None, []

    bundle = local[-1]
    first_local_tag = next(tag for tag, src in tags if src == local[0])

    out = text
    for tag, src in tags:
        if src not in local:
            continue
        replacement = ('<script src="%s"></script>\n' % bundle
                       if tag == first_local_tag else "")
        # Preserve the leading indentation of the tag being replaced.
        if replacement:
            indent = tag[:len(tag) - len(tag.lstrip(" \t"))]
            replacement = indent + replacement
        out = out.replace(tag, replacement, 1)

    return out, bundle, [s.lstrip("/") for s in local]


def build(out_dir: Path, report_only: bool) -> int:
    if not DATA.is_dir():
        print("build_assets: no data/ directory", file=sys.stderr)
        return 1

    pages = sorted(DATA.glob("*.html"))
    bundled_into: dict[str, str] = {}   # source name -> bundle name
    bundles: dict[str, list[str]] = {}  # bundle name -> ordered sources
    rewritten: dict[str, str] = {}      # html name -> new text

    for page in pages:
        text, bundle, sources = plan_page(page)
        if bundle is None:
            continue
        rewritten[page.name] = text
        bundles.setdefault(bundle, sources)
        for s in sources:
            # A script may appear on several pages (auth.js does, on all of
            # them). It is copied into each bundle; at ~1 KB compressed that is
            # cheaper than the request it saves.
            bundled_into.setdefault(s, bundle)

    if not report_only:
        if out_dir.exists():
            shutil.rmtree(out_dir)
        out_dir.mkdir(parents=True)

    written = 0
    saved_requests = 0
    for page, sources in bundles.items():
        saved_requests += len(sources) - 1

    for src in sorted(DATA.iterdir()):
        if not src.is_file():
            continue
        name = src.name

        # Emitted as part of a bundle rather than on its own — unless it is
        # SHARED. auth.js is in all nine bundles, so a browser holding a cached
        # copy of the old markup still asks for it by name, and a 404 there
        # takes out the login page. One extra kilobyte buys away that window.
        shared = sum(1 for srcs in bundles.values() if name in srcs) > 1
        if name in bundled_into and name not in bundles and not shared:
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

        if report_only:
            note = ""
            if name in bundles:
                note = "  <- " + " + ".join(bundles[name])
            print("  %-24s %7d -> %6d B%s" % (target, len(raw), len(blob), note))
        else:
            (out_dir / target).write_bytes(blob)
        written += 1

    where = "would write" if report_only else "wrote"
    print("build_assets: %s %d files to %s"
          % (where, written, out_dir.relative_to(ROOT).as_posix()))
    print("build_assets: %d fewer requests per page load across %d bundle(s)"
          % (saved_requests, len(bundles)))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default=str(ROOT / ".pio" / "assets"))
    ap.add_argument("--list", action="store_true",
                    help="report what would be built and write nothing")
    args = ap.parse_args()
    return build(Path(args.out), args.list)


if __name__ == "__main__":
    sys.exit(main())
