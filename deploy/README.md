# Self-hosted ThingsBoard CE for esp-garden

Deployment tooling for running **ThingsBoard Community Edition** on a single
Ubuntu VPS (Hostinger or anywhere else — a Hostinger VPS is plain Ubuntu over
SSH, and nothing here uses any Hostinger-specific API), serving the ESP32
firmware in this repo: MQTT over TLS on 8883, two-way RPC, and the chunked
`v2/fw` firmware stream.

**Honesty line, in this repo's tradition:** everything in this directory is
*written and locally checked, not yet executed against a real VPS*. The
[Verified vs. unverified](#verified-vs-unverified) table at the bottom says
exactly which is which. Treat the first real deployment as the test it is.

## Architecture

```
                        internet
                           |
        :80 ---------------+---------------- :8883
   ACME challenge          |            MQTT over TLS
   + redirect only        :443          (the ESP32 dials this)
                           |
                    +-------------+
                    |   traefik   |   terminates ALL TLS
                    +-------------+   (one Let's Encrypt cert, auto-renewed)
                      |         |
              http tb:8080   tcp tb:1883      <- internal Docker network only
                      |         |
                    +-------------+     +----------+
                    | tb-node     |-----| postgres |   <- NO published ports
                    | (monolith)  |     +----------+
                    +-------------+
```

Three containers, all official public images (`thingsboard/tb-node`,
`postgres`, `traefik`) — no Dockerfile anywhere, because nothing here needs
one; configuration does it all.

**Why Traefik terminates TLS for MQTT too** (the deliberate choice the brief
asks for): an HTTP-only reverse proxy cannot carry MQTT at all, and letting
ThingsBoard terminate MQTT TLS itself would create a *second* certificate
consumer with its own key files and reload procedure. That is the setup where
the web certificate renews automatically for years while MQTT quietly serves
an expired one — and a fleet of CA-pinning ESP32s fails every connect with
`-9984` and *nothing surfaces it*. This project already lost 2023–2026 of
telemetry to exactly that failure shape on ThingSpeak. Here there is one
certificate, one terminator, one automatic renewal path; MQTT *cannot* be on
a different cert than the UI. ThingsBoard sees plaintext MQTT on the internal
Docker network of a single host — traffic that never touches a wire.

## Files

| File | What it is |
|---|---|
| `docker-compose.yml` | The stack. Reads everything from `.env`; secrets use `${VAR:?}` so compose refuses to start half-configured |
| `.env.example` | Every parameter, documented inline. Copy to `.env` (gitignored) and fill in |
| `tbctl` | The single operator entry point — install / up / down / status / logs / backup / restore / upgrade / ca-export / secure-admin / harden / backup-cron |
| `traefik/dynamic.yml.in` | Route template; `tbctl` renders it with the values from `.env` into `traefik/dynamic/` (gitignored) |
| `.gitignore` | Keeps `.env`, backups, rendered config and exported PEMs out of git |

## First deployment, end to end

Prerequisites: an Ubuntu VPS you can SSH into, and a DNS **A record** for
your chosen domain pointing at it **before** you start — Let's Encrypt
validates over HTTP, and issuance fails without it.

```bash
# 1. On the VPS: Docker from Ubuntu's own repos (distro-maintained beats
#    curl-pipe-sh from a security standpoint).
sudo apt-get update && sudo apt-get install -y docker.io docker-compose-v2

# 2. From your machine: copy this directory over.
rsync -av --exclude backups deploy/ user@your-vps:~/tb-deploy/

# 3. On the VPS:
cd ~/tb-deploy
chmod +x tbctl                # rsync from a Windows checkout drops the exec bit
cp .env.example .env && chmod 600 .env
$EDITOR .env                  # every REQUIRED field is documented in the file

sudo ./tbctl harden           # firewall + SSH keys-only + unattended upgrades.
                              # Runs FIRST, not last: the window between "TB is
                              # up" and "the host is hardened" is when the
                              # default-credential scanners find you.

./tbctl install               # pull, schema install, start, wait for a valid
                              # certificate, retire the default sysadmin login
sudo ./tbctl backup-cron      # nightly backups at 03:17
./tbctl status                # everything green? cert on 8883? probe refused?
```

`tbctl install` will not finish while the factory `sysadmin@thingsboard.org /
sysadmin` credential still works: it changes that password to
`TB_SYSADMIN_PASSWORD` from `.env` over the REST API, and `tbctl status`
re-probes the default credential **every time it runs** — because restoring
an old backup can silently resurrect it.

Then, in the ThingsBoard UI at `https://<DOMAIN>`:

1. Log in as `sysadmin@thingsboard.org` with `TB_SYSADMIN_PASSWORD`.
2. Create a **tenant**, and a tenant administrator with its own strong
   password. Day-to-day work happens as the tenant, not as sysadmin.
3. Create the **device** (its profile can stay `default`).
4. Open the device → **Manage credentials** → type **MQTT Basic** → set
   *Client ID*, *Username* and *Password*. The ESP32 authenticates with MQTT
   Basic, **not** an access token — the firmware sends clientId + username +
   password, so an access-token credential will never match it.

## The ESP32 side

Two things move from the VPS to the device: the broker settings and the CA
file the firmware pins.

**1. The CA file.** On the VPS:

```bash
./tbctl ca-export             # writes ./thingsboard.pem
```

This emits **ISRG Root X1** (the root of every Let's Encrypt chain, valid to
2035 — pin roots, not intermediates: intermediates rotate, which is the exact
mechanism behind the 2023–2026 ThingSpeak outage) — and it *refuses to emit
the file* unless the live chain on `<DOMAIN>:8883` actually verifies against
it. An exported-but-wrong CA is every device failing with `-9984` in silence;
the refusal is the feature.

Get it onto the device:

- Copy it into this repo as `data/thingsboard.pem` (so future filesystem
  images carry it), **and**
- upload it to the running device as `/thingsboard.pem` via
  `POST /spiffs/upload` (ADMIN) — one file, no reflash. Do **not** reach for
  `uploadfs` for this: a filesystem deploy overwrites the device's
  `/config.json` with whatever is in `data/` (see the trap in the root
  `CLAUDE.md`).

**2. `config.json`** on the device (`GET /config.json?secrets=1` → edit →
`POST /config.json`, then restart — the config is read at boot only):

```jsonc
"mqtt": {
    "enabled": true,
    "backend": "thingsboard",         // selects the ThingsBoard payload + downlink
    "server": "<DOMAIN>",             // must equal the certificate's hostname
    "port": 8883,
    "useTLS": true,
    "cacert": "/thingsboard.pem",     // the file uploaded above
    "clientID": "<Client ID from Manage credentials>",
    "username": "<Username from Manage credentials>",
    "password": "<Password from Manage credentials>",
    "rpc": true,                      // v1/devices/me/rpc/request/+
    "fwUpdate": true,                 // v2/fw chunked firmware stream
    "fwTitle": "esp-garden"           // must match the package title in ThingsBoard
}
```

The RPC and firmware topics ride the same single MQTT connection — nothing
else to open on the VPS. The 4 KB firmware chunks are ordinary MQTT
publishes, which is precisely why the plan terminates MQTT TLS as TCP
passthrough-with-TLS at Traefik instead of trying to squeeze any of this
through an HTTP reverse proxy.

If the device does not connect: `./tbctl logs tb` on the VPS shows the
broker's view; on the device, `-9984` in `/logs` means the chain/CA mismatch
this tooling exists to prevent — re-run `./tbctl ca-export` and compare with
what is on the device.

## Operations

### Backups — and the restore you must rehearse

```bash
./tbctl backup                       # pg_dump + /data volume + ACME state
./tbctl restore backups/<timestamp>  # destructive; asks for confirmation
```

Nightly via `sudo ./tbctl backup-cron`; retention is
`BACKUP_RETENTION_DAYS`. The backup contains **every device credential and
the TLS account key** — the `backups/` directory is `chmod 700` and must be
treated as secret.

A backup nobody has restored is a hope. Right after the first install, while
there is nothing to lose, run one full `backup` → `restore` cycle and then
`./tbctl status` — that rehearsal is what turns the restore path from prose
into a tested procedure, and it is deliberately step 5 of what `install`
prints.

Backups land on the **same disk** as the database. Copy them off-host (from
your machine: `rsync -av user@vps:~/tb-deploy/backups/ ./tb-backups/`) — a
dead VPS otherwise takes the backups with it.

### Upgrading ThingsBoard

```bash
$EDITOR .env          # change TB_VERSION to the new pinned tag
./tbctl upgrade       # backs up first, runs the schema upgrade, restarts
```

`upgrade` follows ThingsBoard's own documented docker upgrade (stop, run the
image once with `UPGRADE_TB=true FROM_VERSION=<old>`, start) and takes a
backup before touching anything — that backup *is* the rollback.

**When an upgrade goes wrong:** set `TB_VERSION` back to the old tag in
`.env`, then `./tbctl restore backups/<the one upgrade just took> --yes`,
then `./tbctl up`. Do not try to run the new binary against the half-upgraded
schema — the restore is the supported path back.

Postgres major versions (`POSTGRES_TAG` 16 → 17) are a different animal: the
data directory format changes, so the path is `backup` on the old version →
`down` → change the tag → remove the `pgdata` volume → `up` → `restore`.

### Day to day

```bash
./tbctl status        # containers, cert expiry on 8883, default-cred probe
./tbctl logs tb       # or postgres, traefik
./tbctl down && ./tbctl up
```

## Security posture

What is in place, each tied to the failure it prevents:

- **Three ports total** (80 ACME/redirect, 443 UI, 8883 MQTT-TLS). Postgres
  and plaintext MQTT have **no `ports:` entry at all** — not firewalled-off,
  simply never published. This matters because Docker's published ports
  bypass ufw; the only port that cannot be reached is one that was never
  mapped.
- **TLS everywhere reachable**, one auto-renewing Let's Encrypt certificate
  for both 443 and 8883 — renewal cannot "forget MQTT" because MQTT has no
  separate certificate to forget.
- **No Docker socket in any container.** Traefik uses the file provider;
  socket access is host-root, and a proxy compromise must not become a VPS
  compromise.
- **Host hardening is a command** (`sudo ./tbctl harden`): default-deny ufw,
  rate-limited SSH, keys-only auth (with a lockout guard: it refuses unless
  an `authorized_keys` exists), `sshd -t` before reload, unattended security
  updates.
- **The factory sysadmin credential does not survive install**, and `status`
  re-probes it forever after.
- **Secrets live in `.env` only** (gitignored, `chmod 600`): not in image
  layers (no images are built), not on `docker compose` command lines, not
  in the repo. Compose refuses to start with `${VAR:?}` unset, and `tbctl`
  additionally rejects placeholder and short values.
- `no-new-privileges` on every container; memory limits so one runaway
  process cannot OOM the neighbours. Postgres drops to its own unprivileged
  user by image design; tb-node and traefik start as root in their official
  images (traefik must bind :80/:443/:8883) — constraining them further was
  deliberately not invented here (see below).

**What this does NOT protect against** — plainly:

- **DoS / resource exhaustion** on the three public ports. Nothing
  rate-limits MQTT connects or HTTP requests beyond what ThingsBoard itself
  does.
- **Brute force against device MQTT credentials or tenant web logins.** ufw
  rate-limits SSH only; ThingsBoard's own lockout policies are whatever CE
  defaults to.
- **A kernel/Docker-level container escape.** All containers share the VPS
  kernel; there is no VM boundary, no AppArmor/seccomp profile beyond
  Docker's defaults, and tb/traefik run as in-container root.
- **A compromised VPS.** Anyone with root on the host owns the database, the
  TLS key, `.env`, and the backups. There is no intrusion detection.
- **Data at rest.** Volumes and backups are unencrypted on the VPS disk;
  off-host copies are only as safe as where you put them.
- **Loss of the VPS between backups.** Nightly cadence means up to 24 h of
  telemetry gone; the ESP32's RAM-only publish queue (see root `CLAUDE.md`)
  buffers about one hour, not a day.
- **A malicious or hijacked upstream image.** Tags are pinned but not
  digest-pinned; Docker Hub serving a poisoned `4.3.1.4` would be pulled.

## Verified vs. unverified

Kept in the spirit of the root `CLAUDE.md`: a number without an execution
behind it is a guess formatted to look like a fact.

**Verified (2026-08-24, on the workstation — no VPS was touched):**

- `bash -n tbctl` passes; both YAML files parse; the rendered
  `dynamic.yml.in` output is valid YAML with the domain substituted and no
  leftover `@...@` token (the actual `render()` was executed in a harness).
- `load_env`/`check_env` were executed against nine `.env` cases: the valid
  one is accepted, and empty/placeholder/short passwords, the factory
  `sysadmin` password, a quote-carrying password, a URL-shaped or
  still-`example.com` DOMAIN and a non-email ACME_EMAIL are each refused
  with the intended message. (This harness caught two real bugs before any
  VPS could: a literal `@TOKENS@` in a template comment tripping render's
  leftover-token check, and a `find | grep` under `pipefail` that would have
  falsely refused `harden` on hosts with keys only under `/root`.)
- `thingsboard/tb-node:4.3.1.4` exists on Docker Hub (tag list fetched).
- The install and upgrade invocations (`INSTALL_TB=true LOAD_DEMO=false`,
  `UPGRADE_TB=true FROM_VERSION=…`, `run --no-deps --rm`) mirror
  ThingsBoard's own `docker-install-tb.sh` / `docker-upgrade-tb.sh`, read
  from their repo on the same date.
- `HTTP_BIND_PORT` defaults to 8080 and `MQTT_BIND_PORT` to 1883 in
  ThingsBoard's `thingsboard.yml` (read from source).
- MQTT Basic (clientId/username/password) is a standard per-device
  credential type needing no server-side flag (ThingsBoard docs).
- The `config.json` keys in "The ESP32 side" match what `src/config.cpp`
  actually parses (grepped, same date).

**Unverified — written, reviewed, never run:**

- `docker compose config` itself (no Docker on this workstation), and
  therefore every runtime behaviour of the stack: schema install, ACME
  issuance, the Traefik TCP/TLS route, healthchecks (including whether
  `bash` is on the tb-node image's healthcheck PATH), `JAVA_OPTS` being
  honoured, the `changePassword` REST call, backup/restore/upgrade, harden,
  ca-export's live-chain check. Each carries a comment saying so where the
  risk is real.
- Whether ThingsBoard CE 4.3.x accepts `FROM_VERSION` in four-part
  `x.y.z.w` form; if the upgrade container objects, pass the three-part
  version by hand: `echo 4.2.2 > .tb-installed-version` and re-run.
- End-to-end: an actual ESP32 connecting through Traefik to this broker
  with the exported CA. That is the first thing to test on a real deploy —
  `./tbctl ca-export` refusing or succeeding is the earliest honest signal.
