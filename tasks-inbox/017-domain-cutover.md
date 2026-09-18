# Move everything off sslip.io onto espgarden.com.br

**Blocked on DNS delegation.** The domain is bought, on Cloudflare free, with
all six records created and — the one that matters — **every one of them
DNS only, grey cloud**. registro.br announced a ~2 h nameserver transition and
`a.dns.br` still delegates to nobody.

**The grey cloud is not a preference.** Cloudflare's free plan proxies only
HTTP/HTTPS ports; **8883 is not among them**. A proxied record makes the ESP32
resolve to a Cloudflare address that does not answer MQTT at all, and the
failure reads as a network fault rather than a DNS choice. Cloudflare's own
banner asks you to turn proxying on — ignore it for these records.

**Done means, in order:**
1. `DOMAIN=tb.espgarden.com.br` in the VPS `.env`, Traefik re-rendered, a new
   certificate issued. `tbctl` only declares success on a chain `curl -f`
   accepts, so it proves itself.
2. `mqtt.server` on both boards. **`mqtt.cacert` does NOT change** — same
   Let's Encrypt root, and the five-root bundle already on flash validates it.
3. ThingsBoard sender → `noreply@espgarden.com.br`, then the UI's own test
   button.

**What this retires:** the dependency on sslip.io, a third-party wildcard DNS
service whose disappearance would break the name and the certificate renewal
together. That was the known weak point of the deploy from the day it shipped.
