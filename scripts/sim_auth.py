"""The simulated login — nonce + SHA-256 challenge, sessions and the fixed
developer credential — kept out of the request dispatcher that calls it.

Mirrors src/custom_login.cpp. Split out of dev_server.py, which crossed the
1000-line limit scripts/check_lines.py enforces.
"""

from __future__ import annotations

import hashlib
import json
import secrets
import threading
import time
from pathlib import Path

# Generated and disposable, like .pio/assets. The device keeps the equivalent at
# /sessions.json on LittleFS; keeping the mirror's copy out of the working tree
# means a developer's remembered token never reaches a commit.
SESSION_FILE = Path(__file__).resolve().parent.parent / ".pio" / "sim_sessions.json"


def _sha256(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


class AuthSim:
    """Mirror of src/custom_login.cpp — nonce + SHA-256 challenge.

    Credentials here are fixed and printed at startup: this simulator only ever
    binds to a developer machine, and a surprise password would just make the
    login page untestable.
    """

    NONCE_TTL_S = 30
    USERNAME = "admin"
    PASSWORD = "admin"
    ROLE_ADMIN = 2
    # Mirrors kMaxSessions / kMaxPersistentSessions / kPersistentTtlSec in
    # include/network/custom_login.h. Several devices per user is the point of
    # the first number; the second stops remembered sessions from filling the
    # table, since they are exempt from the idle timeout that bounds the rest.
    MAX_SESSIONS = 8
    MAX_PERSISTENT = 6
    PERSISTENT_TTL_S = 30 * 24 * 60 * 60

    def __init__(self, session_file=SESSION_FILE) -> None:
        self.lock = threading.Lock()
        self.salt = "0123456789abcdef"
        self.password_hash = _sha256(f"{self.salt}:{self.PASSWORD}")
        self.nonces: dict[str, float] = {}
        self.session_file = session_file
        # token -> {"user", "persistent", "last_seen", "created"}. The firmware
        # stores the user's INDEX; what matters to mirror is that a token has
        # ONE owner, so a per-user invalidation can be exact instead of taking
        # the whole table with it.
        self.tokens: dict[str, dict] = {}
        self._load()

    def issue_nonce(self, username: str) -> dict:
        nonce = secrets.token_hex(16)  # 32 hex chars, as in the firmware
        now = time.time()
        with self.lock:
            self.nonces = {
                n: t for n, t in self.nonces.items() if now - t <= self.NONCE_TTL_S
            }
            self.nonces[nonce] = now
        # An unknown user still gets a stable decoy salt, so the endpoint cannot
        # be used to enumerate accounts.
        salt = self.salt if username == self.USERNAME else _sha256(
            f"sim:nosuchuser:{username}"
        )[:16]
        return {"nonce": nonce, "salt": salt, "ttlMs": self.NONCE_TTL_S * 1000}

    # ---------- the slot table ----------
    def _evict_locked(self) -> None:
        """Free a slot the way session_slots::allocate() does: the oldest
        NON-persistent session first, and a remembered one only as a last
        resort. Without the tiering, a script authenticating in a loop would
        evict somebody's remembered browser, which is the failure raising
        MAX_SESSIONS was meant to relieve."""
        while len(self.tokens) >= self.MAX_SESSIONS:
            ephemeral = [t for t, s in self.tokens.items() if not s["persistent"]]
            pool = ephemeral or list(self.tokens)
            del self.tokens[min(pool, key=lambda t: self.tokens[t]["last_seen"])]

    def _enforce_persistent_cap_locked(self) -> None:
        """The seventh remembered device forgets the first. Explicable to an
        operator in a way that a refused login is not."""
        remembered = [t for t, s in self.tokens.items() if s["persistent"]]
        while len(remembered) >= self.MAX_PERSISTENT:
            oldest = min(remembered, key=lambda t: self.tokens[t]["last_seen"])
            del self.tokens[oldest]
            remembered.remove(oldest)

    def _purge_locked(self) -> None:
        now = time.time()
        for token in [t for t, s in self.tokens.items()
                      if s["persistent"] and s["created"]
                      and now - s["created"] > self.PERSISTENT_TTL_S]:
            del self.tokens[token]

    # ---------- /sessions.json ----------
    def _save(self) -> None:
        """Called whenever a REMEMBERED token is created OR destroyed. Saving
        only on creation is what let an evicted persistent token stay in the
        file and come back active at the next boot - and since a persistent
        session is exempt from the idle timeout, it would then be live for
        ever."""
        if self.session_file is None:
            return
        remembered = [
            {"t": t, "u": s["user"], "c": int(s["created"])}
            for t, s in self.tokens.items() if s["persistent"]
        ]
        self.session_file.parent.mkdir(parents=True, exist_ok=True)
        self.session_file.write_text(json.dumps(remembered), encoding="utf-8")

    def _load(self) -> None:
        if self.session_file is None or not self.session_file.exists():
            return
        try:
            stored = json.loads(self.session_file.read_text(encoding="utf-8"))
        except (ValueError, OSError):
            return
        if not isinstance(stored, list):
            return

        now = time.time()
        restored, discarded = 0, 0
        for entry in stored:
            token = entry.get("t") if isinstance(entry, dict) else None
            created = entry.get("c", 0) if isinstance(entry, dict) else 0
            # Only free slots, and only up to the cap - a full file must still
            # leave room for an ordinary login. Surplus and expired entries are
            # DROPPED, and the file is rewritten below rather than left to warn
            # again at every boot while its tokens stay resurrectable.
            if (not isinstance(token, str) or len(token) != 64
                    or restored >= self.MAX_PERSISTENT
                    or (created and now - created > self.PERSISTENT_TTL_S)):
                discarded += 1
                continue
            self.tokens[token] = {"user": entry.get("u", self.USERNAME),
                                  "persistent": True,
                                  "last_seen": now,
                                  "created": created}
            restored += 1
        if discarded:
            self._save()

    def login(self, username: str, nonce: str, response: str,
              remember: bool = False) -> str | None:
        now = time.time()
        with self.lock:
            issued = self.nonces.pop(nonce, None)  # one-shot
        if issued is None or now - issued > self.NONCE_TTL_S:
            return None
        if username != self.USERNAME:
            return None
        if response != _sha256(f"{nonce}:{self.password_hash}"):
            return None

        token = secrets.token_hex(32)  # 64 hex chars
        now = time.time()
        with self.lock:
            self._purge_locked()
            had_persistent = any(s["persistent"] for s in self.tokens.values())
            if remember:
                self._enforce_persistent_cap_locked()
            self._evict_locked()
            still_persistent = any(s["persistent"] for s in self.tokens.values())
            self.tokens[token] = {"user": username, "persistent": remember,
                                  "last_seen": now,
                                  "created": now if remember else 0}
            # The file is rewritten when this login is remembered AND when it
            # merely displaced one that was.
            if remember or (had_persistent and not still_persistent):
                self._save()
        return token

    def set_password(self, password: str) -> None:
        """Rewrite the stored credential. Sessions are NOT touched here.

        Ending them is the caller's decision and belongs to the endpoint that
        wrote the password — see invalidate_user. Doing it as a side effect of
        the hash change is what hid the hole in the firmware: while only one
        persistent session per user could exist, the next login revoked the old
        token by accident, so nothing ever revoked it on purpose.
        """
        with self.lock:
            self.password_hash = _sha256(f"{self.salt}:{password}")

    def password_matches(self, password: str) -> bool:
        """Whether this password is the one already stored.

        Mirrors the firmware comparing hashPassword(stored.salt, incoming)
        against the stored hash, which is what stops the documented
        GET ?secrets=1 -> edit -> POST restore from re-salting an unchanged
        password and signing every admin out with nothing altered.
        """
        with self.lock:
            return _sha256(f"{self.salt}:{password}") == self.password_hash

    def invalidate_user(self, username: str, caller_token: str = "") -> bool:
        """End every session of one account. Returns True when the caller's own
        token was among them, which is what the page turns into {"reauth":true}.

        The caller is not exempt: an attacker holding a stolen ADMIN token makes
        the request just as well as its owner, so exempting whoever asked would
        hand the surviving token to the wrong party.
        """
        with self.lock:
            doomed = [t for t, s in self.tokens.items() if s["user"] == username]
            persistent = any(self.tokens[t]["persistent"] for t in doomed)
            for token in doomed:
                del self.tokens[token]
            if persistent:
                self._save()
            return caller_token in doomed

    def invalidate_all(self) -> None:
        """Every session, every account — what a user DELETE owes, because the
        firmware's sessions hold a user index and removal shifts every later
        one."""
        with self.lock:
            self.tokens.clear()
            self._save()

    def logout(self, token: str) -> None:
        with self.lock:
            was_persistent = self.tokens.pop(token, {}).get("persistent", False)
            if was_persistent:
                self._save()

    def valid(self, token: str) -> bool:
        with self.lock:
            self._purge_locked()
            session = self.tokens.get(token)
            if session is None:
                return False
            session["last_seen"] = time.time()
            return True


AUTH = AuthSim()
