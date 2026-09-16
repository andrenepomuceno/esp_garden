# The setup AP's password is in the repository

**Operator's call — it changes the flow you specified.**

As built (your spec): SSID `espgarden-<id>`, password `espgarden`. `POST
/onboarding` takes no token, because there is nothing to authenticate against —
a board whose config never loaded has no `/users.json` account. So while the
portal is up, anyone in radio range can write the Wi-Fi credentials and the admin
account and take the board.

What bounds it today is *when* the portal can exist — one boot on a working
setup — and not authentication.

**The cheap alternative, proposed by the implementer and not implemented:**
derive the password from the efuse MAC and print it on the serial boot line and
on a sticker. Removes the shared secret entirely; costs one label per board.

**Rejected already, and worth recording so it is not re-proposed:** a portal that
self-closes after N minutes. On a board that cannot associate that is a reboot
loop wearing a timeout.
