#!/usr/bin/env python3
"""The device's nonce + SHA-256 login, in one place.

Lifted out of scripts/moisture_fit.py, unchanged, when scripts/history_export.py
needed the same client. Two copies of an authentication handshake is two places
to update when `/nonce` changes shape, and the second copy is the one that keeps
working against a firmware nobody has run it on for months.

There is NO Basic-Auth path on the device. `curl -u user:pass` returns 401 on
every guarded route; src/custom_login.cpp implements:

    GET  /nonce?username=<u>  ->  {nonce, salt, ttlMs}
    passwordHash = sha256hex(salt + ":" + password)
    response     = sha256hex(nonce + ":" + passwordHash)
    POST /login  username, nonce, response [, remember=true]  ->  {token, role}
    every later request carries  Authorization-Token: <token>

BUDGET, NOT ETIQUETTE. The board serves HTTP from a single `async_tcp` task, so
anything polled while it works competes with the browser for it, and every login
takes one of `kMaxSessions` slots (8 since firmware 2.11.0, 4 before it). A tool
that logs in per request churns those slots and signs an operator out. So: ONE
login per run, requests strictly sequential, and a logout at the end.
"""

from __future__ import annotations

import hashlib
import json
import urllib.parse
import urllib.request


class Device:
    """One session against one device. Read-only unless a caller POSTs."""

    # NO PROXY, ever. urllib reads the system proxy settings, and a garden on
    # 192.168.1.55 or a simulator on 127.0.0.1 has no business going through
    # one: found here when a Windows proxy answered `GET /nonce` with a 302 to
    # `/`, which arrives as "Expecting value: line 1 column 1" from json.loads
    # and reads exactly like a device that stopped speaking JSON.
    _opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def __init__(self, host, timeout=15.0):
        self.base = f"http://{host}"
        self.timeout = timeout
        self.token = None
        # Every GET this session has issued. The cost of a run is a number the
        # operator is entitled to see BEFORE pointing it at a live garden, and a
        # counter kept by the thing doing the requesting cannot disagree with it.
        self.requests = 0
        self.bytes_read = 0

    def _request(self, path, data=None):
        request = urllib.request.Request(self.base + path, data=data)
        if self.token:
            request.add_header("Authorization-Token", self.token)
        with self._opener.open(request, timeout=self.timeout) as response:
            body = response.read()
        self.requests += 1
        self.bytes_read += len(body)
        return body.decode("utf-8")

    def login(self, username, password):
        challenge = json.loads(
            self._request("/nonce?username=" + urllib.parse.quote(username))
        )
        password_hash = hashlib.sha256(
            f"{challenge['salt']}:{password}".encode()
        ).hexdigest()
        answer = hashlib.sha256(
            f"{challenge['nonce']}:{password_hash}".encode()
        ).hexdigest()
        body = urllib.parse.urlencode(
            {
                "username": username,
                "nonce": challenge["nonce"],
                "response": answer,
            }
        ).encode()
        session = json.loads(self._request("/login", body))
        self.token = session["token"]
        return session

    def config(self):
        return json.loads(self._request("/config.json"))

    def history(self, limit, offset=None):
        """GET /history.json, paging by logical index.

        `window=` is deliberately never passed. It selects by time and
        DECIMATES to fit the 200-record cap - handleHistoryJson reports the
        `stride` it used - so a window query is a lossy view built for a chart.
        An archiver that used it would silently store one record in eight and
        report a full collection.
        """
        query = f"/history.json?limit={int(limit)}"
        if offset is not None:
            query += f"&offset={int(offset)}"
        return json.loads(self._request(query))

    def post_config(self, document):
        """UNEXERCISED against the garden. Only moisture_fit.py --push reaches it.

        The document goes in a FORM FIELD called `config`, not as a raw JSON
        body: `handleConfigPost` reads `request->getParam("config", true)` and
        answers 400 to anything else. `data/config.js`, `devices.js` and
        `schedules.js` all post it the same way. Read out of src/web_config.cpp
        rather than guessed, since nothing here has ever exercised it.
        """
        body = urllib.parse.urlencode(
            {"config": json.dumps(document, separators=(",", ":"))}
        ).encode()
        request = urllib.request.Request(
            self.base + "/config.json", data=body, method="POST"
        )
        request.add_header("Content-Type", "application/x-www-form-urlencoded")
        request.add_header("Authorization-Token", self.token)
        with self._opener.open(request, timeout=self.timeout) as response:
            self.requests += 1
            return response.read().decode("utf-8")

    def logout(self):
        if not self.token:
            return
        try:
            self._request("/logout", b"")
        except OSError:
            pass
        self.token = None
