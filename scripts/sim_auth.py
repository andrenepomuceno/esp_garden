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

    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.salt = "0123456789abcdef"
        self.password_hash = _sha256(f"{self.salt}:{self.PASSWORD}")
        self.nonces: dict[str, float] = {}
        self.tokens: set[str] = set()

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
            self.tokens.add(token)
        return token

    def set_password(self, password: str) -> None:
        with self.lock:
            self.password_hash = _sha256(f"{self.salt}:{password}")
            self.tokens.clear()  # existing sessions die with the old credential

    def logout(self, token: str) -> None:
        with self.lock:
            self.tokens.discard(token)

    def valid(self, token: str) -> bool:
        with self.lock:
            return token in self.tokens


AUTH = AuthSim()
