#!/usr/bin/env python3
"""The ThingsBoard session and the request body tb_import.py sends.

Split out of scripts/tb_import.py when the pair crossed the 1000-line gate,
along the seam the repository already uses for history_export.py /
history_store.py: the tool that DECIDES stays in one file, the thing that talks
to the far end lives in another.

Two halves, and they are here together because they are the same concern -- what
goes on the wire:

  Client      one authenticated session, retried on a throttle and
              re-authenticated once on a 401, counting every byte it sends.
  build_body  the `[{"ts": ms, "values": {...}}]` array, with every archived
              value re-emitted as the JSON literal ThingsBoard originally
              parsed. json_literal() is the whole of that decision and it is
              the one function here that can silently change a stored series'
              TYPE -- see tb_import.py's module docstring, and the checks in
              `tb_import.py --self-test`, which is what covers this file.

Standard library only, like every other tool in scripts/.
"""

from __future__ import annotations

import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

# Points per GET when counting. Measured on the self-hosted instance:
# limit=60000 returned 60000 of 60000, and limit=100000 returned the same 60000
# because that was all there was -- so there is no 50 000 threshold here, which
# ThingsBoard Cloud does have. The walk pages anyway. tb_export.py records what
# a trusted limit costs: a window holding 13 173 points answered with exactly
# 10 000 and no flag saying so, and an archive with a hole in it that reports
# success is the one unacceptable outcome.
READ_PAGE = 50000

# Datapoints per POST. Measured headroom is 200 000 in a 10.5 MB body, so this
# is not a ceiling -- it is the size at which a retry costs a second rather than
# a minute, and at which the progress line moves often enough to be believed.
WRITE_BATCH_POINTS = 20000

RETRY_STATUSES = (429, 502, 503, 504)
MAX_RETRIES = 6
RETRY_BASE_SEC = 2.0
RETRY_CAP_SEC = 60.0

# A JSON number, exactly as RFC 8259 draws it. Anything else -- including a
# leading `+`, a leading zero, a bare `.5`, `NaN` and `Infinity` -- is emitted
# as a quoted string rather than guessed at. The archive holds none of those
# today (checked: 0 rows), and the day it does, a quoted string is wrong in a
# way somebody can see instead of wrong in a way that parses.
JSON_NUMBER = re.compile(r"^-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?$")


class ApiError(Exception):
    """An HTTP status and the body the server sent, verbatim.

    Same rule as tb_export.py: a 403 on one endpoint and a 400 on a malformed
    range are different problems, and a message that flattens both into "it did
    not work" costs the only sentence that says which.
    """

    def __init__(self, method: str, path: str, status: int, body: bytes):
        self.status = status
        self.body = body.decode("utf-8", "replace")[:600]
        super().__init__("%s %s -> %d: %s" % (method, path, status, self.body))


class Fatal(Exception):
    """Something the operator has to fix. Printed as a sentence, not a trace."""


# --------------------------------------------------------------------------
# HTTP
# --------------------------------------------------------------------------

class Client:
    """One session against one ThingsBoard, counting everything it sends.

    The cost of a run is a number the operator is entitled to see, and a counter
    kept by the thing doing the requesting cannot disagree with it.
    """

    # NO PROXY, ever -- the lesson device_http.py records. urllib reads the
    # system proxy settings, and a proxy that answers a POST with its own 302
    # arrives as a JSON parse error and reads exactly like a broken server.
    _opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def __init__(self, base: str, key_file: Path):
        self.base = base.rstrip("/")
        self.key_file = key_file
        self._token = None
        self._cred = None
        self.requests = 0
        self.bytes_sent = 0
        self.bytes_read = 0
        self.retries = 0
        self.slept = 0.0

    # -- credential --------------------------------------------------------

    def credential(self) -> dict:
        if self._cred is None:
            path = Path(os.environ.get("TB_IMPORT_KEY_FILE") or self.key_file)
            try:
                text = path.read_text(encoding="utf-8").strip()
            except OSError as e:
                raise Fatal(
                    "cannot read the ThingsBoard credential from %s (%s).\n"
                    "Put {\"username\": ..., \"password\": ...} in that file, or "
                    "two lines holding the same two values, or point "
                    "TB_IMPORT_KEY_FILE at it. Do not pass it on a command line."
                    % (path, e.strerror or e))
            if not text:
                raise Fatal("%s is empty" % path)
            if text.lstrip().startswith("{"):
                data = json.loads(text)
                self._cred = {"username": data["username"],
                              "password": data["password"]}
            else:
                lines = [ln.strip() for ln in text.splitlines() if ln.strip()]
                if len(lines) < 2:
                    raise Fatal("%s should hold a JSON object with username and "
                                "password, or those two values on two lines"
                                % path)
                self._cred = {"username": lines[0], "password": lines[1]}
        return self._cred

    def login(self) -> str:
        status, body = self._raw("POST", "/api/auth/login",
                                 json.dumps(self.credential()).encode())
        if status != 200:
            raise ApiError("POST", "/api/auth/login", status, body)
        self._token = json.loads(body)["token"]
        return self._token

    def whoami(self) -> dict:
        return self.call("GET", "/api/auth/user")

    # -- transport ---------------------------------------------------------

    def _raw(self, method: str, path: str, data):
        req = urllib.request.Request(self.base + path, method=method, data=data)
        req.add_header("Accept", "application/json")
        if data is not None:
            req.add_header("Content-Type", "application/json")
        if self._token:
            req.add_header("X-Authorization", "Bearer " + self._token)
        try:
            with self._opener.open(req, timeout=600) as resp:
                body = resp.read()
                status = resp.status
        except urllib.error.HTTPError as e:
            body, status = e.read(), e.code
        except urllib.error.URLError as e:
            raise Fatal("cannot reach %s (%s)" % (self.base, e.reason))
        self.requests += 1
        self.bytes_sent += len(data or b"")
        self.bytes_read += len(body)
        return status, body

    def call(self, method: str, path: str, data: bytes = None):
        """One request, retried on a throttle, and re-authenticated on a 401.

        A bulk run outlives a JWT: ThingsBoard's default access-token life is
        well under the time 404 142 datapoints and a full verification take, and
        a 401 halfway through a migration is a resume nobody needed. A 401 is
        therefore ONE re-login and ONE retry -- never a loop, because a genuinely
        wrong password retried forever is a lockout wearing a progress bar.
        """
        relogged = False
        for attempt in range(MAX_RETRIES + 1):
            status, body = self._raw(method, path, data)
            if status in (200, 201):
                return json.loads(body) if body else None
            if status == 401 and not relogged:
                relogged = True
                self.login()
                continue
            if status not in RETRY_STATUSES or attempt == MAX_RETRIES:
                raise ApiError(method, path, status, body)
            delay = min(RETRY_BASE_SEC * (2 ** attempt), RETRY_CAP_SEC)
            self.retries += 1
            self.slept += delay
            # Logged every time. A run that quietly took ten minutes because it
            # was throttled is a different fact from one that took ten minutes
            # because there is a lot of data.
            print("tb_import: %d from the API, retry %d/%d in %.0fs"
                  % (status, attempt + 1, MAX_RETRIES, delay), file=sys.stderr)
            time.sleep(delay)

    # -- endpoints ---------------------------------------------------------

    def device_by_name(self, name: str) -> dict:
        page = self.call("GET", "/api/tenant/devices?pageSize=500&page=0")
        for d in page.get("data", []):
            if d.get("name") == name:
                return {"id": d["id"]["id"], "name": d["name"]}
        seen = ", ".join(sorted(d.get("name", "?") for d in page.get("data", [])))
        raise Fatal("no device named %r on %s; it has: %s"
                    % (name, self.base, seen))

    def _tel(self, dev: str, suffix: str) -> str:
        return "/api/plugins/telemetry/DEVICE/%s/%s" % (dev, suffix)

    def keys(self, dev: str) -> list:
        return self.call("GET", self._tel(dev, "keys/timeseries")) or []

    def series(self, dev: str, key: str, start: int, end: int,
               limit: int = READ_PAGE, strict: bool = False) -> list:
        query = urllib.parse.urlencode({
            "keys": key, "startTs": int(start), "endTs": int(end),
            "limit": int(limit), "agg": "NONE", "orderBy": "ASC",
            "useStrictDataTypes": "true" if strict else "false"})
        out = self.call("GET", self._tel(dev, "values/timeseries?" + query))
        return (out or {}).get(key, []) if isinstance(out, dict) else []

    def walk(self, dev: str, key: str, start: int, end: int, strict=False):
        """Every stored point for one key in [start, end), paged.

        A page exactly READ_PAGE long means the server truncated and there is
        more; the next window opens one millisecond past its last point. The
        walk refuses to stand still or go backwards rather than looping or
        skipping a stretch -- the same assertion tb_export.py makes going the
        other way.
        """
        cursor, seen, guard = int(start), [], 0
        while cursor < end:
            page = self.series(dev, key, cursor, end, READ_PAGE, strict)
            if not page:
                break
            last = page[-1]["ts"]
            if last < cursor:
                raise Fatal("%s: the server answered out of order at %d"
                            % (key, cursor))
            seen.extend(page)
            if len(page) < READ_PAGE:
                break
            cursor = last + 1
            guard += 1
            if guard > 1000:
                raise Fatal("%s: paging did not terminate" % key)
        return seen

    def attributes(self, dev: str, scope: str = None) -> list:
        path = "values/attributes" + ("/" + scope if scope else "")
        return self.call("GET", self._tel(dev, path)) or []



# --------------------------------------------------------------------------
# Emitting the body
# --------------------------------------------------------------------------

def json_literal(text: str) -> str:
    """The archived TEXT as the JSON literal ThingsBoard originally parsed.

    Verbatim for numbers, so the server repeats its own typing decision. See
    the module docstring: float(text) then repr() would shorten a 17-digit
    literal to 16 and turn a string series into a double series with nothing in
    the data to say where.
    """
    if text == "true" or text == "false":
        return text
    if JSON_NUMBER.match(text):
        return text
    return json.dumps(text)


def build_body(points) -> bytes:
    """`[{"ts": ms, "values": {...}}, ...]`, built by hand.

    json.dumps cannot be used for the whole structure: it would re-format every
    number through Python's float. So the keys and the string values go through
    json.dumps (which is what makes a quote or a backslash in `relayName` or
    `bootReason` safe) and the numeric literals do not.
    """
    chunks = []
    for ts, values in points:
        inner = ",".join("%s:%s" % (json.dumps(k), json_literal(v))
                         for k, v in values)
        chunks.append('{"ts":%d,"values":{%s}}' % (int(ts), inner))
    return ("[" + ",".join(chunks) + "]").encode("utf-8")


def group_by_ts(rows):
    """Rows ordered by ts collapsed into one entry per timestamp.

    A periodic payload wrote a dozen keys at one instant, so this is what turns
    404 142 datapoints into 23 792 entries and a 20 MB body into a manageable
    one. The archive's PRIMARY KEY (key, ts) is what guarantees no key repeats
    inside an entry.
    """
    out, current, values = [], None, []
    for ts, key, value in rows:
        if ts != current:
            if current is not None:
                out.append((current, values))
            current, values = ts, []
        values.append((key, value))
    if current is not None:
        out.append((current, values))
    return out


def batches(points, limit=WRITE_BATCH_POINTS):
    """Whole timestamps only, up to `limit` datapoints each.

    A timestamp is never split across two requests. Splitting would be SAFE --
    the write is an overwrite, measured -- but it makes the ledger below a
    statement about bytes instead of about instants, and a resume that has to
    reason about half a payload is a resume nobody will trust at 3 a.m.
    """
    batch, count = [], 0
    for ts, values in points:
        if batch and count + len(values) > limit:
            yield batch
            batch, count = [], 0
        batch.append((ts, values))
        count += len(values)
    if batch:
        yield batch


