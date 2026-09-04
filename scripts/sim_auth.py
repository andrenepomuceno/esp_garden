"""The simulated login — nonce + SHA-256 challenge, sessions and the fixed
developer credential — kept out of the request dispatcher that calls it.

Mirrors src/custom_login.cpp. Split out of dev_server.py, which crossed the
1000-line limit scripts/check_lines.py enforces.
"""

from __future__ import annotations

import hashlib
import secrets
import threading
import time


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
    # Mirrors kMaxSessions in include/network/custom_login.h. Several devices
    # per user is the point of that number, so a token is never evicted for
    # belonging to somebody already signed in — only for being the oldest.
    MAX_SESSIONS = 8

    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.salt = "0123456789abcdef"
        self.password_hash = _sha256(f"{self.salt}:{self.PASSWORD}")
        self.nonces: dict[str, float] = {}
        # token -> owning username. The firmware stores the user's INDEX; what
        # matters to mirror is that a token has ONE owner, so a per-user
        # invalidation can be exact instead of taking the whole table with it.
        self.tokens: dict[str, str] = {}

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

    def login(self, username: str, nonce: str, response: str) -> str | None:
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
        with self.lock:
            self.tokens[token] = username
            # The firmware evicts the least-recently-SEEN slot; this evicts the
            # oldest issued, because the simulator does not track a last-seen
            # time. The property being mirrored is that a full table evicts
            # rather than refusing a login.
            while len(self.tokens) > self.MAX_SESSIONS:
                self.tokens.pop(next(iter(self.tokens)))
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

    def invalidate_user(self, username: str, caller_token: str = "") -> bool:
        """End every session of one account. Returns True when the caller's own
        token was among them, which is what the page turns into {"reauth":true}.

        The caller is not exempt: an attacker holding a stolen ADMIN token makes
        the request just as well as its owner, so exempting whoever asked would
        hand the surviving token to the wrong party.
        """
        with self.lock:
            doomed = [t for t, owner in self.tokens.items() if owner == username]
            for token in doomed:
                del self.tokens[token]
            return caller_token in doomed

    def invalidate_all(self) -> None:
        """Every session, every account — what a user DELETE owes, because the
        firmware's sessions hold a user index and removal shifts every later
        one."""
        with self.lock:
            self.tokens.clear()

    def logout(self, token: str) -> None:
        with self.lock:
            self.tokens.pop(token, None)

    def valid(self, token: str) -> bool:
        with self.lock:
            return token in self.tokens


AUTH = AuthSim()
