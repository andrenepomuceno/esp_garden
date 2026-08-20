"""
ESP Garden — soil moisture calibration helper.

Answers two separate questions about a moisture channel, and refuses to
conflate them:

1. Does the history contain distinct dry/humid/wet STATES?
   Tested properly: fit the drying trend, then compare BIC for a 1-, 2- and
   3-component Gaussian mixture on the RESIDUALS. Clustering the raw series
   instead is the trap — a slow drying curve is unimodal but spends different
   amounts of time in different bands, so a 3-cluster fit always "succeeds" and
   returns tertiles of a trend dressed up as soil states.

2. If not, what does the history actually support?
   The drying rate, the response to watering, and the observed range — which is
   what you need to decide *when* to water, as opposed to *how wet* the soil is.

Absolute thresholds for a capacitive probe come from two reference readings,
not from history: the probe in air (its dry end) and submerged in water (its wet
end). Each probe has its own gain and offset, which is exactly why they have to
be calibrated individually.

3. Can the ACTUATOR give the anchors?
   Watering is a labelled transition — driest just before, wettest shortly
   after — so --from-events reads the anchors off those events instead of
   needing the probe pulled out of the soil. The span it returns is
   operational (this soil, this pot) rather than the probe's physical range,
   which is the more useful scale for deciding when to water.

Usage:
    python scripts/moisture_calibration.py --days 30
    python scripts/moisture_calibration.py --field 4 --days 60
    python scripts/moisture_calibration.py --days 2 --from-events   # anchors from watering
    python scripts/moisture_calibration.py --dry 94.0 --wet 12.0    # emit bands
"""

from __future__ import annotations

import argparse
import datetime
import json
import math
import statistics
import urllib.parse
import urllib.request

THINGSPEAK = "https://api.thingspeak.com/channels"
MAX_RESULTS_PER_CALL = 8000  # ThingSpeak hard cap


# --------------------------------------------------------------------------- fetch
# A trend fitted over a few hours is not a drying rate. Extrapolating one to a
# per-day figure produced "704 points/day" on a 0.2-day window the first time
# this ran, which is the kind of number that gets believed.
MIN_TREND_DAYS = 3.0


def fetch(channel: int, days: int, api_key: str | None,
          end: datetime.datetime | None = None) -> list[dict]:
    """Paginated pull. One call cannot exceed MAX_RESULTS_PER_CALL rows."""
    end = end or datetime.datetime.now(datetime.timezone.utc)
    start = end - datetime.timedelta(days=days)
    rows: list[dict] = []
    cursor = start

    while cursor < end:
        chunk_end = min(cursor + datetime.timedelta(days=7), end)
        params = {
            "start": cursor.strftime("%Y-%m-%d %H:%M:%S"),
            "end": chunk_end.strftime("%Y-%m-%d %H:%M:%S"),
            "results": MAX_RESULTS_PER_CALL,
        }
        if api_key:
            params["api_key"] = api_key
        url = f"{THINGSPEAK}/{channel}/feeds.json?{urllib.parse.urlencode(params)}"
        with urllib.request.urlopen(url, timeout=60) as response:
            rows += json.load(response).get("feeds", [])
        cursor = chunk_end

    return rows


def series(rows: list[dict], field: int, floor: float) -> list[tuple[float, float]]:
    """(days_since_start, value), dropping nulls and readings at or below floor.

    A disconnected probe reads at one rail, and those samples are not soil
    measurements — left in, they become a phantom mixture component.
    """
    key = f"field{field}"
    out = []
    for row in rows:
        raw = row.get(key)
        if raw in (None, ""):
            continue
        try:
            value = float(raw)
        except ValueError:
            continue
        if value <= floor:
            continue
        stamp = datetime.datetime.strptime(
            row["created_at"], "%Y-%m-%dT%H:%M:%SZ"
        ).timestamp()
        out.append((stamp, value))

    if not out:
        return []
    t0 = out[0][0]
    return [((t - t0) / 86400.0, v) for t, v in out]


# --------------------------------------------------------------------------- stats
def linear_fit(points: list[tuple[float, float]]) -> tuple[float, float, float]:
    """Least squares. Returns (intercept, slope_per_day, r_squared)."""
    n = len(points)
    mean_x = sum(x for x, _ in points) / n
    mean_y = sum(y for _, y in points) / n
    denom = sum((x - mean_x) ** 2 for x, _ in points)
    slope = sum((x - mean_x) * (y - mean_y) for x, y in points) / denom if denom else 0.0
    intercept = mean_y - slope * mean_x
    ss_tot = sum((y - mean_y) ** 2 for _, y in points)
    ss_res = sum((y - (intercept + slope * x)) ** 2 for x, y in points)
    r2 = 1 - ss_res / ss_tot if ss_tot else 0.0
    return intercept, slope, r2


def gmm_bic(data: list[float], k: int, iters: int = 200):
    """1-D Gaussian mixture by EM. Returns (bic, means). Lower BIC is better."""
    lo, hi = min(data), max(data)
    mu = [lo + (hi - lo) * (i + 0.5) / k for i in range(k)]
    sd = [statistics.pstdev(data) or 1.0] * k
    weight = [1.0 / k] * k

    for _ in range(iters):
        resp = []
        for value in data:
            p = [
                weight[j]
                * math.exp(-0.5 * ((value - mu[j]) / sd[j]) ** 2)
                / (sd[j] * math.sqrt(2 * math.pi))
                + 1e-300
                for j in range(k)
            ]
            total = sum(p)
            resp.append([q / total for q in p])
        for j in range(k):
            nk = sum(r[j] for r in resp) or 1e-300
            mu[j] = sum(r[j] * v for r, v in zip(resp, data)) / nk
            var = sum(r[j] * (v - mu[j]) ** 2 for r, v in zip(resp, data)) / nk
            sd[j] = max(math.sqrt(var), 1e-3)
            weight[j] = nk / len(data)

    log_lik = sum(
        math.log(
            sum(
                weight[j]
                * math.exp(-0.5 * ((v - mu[j]) / sd[j]) ** 2)
                / (sd[j] * math.sqrt(2 * math.pi))
                for j in range(k)
            )
            + 1e-300
        )
        for v in data
    )
    n_params = 3 * k - 1
    return -2 * log_lik + n_params * math.log(len(data)), sorted(mu)


# --------------------------------------------------------------------------- report
def analyse(points, rows, field, watering_field):
    values = [v for _, v in points]
    print(f"amostras utilizaveis : {len(values)}")
    print(
        f"faixa observada      : {min(values):.2f} .. {max(values):.2f}"
        f"  (mediana {statistics.median(values):.2f}, dp {statistics.pstdev(values):.2f})"
    )

    intercept, slope, r2 = linear_fit(points)
    span_days = points[-1][0] - points[0][0]
    trend_usable = span_days >= MIN_TREND_DAYS

    if trend_usable:
        print(
            f"tendencia            : {intercept:.2f} {slope:+.3f} por dia"
            f"  (R2={r2:.3f}, {span_days:.1f} dias)"
        )
    else:
        print(
            f"tendencia            : janela de apenas {span_days:.2f} dia(s) —"
            f" abaixo de {MIN_TREND_DAYS:g},"
        )
        print(
            "                       nao ha secagem a medir. Use --end para apontar"
            " para uma"
        )
        print("                       janela com historico.")

    residuals = [y - (intercept + slope * x) for x, y in points]
    sample = residuals[:: max(1, len(residuals) // 5000)]

    print(f"\nGMM nos residuos (n={len(sample)}) — BIC menor e melhor:")
    scores = {}
    for k in (1, 2, 3):
        bic, means = gmm_bic(sample, k)
        scores[k] = bic
        print(f"  k={k}  BIC={bic:10.1f}  centros={[round(m, 2) for m in means]}")
    best_k = min(scores, key=scores.get)

    print(f"\nveredicto            : melhor k = {best_k}")
    if best_k >= 3 and r2 < 0.5:
        print("  Os dados sustentam tres estados. Os centros acima sao os candidatos")
        print("  a seco / umido / molhado; use os pontos medios como limiares.")
    else:
        print("  Os dados NAO sustentam tres estados.")
        if not trend_usable:
            print("  A janela e curta demais para separar estados de uma tendencia.")
        elif r2 >= 0.5:
            print(
                f"  A tendencia sozinha explica {100 * r2:.1f}% da variancia:"
                " a serie e uma curva"
            )
            print(
                "  de secagem, nao estados. Agrupar isso em 3 devolve tercis da"
                " tendencia,"
            )
            print("  que rotulariam de 'seco' qualquer fim de periodo.")
        print("  Limiares absolutos precisam de calibracao de dois pontos por sonda")
        print("  (--dry / --wet), nao de historico.")

    events = []
    for row in rows:
        raw = row.get(f"field{watering_field}")
        if raw not in (None, ""):
            moisture = row.get(f"field{field}")
            events.append((row["created_at"], raw, moisture))
    print(f"\neventos de rega      : {len(events)}")
    for stamp, duration, moisture in events[:10]:
        print(f"  {stamp}  {duration} ms   umidade no evento: {moisture}")
    if len(events) < 10:
        print("  Poucos eventos para medir a resposta a rega de forma confiavel.")

    if slope < 0:
        print(
            f"\nutil mesmo assim     : a sonda perde {abs(slope):.3f} ponto/dia."
            " Isso dimensiona"
        )
        print("  o intervalo entre regas, que e a decisao operacional real.")


def calibrate_from_events(rows, field, watering_field, pre_min, post_min,
                          floor, ceiling):
    """Anchor dry/wet on watering events instead of air/water references.

    Watering is a labelled transition: the soil is at its driest just before one
    and at its wettest shortly after. That gives an OPERATIONAL span — this soil
    in this pot — rather than the probe's physical one, which is the more useful
    scale for deciding when to water.

    Events arrive in bursts, and consecutive activations share one wetting
    response. They are grouped into sessions first; counting each activation
    separately would report the same peak several times and pretend it is
    independent evidence.
    """
    stamps = []
    for row in rows:
        raw = row.get(f"field{field}")
        if raw in (None, ""):
            continue
        try:
            value = float(raw)
        except ValueError:
            continue
        moment = datetime.datetime.strptime(
            row["created_at"], "%Y-%m-%dT%H:%M:%SZ"
        ).replace(tzinfo=datetime.timezone.utc)
        stamps.append((moment, value, row.get(f"field{watering_field}") or ""))

    stamps.sort(key=lambda s: s[0])
    events = [m for m, _, w in stamps if w]
    if not events:
        print("nenhum evento de rega na janela.")
        return None

    sessions = [[events[0]]]
    for moment in events[1:]:
        if (moment - sessions[-1][-1]).total_seconds() <= post_min * 60:
            sessions[-1].append(moment)
        else:
            sessions.append([moment])

    print(f"eventos: {len(events)}  ->  sessoes de rega: {len(sessions)}")
    print(f"janelas: base = {pre_min} min antes, pico = {post_min} min depois\n")

    usable = []
    for session in sessions:
        start, last = session[0], session[-1]
        pre = [v for m, v, _ in stamps
               if -pre_min * 60 <= (m - start).total_seconds() < 0]
        post = [v for m, v, _ in stamps
                if 0 < (m - last).total_seconds() <= post_min * 60]
        if not pre or not post:
            verdict = "sem amostras suficientes"
            base = peak = None
        else:
            base = statistics.median(pre)
            peak = max(post)
            if not (floor < base < ceiling) or not (floor < peak < ceiling):
                verdict = "sonda no batente (desconectada)"
            elif peak <= base:
                verdict = "sem subida — descartada"
            else:
                verdict = "OK"
                usable.append((base, peak, statistics.pstdev(pre) if len(pre) > 1 else 0.0))
        shown = f"base={base:6.2f} pico={peak:6.2f} delta={peak - base:+5.2f}" \
            if base is not None else "—"
        print(f"  {start:%m-%d %H:%M} ({len(session):2d} acionamentos)  {shown}  {verdict}")

    if not usable:
        print("\nnenhuma sessao utilizavel: sem par base/pico valido.")
        return None

    # Anchors are EXTREMES, not central values. A median is dragged to the
    # middle by sessions that watered already-saturated soil, and most sessions
    # are exactly that: the driest the soil ever reached is the only honest dry
    # anchor, and the wettest it ever reached the only honest wet one.
    dry = min(b for b, _, _ in usable)
    wet = max(p for _, p, _ in usable)
    deltas = [p - b for b, p, _ in usable]

    print(f"\nsessoes utilizaveis  : {len(usable)}")
    print(f"resposta a rega      : mediana {statistics.median(deltas):+.2f}"
          f"  (min {min(deltas):+.2f}, max {max(deltas):+.2f})")
    if statistics.median(deltas) < 0.5 * max(deltas):
        print("  a mediana e bem menor que o maximo: a maioria das regas caiu em")
        print("  solo ja saturado, e so a maior sessao mede a faixa real")
    print(f"ancoras (extremos)   : dry={dry:.2f}  wet={wet:.2f}"
          f"  (span {wet - dry:.2f})")

    # Within-state noise, not the spread of the whole series: the latter counts
    # the wetting steps themselves as noise and overstates it several-fold.
    noise = statistics.median([sd for _, _, sd in usable])
    band = abs(wet - dry) / 3.0
    print(f"ruido intra-estado   : {noise:.2f}   largura de cada faixa: {band:.2f}")
    if band < noise:
        print("  AVISO: cada faixa e mais estreita que o ruido da propria serie."
              " A classificacao")
        print("  vai oscilar entre estados sem o solo mudar. Amplie a janela ou"
              " use ancoras")
        print("  fisicas (--dry/--wet) para uma escala maior.")

    if len(usable) < 3:
        print(f"  AVISO: apenas {len(usable)} sessao(oes) utilizavel(is)."
              " Trate as ancoras como provisorias.")

    print("\n  para config.json (\"moisture\"):")
    print(json.dumps({"dry": round(dry, 2), "wet": round(wet, 2)}, indent=4))
    return dry, wet


def emit_bands(dry: float, wet: float) -> None:
    """Two-point calibration: reading in air and submerged, per probe."""
    print("\n--- calibracao de dois pontos ---")
    print(f"  seco  (sonda no ar)   = {dry:.2f}")
    print(f"  molhado (submersa)    = {wet:.2f}")
    span = wet - dry
    humid_lo = dry + span / 3.0
    humid_hi = dry + 2.0 * span / 3.0
    print("\n  limiares sugeridos (tercos da faixa fisica da propria sonda):")
    print(f"    seco    : ate      {humid_lo:.2f}")
    print(f"    umido   : {humid_lo:.2f} .. {humid_hi:.2f}")
    print(f"    molhado : a partir de {humid_hi:.2f}")
    print("\n  para config.json:")
    print(
        json.dumps(
            {"dry": round(dry, 2), "wet": round(wet, 2)}, indent=4
        )
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--channel", type=int, default=1348790)
    parser.add_argument("--field", type=int, default=1, help="moisture field number")
    parser.add_argument("--watering-field", type=int, default=2)
    parser.add_argument("--days", type=int, default=30)
    parser.add_argument(
        "--end",
        default=None,
        help="fim da janela, YYYY-MM-DD (padrao: agora). Use para analisar historico.",
    )
    parser.add_argument("--api-key", default=None, help="read key for a private channel")
    parser.add_argument(
        "--floor",
        type=float,
        default=1.0,
        help="drop readings at or below this (disconnected probe rail)",
    )
    parser.add_argument(
        "--from-events",
        action="store_true",
        help="derive dry/wet from watering events instead of air/water references",
    )
    parser.add_argument("--pre-min", type=int, default=15,
                        help="minutos antes da rega para a base (padrao 15)")
    parser.add_argument("--post-min", type=int, default=45,
                        help="minutos apos a rega para o pico (padrao 45)")
    parser.add_argument("--ceiling", type=float, default=90.0,
                        help="descarta leituras acima disto (sonda no batente)")
    parser.add_argument("--dry", type=float, help="reading with the probe in air")
    parser.add_argument("--wet", type=float, help="reading with the probe in water")
    args = parser.parse_args()

    if args.dry is not None and args.wet is not None:
        emit_bands(args.dry, args.wet)
        return

    end = None
    if args.end:
        end = datetime.datetime.strptime(args.end, "%Y-%m-%d").replace(
            tzinfo=datetime.timezone.utc
        )
    print(
        f"canal {args.channel}, field{args.field}, {args.days} dias ate"
        f" {args.end or 'agora'}"
    )
    rows = fetch(args.channel, args.days, args.api_key, end)

    if args.from_events:
        calibrate_from_events(rows, args.field, args.watering_field,
                              args.pre_min, args.post_min, args.floor, args.ceiling)
        return
    print(f"registros baixados   : {len(rows)}")
    points = series(rows, args.field, args.floor)
    if len(points) < 100:
        raise SystemExit(
            f"apenas {len(points)} amostras utilizaveis — insuficiente para qualquer ajuste"
        )
    analyse(points, rows, args.field, args.watering_field)


if __name__ == "__main__":
    main()
