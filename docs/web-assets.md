# Web assets — bundled and gzipped, because requests are the scarce resource

`devices.html` stopped loading with `ERR_CONTENT_LENGTH_MISMATCH`. It was not a corrupt file: **served one at a time, every asset came back byte-identical.** Under the six parallel requests a browser makes for one page, the largest contiguous free heap block collapsed from ~45 KB to as little as 1 KB and ESPAsyncWebServer truncated whichever response it was filling — so the victim changed on every reload, which is what a resource ceiling looks like from outside. LittleFS is what pushed it over: every open file carries a 4 KB cache where SPIFFS used 256 B pages, so the 2.2.1 load test (measured on SPIFFS) does not carry over. **Gzip alone was not enough.** Compressing the five assets cut them ~70 % and took the page from failing every load to failing one in three, because the pressure scales with the NUMBER of open files, not their size. What fixed it was going from 7 requests to 4.

`scripts/build_assets.py` produces `.pio/assets/` from `data/`:

- **The scripts a page loads are read from its HTML, in order**, and concatenated into the LAST one's name. Deriving the order from the markup rather than a manifest stops the two drifting apart, and reusing the last name means **the route table does not change**.
- **A script that exists only as `<name>.gz` in `data/` is vendored** (jQuery, sha256, SparkMD5, Bootstrap) and stays a separate file: shared across nine pages, so bundling one into each would cost 30 KB of flash per copy.
- **A script shared by several pages is also emitted standalone.** `auth.js` is in all nine bundles, so a browser holding cached markup still asks for it by name — and a 404 there takes out the login page.
- **`.json`, `.pem`, `.txt` and `.bin` are copied verbatim, never compressed.** The firmware opens those through `FILESYSTEM.open()`, which has no gzip fallback. Compressing `/config.json` or `/thingspeak.pem` bricks the device at the next boot.
- Measured: 250 KB of sources become **143 KB** on flash, every page dropping from 6–7 requests to 4.

`data/` stays plain: diffable in git, counted by `check_lines.py`, and served as-is by `dev_server.py`, so the simulator needs no build step and shows the unbundled files a developer is editing.

**The upload guard had to become exact.** `uploadPathIsProtected()` was `startsWith("/config")` and friends, which also refused `/config.html`, `/config.js`, `/users.html` and `/users.js` — ordinary web assets. It surfaced the moment assets started being deployed one at a time: four files answered 400 for no reason but their name, and the only remaining way to update them was the whole-image path that wipes `/config.json`. **A guard that pushes you toward the more destructive tool is worse than the one it replaced.** The `/spiffs` BROWSE shadows stay prefix matches deliberately — refusing to read `/spiffs/config.html.gz` costs nothing, since the same bytes are served from its own public route.
