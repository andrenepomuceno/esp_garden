#!/usr/bin/env python3
"""Validate the device's reference-evapotranspiration estimate against a public station.

The firmware computes ET0 by Hargreaves-Samani, which needs only the day's
temperature extremes and extraterrestrial radiation -- a closed form in latitude
and day of year, so nothing on the device ever calls a weather API. This script
is the off-device half: it resolves where the device is, pulls the public record
for that spot, and reports how far the device's own estimate is from it.

It answers three questions, in this order, and refuses to skip the first:

  1. How much rain fell inside the archive's window? A watering model fitted on
     a window with no rain in it is a classifier with no positive examples.
  2. Does the device's thermometer see the outdoor diurnal cycle at all?
     Hargreaves-Samani is driven by the diurnal RANGE, so a sheltered sensor
     breaks it in a way no scale factor honestly repairs.
  3. What does the ET0 estimate actually score, and does a fitted correction
     survive leave-one-out?

Location comes from `postalCode` in the device's own config, so the archive is
self-describing; `--lat/--lon` overrides it and `--postal-code` replaces it.

    python scripts/et0_fit.py
    python scripts/et0_fit.py --lat -15.7880 --lon -47.9306
    python scripts/et0_fit.py --db backups/telemetry.sqlite --days 31

Nothing here writes to the archive or to any config file.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sqlite3
import statistics
import sys
import urllib.parse
import urllib.request
from collections import defaultdict
from datetime import date, datetime, timedelta, timezone

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DB = os.path.join(REPO, "backups", "telemetry.sqlite")
CACHE_DIR = os.path.join(REPO, ".pio", "weather_cache")
USER_AGENT = "esp-garden/et0_fit (https://github.com/andrenepomuceno/esp-garden)"

# The archive stores the window mean under the plain key; at a 60 s publish
# period that is the mean of each minute's 1 Hz samples.
TEMP_KEY = "temperature"
HUMIDITY_KEY = "airHumidity"

# A daily extreme is only a daily extreme if the day was watched. Tmin lands
# near sunrise and Tmax mid-afternoon, so a day missing either end reports a
# range that is an artefact of when the device happened to be up. This is the
# same gate the firmware applies in `et0DayFold()`.
MIN_HOURS_COVERED = 22

GSC = 0.0820  # solar constant, MJ m-2 min-1 (FAO-56)
LATENT_HEAT = 2.45  # MJ kg-1, so MJ m-2 d-1 -> mm d-1


# --------------------------------------------------------------------------
# physics -- the same two formulas the firmware implements, kept here so the
# script validates the maths and not merely the plumbing
# --------------------------------------------------------------------------

def extraterrestrial_radiation(lat_deg: float, day_of_year: int) -> float:
    """FAO-56 eq. 21/23/24/25. MJ m-2 day-1."""
    phi = math.radians(lat_deg)
    dr = 1.0 + 0.033 * math.cos(2.0 * math.pi * day_of_year / 365.0)
    dec = 0.409 * math.sin(2.0 * math.pi * day_of_year / 365.0 - 1.39)
    x = -math.tan(phi) * math.tan(dec)
    # Beyond the polar circles the sunset hour angle has no solution: the sun
    # either never sets or never rises. Clamping is the physical answer.
    if x <= -1.0:
        ws = math.pi
    elif x >= 1.0:
        ws = 0.0
    else:
        ws = math.acos(x)
    return ((24.0 * 60.0 / math.pi) * GSC * dr *
            (ws * math.sin(phi) * math.sin(dec) +
             math.cos(phi) * math.cos(dec) * math.sin(ws)))


def hargreaves(t_min: float, t_max: float, ra_mj: float) -> float:
    """FAO-56 eq. 52, Hargreaves-Samani. mm day-1."""
    t_mean = 0.5 * (t_min + t_max)
    rng = max(t_max - t_min, 0.0)
    return 0.0023 * (t_mean + 17.8) * math.sqrt(rng) * ra_mj / LATENT_HEAT


def saturation_vapour_pressure(t_c: float) -> float:
    return 0.6108 * math.exp(17.27 * t_c / (t_c + 237.3))


def dewpoint(t_c: float, rh_pct: float) -> float:
    e = saturation_vapour_pressure(t_c) * max(rh_pct, 1e-3) / 100.0
    ln = math.log(e / 0.6108)
    return 237.3 * ln / (17.27 - ln)


# --------------------------------------------------------------------------
# small stats helpers
# --------------------------------------------------------------------------

def linreg(xs, ys):
    """Return (r, slope, intercept); NaNs when a series is constant."""
    if len(xs) < 3:
        return float("nan"), float("nan"), float("nan")
    mx, my = statistics.mean(xs), statistics.mean(ys)
    sxy = sum((a - mx) * (b - my) for a, b in zip(xs, ys))
    sxx = sum((a - mx) ** 2 for a in xs)
    syy = sum((b - my) ** 2 for b in ys)
    if sxx <= 0.0 or syy <= 0.0:
        return float("nan"), float("nan"), float("nan")
    slope = sxy / sxx
    return sxy / math.sqrt(sxx * syy), slope, my - slope * mx


def score(pred, obs):
    n = len(pred)
    bias = statistics.mean(p - o for p, o in zip(pred, obs))
    mae = statistics.mean(abs(p - o) for p, o in zip(pred, obs))
    rmse = math.sqrt(statistics.mean((p - o) ** 2 for p, o in zip(pred, obs)))
    r, _, _ = linreg(pred, obs)
    return dict(n=n, mean_pred=statistics.mean(pred), mean_obs=statistics.mean(obs),
                bias=bias, mae=mae, rmse=rmse, r=r)


def show(label, s):
    pct = 100.0 * s["bias"] / s["mean_obs"] if s["mean_obs"] else float("nan")
    print(f"  {label:38s} n={s['n']:2d}  {s['mean_pred']:5.2f} vs {s['mean_obs']:5.2f}  "
          f"bias {s['bias']:+6.2f} ({pct:+6.1f} %)  MAE {s['mae']:5.2f}  "
          f"RMSE {s['rmse']:5.2f}  r {s['r']:+.3f}")


# --------------------------------------------------------------------------
# network -- everything that leaves the machine, in one place
# --------------------------------------------------------------------------

def fetch(url: str, cache_name: str | None, refresh: bool) -> dict:
    path = os.path.join(CACHE_DIR, cache_name) if cache_name else None
    if path and not refresh and os.path.exists(path):
        with open(path, "r", encoding="utf-8") as fh:
            return json.load(fh)
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=45) as resp:
        payload = json.loads(resp.read().decode("utf-8"))
    if path:
        os.makedirs(CACHE_DIR, exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(payload, fh)
    return payload


def read_postal_code(explicit: str | None) -> str | None:
    if explicit:
        return explicit
    # The real device config first; the template only carries an empty string,
    # so it is a fallback that will simply return None and say so.
    for name in ("config.json", "config.template.json"):
        path = os.path.join(REPO, "data", name)
        if not os.path.exists(path):
            continue
        try:
            with open(path, "r", encoding="utf-8") as fh:
                doc = json.load(fh)
        except (OSError, ValueError):
            continue
        code = (doc.get("postalCode") or "").strip()
        if code:
            print(f"  postalCode {code!r} read from data/{name}")
            return code
    return None


def geocode(postal_code: str, refresh: bool):
    """CEP -> street address -> lat/lon. Returns (lat, lon, how)."""
    digits = "".join(c for c in postal_code if c.isdigit())
    if len(digits) != 8:
        raise SystemExit(f"postalCode {postal_code!r} is not an 8-digit Brazilian CEP")
    cep = f"{digits[:5]}-{digits[5:]}"

    via = fetch(f"https://viacep.com.br/ws/{cep}/json/", f"viacep_{digits}.json", refresh)
    if via.get("erro"):
        raise SystemExit(f"ViaCEP does not know {cep}")
    street = via.get("logradouro", "")
    hood = via.get("bairro", "")
    city = via.get("localidade", "")
    uf = via.get("uf", "")
    print(f"  ViaCEP {cep}: {street}, {hood}, {city} - {uf}")

    # The street itself is the tightest thing OSM is likely to hold. Nominatim's
    # postalcode search returns nothing for Brazilian CEPs, so go by name -- and
    # a Brasilia address is "Quadra QRSW 5 Bloco A-6" where OSM holds the road as
    # plain "QRSW 5", so try the stripped form before the verbatim one.
    road = street
    for prefix in ("Quadra ", "Conjunto ", "Setor "):
        if road.startswith(prefix):
            road = road[len(prefix):]
            break
    for sep in (" Bloco ", " Lote ", " Casa ", " Conjunto "):
        if sep in road:
            road = road.split(sep)[0]
    road = road.strip()

    for query in ((f"{road}, {city}, {uf}, Brazil" if road and road != street else None),
                  road if road and road != street else None,
                  f"{street}, {city}, {uf}, Brazil" if street else None,
                  street or None,
                  f"{hood}, {city}, {uf}, Brazil" if hood else None):
        if not query:
            continue
        url = ("https://nominatim.openstreetmap.org/search?"
               + urllib.parse.urlencode({"q": query, "format": "jsonv2", "limit": 1,
                                         "countrycodes": "br"}))
        try:
            hits = fetch(url, f"nominatim_{abs(hash(query)) % (10 ** 10)}.json", refresh)
        except Exception as exc:  # noqa: BLE001 - a geocoder outage is not fatal
            print(f"  Nominatim failed for {query!r}: {exc}")
            continue
        if hits:
            h = hits[0]
            how = f"ViaCEP -> OSM Nominatim {h.get('osm_type')}/{h.get('osm_id')} ({query!r})"
            print(f"  Nominatim: {h.get('display_name')}")
            return float(h["lat"]), float(h["lon"]), how

    geo = fetch("https://geocoding-api.open-meteo.com/v1/search?"
                + urllib.parse.urlencode({"name": city, "country": "BR", "count": 1,
                                          "format": "json"}),
                f"omgeo_{city}.json", refresh)
    if geo.get("results"):
        g = geo["results"][0]
        print("  Nominatim had nothing; fell back to the city centroid, which is "
              "COARSE -- pass --lat/--lon if the garden is far from it.")
        return float(g["latitude"]), float(g["longitude"]), "ViaCEP -> Open-Meteo city centroid"
    raise SystemExit("could not geocode the postal code; pass --lat/--lon")


def open_meteo_daily(lat, lon, past_days, refresh):
    url = ("https://api.open-meteo.com/v1/forecast?"
           + urllib.parse.urlencode({
               "latitude": f"{lat:.4f}", "longitude": f"{lon:.4f}",
               "daily": ",".join(["temperature_2m_max", "temperature_2m_min",
                                  "precipitation_sum", "rain_sum", "precipitation_hours",
                                  "et0_fao_evapotranspiration", "shortwave_radiation_sum",
                                  "relative_humidity_2m_mean", "wind_speed_10m_mean"]),
               "timezone": "auto", "past_days": past_days, "forecast_days": 1}))
    return fetch(url, f"om_daily_{lat:.3f}_{lon:.3f}_{past_days}.json", refresh)


def open_meteo_hourly(lat, lon, past_days, refresh):
    url = ("https://api.open-meteo.com/v1/forecast?"
           + urllib.parse.urlencode({
               "latitude": f"{lat:.4f}", "longitude": f"{lon:.4f}",
               "hourly": "temperature_2m,relative_humidity_2m,precipitation",
               "timezone": "auto", "past_days": past_days, "forecast_days": 1}))
    return fetch(url, f"om_hourly_{lat:.3f}_{lon:.3f}_{past_days}.json", refresh)


# --------------------------------------------------------------------------
# the archive
# --------------------------------------------------------------------------

def load_series(db_path, key, tz):
    """key -> {local datetime: value}. Opened read-only; this never writes."""
    uri = "file:" + urllib.request.pathname2url(db_path) + "?mode=ro"
    con = sqlite3.connect(uri, uri=True)
    try:
        rows = con.execute(
            "select ts, cast(value as real) from telemetry where key=? order by ts",
            (key,)).fetchall()
    finally:
        con.close()
    return [(datetime.fromtimestamp(ts / 1000.0, tz), v) for ts, v in rows]


def daily_extremes(samples):
    """Local date -> dict(tmin, tmax, mean, n, hours, tmax_at, tmin_at)."""
    out = {}
    for when, value in samples:
        e = out.setdefault(when.date(), dict(vals=[], hours=set(), lo=None, hi=None))
        e["vals"].append(value)
        e["hours"].add(when.hour)
        if e["lo"] is None or value < e["lo"][1]:
            e["lo"] = (when, value)
        if e["hi"] is None or value > e["hi"][1]:
            e["hi"] = (when, value)
    for e in out.values():
        e["tmin"] = min(e["vals"])
        e["tmax"] = max(e["vals"])
        e["mean"] = statistics.mean(e["vals"])
        e["n"] = len(e["vals"])
        e["covered"] = len(e["hours"])
        e["full"] = len(e["hours"]) >= MIN_HOURS_COVERED
    return out


# --------------------------------------------------------------------------
# the report
# --------------------------------------------------------------------------

def report_rain(daily, days_present):
    print()
    print("=" * 78)
    print("RAIN INSIDE THE ARCHIVE'S WINDOW -- read this before proposing a rain model")
    print("=" * 78)
    wet = 0
    total = 0.0
    for day in days_present:
        iso = day.isoformat()
        if iso not in daily:
            continue
        mm = daily[iso]["prcp"] or 0.0
        hrs = daily[iso]["prcp_hours"] or 0.0
        total += mm
        if mm >= 1.0:
            wet += 1
        flag = "  <- wet day (>= 1 mm)" if mm >= 1.0 else ""
        print(f"  {iso}  {mm:6.2f} mm over {hrs:4.1f} h{flag}")
    n = len([d for d in days_present if d.isoformat() in daily])
    print(f"\n  total {total:.1f} mm over {n} days; {wet} day(s) reached 1 mm.")
    if wet < 5:
        print("\n  REFUSED: this window cannot support a rain model. Fitting a "
              "rain/no-rain\n  classifier here would be training without positive "
              "examples -- the same\n  defect that blocks the soil-moisture classifier "
              "in this repo. Nothing\n  below models rain; it is reported so the "
              "ET0 comparison can be read\n  knowing the sky was almost always dry.")
    return total, wet


def report_siting(dev_t, dev_h, hourly, tz):
    """Does the device's thermometer see the outdoor cycle? This decides
    whether Hargreaves-Samani can mean anything on this hardware."""
    print()
    print("=" * 78)
    print("IS THE DEVICE'S THERMOMETER IN FREE AIR?")
    print("=" * 78)
    st = {}
    for t, tc, rh in zip(hourly["time"], hourly["temperature_2m"],
                         hourly["relative_humidity_2m"]):
        st[t] = (tc, rh)

    def bucket(samples):
        b = defaultdict(list)
        for when, v in samples:
            b[when.replace(minute=0, second=0, microsecond=0).strftime("%Y-%m-%dT%H:%M")].append(v)
        return {k: statistics.mean(v) for k, v in b.items() if len(v) >= 10}

    dt_, dh = bucket(dev_t), bucket(dev_h)
    common = sorted(set(dt_) & set(st))
    if len(common) < 24:
        print(f"  only {len(common)} matched hours -- not enough to judge siting")
        return None
    print(f"  {len(common)} hours matched against the station.\n")
    print(f"  {'hr':>3s} {'station T':>9s} {'device T':>9s} {'dT':>7s} "
          f"{'station Td':>10s} {'device Td':>9s} {'dTd':>7s}")
    for h in range(24):
        ks = [k for k in common if int(k[11:13]) == h]
        if not ks:
            continue
        s_t = statistics.mean(st[k][0] for k in ks)
        d_t = statistics.mean(dt_[k] for k in ks)
        s_d = statistics.mean(dewpoint(*st[k]) for k in ks)
        d_d = statistics.mean(dewpoint(dt_[k], dh[k]) for k in ks if k in dh) \
            if any(k in dh for k in ks) else float("nan")
        print(f"  {h:3d} {s_t:9.2f} {d_t:9.2f} {d_t - s_t:+7.2f} "
              f"{s_d:10.2f} {d_d:9.2f} {d_d - s_d:+7.2f}")

    r, slope, icpt = linreg([st[k][0] for k in common], [dt_[k] for k in common])
    print(f"\n  device T  = {slope:.3f} * station T  + {icpt:5.2f}   r = {r:+.3f}")
    kd = [k for k in common if k in dh]
    if len(kd) >= 24:
        rd, sd, idd = linreg([dewpoint(*st[k]) for k in kd],
                             [dewpoint(dt_[k], dh[k]) for k in kd])
        print(f"  device Td = {sd:.3f} * station Td + {idd:5.2f}   r = {rd:+.3f}")
        print("\n  Dewpoint is conserved when air is merely heated, so a slope near 1 on"
              "\n  the dewpoint with a slope well under 1 on the temperature is the"
              "\n  signature of a sensor in the same air mass but behind thermal mass --"
              "\n  sheltered, enclosed or indoors, not in free air.")
    if slope < 0.75:
        print(f"\n  WARNING: the device follows only {100 * slope:.0f} % of the outdoor swing."
              "\n  Hargreaves-Samani is driven by the diurnal RANGE, so on this siting the"
              "\n  estimate below is measuring the enclosure as much as the sky. A scale"
              "\n  factor hides that; it does not fix it.")
    return slope


# A grid-point analysis a few km away, plus a DHT11's own +-2 K, comfortably
# covers a 5 K disagreement on the day's maximum. Past that, the two are not
# describing the same air, and the day is named rather than quietly dropped.
TMAX_DISAGREEMENT_K = 5.0


def report_et0(dev_days, daily, lat):
    print()
    print("=" * 78)
    print("ET0 -- HARGREAVES-SAMANI FROM THE DEVICE vs FAO-56 PENMAN-MONTEITH")
    print("=" * 78)
    rows = []
    print(f"  {'date':>10s} {'hrs':>4s} {'dTmin':>6s} {'dTmax':>6s} {'dRng':>6s} "
          f"{'sTmin':>6s} {'sTmax':>6s} {'sRng':>6s} {'Ra':>6s} "
          f"{'HSdev':>6s} {'HSstn':>6s} {'PMstn':>6s}")
    for day in sorted(dev_days):
        iso = day.isoformat()
        if iso not in daily or daily[iso]["et0"] is None:
            continue
        e = dev_days[day]
        s = daily[iso]
        ra = extraterrestrial_radiation(lat, day.timetuple().tm_yday)
        hs_dev = hargreaves(e["tmin"], e["tmax"], ra)
        hs_stn = hargreaves(s["tmin"], s["tmax"], ra)
        odd = e["tmax"] - s["tmax"] > TMAX_DISAGREEMENT_K
        rows.append(dict(iso=iso, full=e["full"], ra=ra, hs_dev=hs_dev, hs_stn=hs_stn,
                         pm=s["et0"], d_rng=e["tmax"] - e["tmin"],
                         s_rng=s["tmax"] - s["tmin"], d_max=e["tmax"], d_min=e["tmin"],
                         odd=odd))
        note = ""
        if not e["full"]:
            note = f"   <- REFUSED, only {e['covered']}/24 h covered"
        elif odd:
            note = (f"   <- device Tmax is {e['tmax'] - s['tmax']:+.1f} K off the "
                    f"station's")
        print(f"  {iso:>10s} {e['covered']:4d} {e['tmin']:6.2f} {e['tmax']:6.2f} "
              f"{e['tmax'] - e['tmin']:6.2f} {s['tmin']:6.2f} {s['tmax']:6.2f} "
              f"{s['tmax'] - s['tmin']:6.2f} {ra:6.2f} {hs_dev:6.2f} {hs_stn:6.2f} "
              f"{s['et0']:6.2f}" + note)

    full = [r for r in rows if r["full"]]
    if len(full) < 3:
        print("\n  fewer than three complete days; nothing to score")
        return None
    print(f"\n  --- complete days only (n={len(full)}) ---")
    pm = [r["pm"] for r in full]
    show("HS(station T)  vs PM(station)", score([r["hs_stn"] for r in full], pm))
    show("HS(device T)   vs PM(station)", score([r["hs_dev"] for r in full], pm))
    show("HS(device T)   vs HS(station T)", score([r["hs_dev"] for r in full],
                                                  [r["hs_stn"] for r in full]))

    odd = [r for r in full if r["odd"]]
    if odd:
        clean = [r for r in full if not r["odd"]]
        print(f"\n  --- sensitivity: {len(odd)} day(s) where the device's Tmax is more "
              f"than {TMAX_DISAGREEMENT_K:.0f} K\n      above the station's. Reported, "
              f"NOT dropped from the headline above. ---")
        for r in odd:
            print(f"    {r['iso']}  device Tmax {r['d_max']:5.2f} C, range "
                  f"{r['d_rng']:5.2f} K -> HS {r['hs_dev']:5.2f} against PM {r['pm']:5.2f}")
        if len(clean) >= 3:
            show("HS(device T) vs PM, odd days dropped",
                 score([r["hs_dev"] for r in clean], [r["pm"] for r in clean]))
        print("    A daily maximum has no averaging in it: one short excursion owns the"
              "\n    whole day. That is a property of the ESTIMATOR, not a bad sample to"
              "\n    filter out -- the sensor really did read that, and this repo does not"
              "\n    clamp real readings away.")

    print("\n  --- what carries the signal (r against PM ET0) ---")
    for name, key in (("device Tmax", "d_max"), ("device Tmin", "d_min"),
                      ("device diurnal range", "d_rng"),
                      ("station diurnal range", "s_rng")):
        r, _, _ = linreg([x[key] for x in full], pm)
        print(f"    {name:26s} r = {r:+.3f}")
    r_rng, _, _ = linreg([x["d_rng"] for x in full], [x["s_rng"] for x in full])
    print(f"    device range vs station range   r = {r_rng:+.3f}")
    if abs(r_rng) < 0.6:
        print("\n    The device's diurnal range barely tracks the real one, which is the"
              "\n    single input Hargreaves-Samani exists to use. Whatever skill the"
              "\n    estimate has below is coming from the temperature LEVEL, not from"
              "\n    the range -- i.e. not from the mechanism the formula is built on.")

    mean_hs = statistics.mean(r["hs_dev"] for r in full)
    fitted = statistics.mean(pm) / mean_hs if mean_hs > 0 else float("nan")
    print(f"\n  --- a fitted scale factor, and whether it survives leave-one-out ---")
    print(f"    scale that removes the mean bias on all {len(full)} days: x{fitted:.3f}")
    errs_cal, errs_null, scales = [], [], []
    for i in range(len(full)):
        tr = [r for j, r in enumerate(full) if j != i]
        te = full[i]
        s = statistics.mean(r["pm"] for r in tr) / statistics.mean(r["hs_dev"] for r in tr)
        scales.append(s)
        errs_cal.append(s * te["hs_dev"] - te["pm"])
        errs_null.append(statistics.mean(r["pm"] for r in tr) - te["pm"])
    rmse_cal = math.sqrt(statistics.mean(e * e for e in errs_cal))
    rmse_null = math.sqrt(statistics.mean(e * e for e in errs_null))
    print(f"    LOO scale spread                x{min(scales):.3f} .. x{max(scales):.3f}")
    print(f"    LOO RMSE, scaled HS(device)     {rmse_cal:5.2f} mm/d")
    print(f"    LOO RMSE, constant climatology  {rmse_null:5.2f} mm/d")
    print(f"    station PM over those days      mean {statistics.mean(pm):.2f}, "
          f"sd {statistics.pstdev(pm):.2f} mm/d")
    if rmse_cal < rmse_null:
        print(f"    -> the scaled estimate beats a constant by "
              f"{100 * (1 - rmse_cal / rmse_null):.0f} % of RMSE, on {len(full)} days.")
    else:
        print("    -> the scaled estimate does NOT beat a constant. Do not ship it.")

    print(f"""
  ---------------------------------------------------------------------------
  To put this device's measured correction into config.json (et0.scale ships
  at 1.0, i.e. textbook Hargreaves-Samani with nothing fitted):

      "et0": {{ "enabled": true, "latitude": {lat:.4f}, "scale": {fitted:.3f} }}

  Read `scale` as a statement about THIS sensor at THIS mounting. A value far
  from 1.0 is a siting fault wearing a calibration coefficient, and it is
  measured on {len(full)} days of one season -- it does not transfer to another
  board, another mounting, or another time of year.
  ---------------------------------------------------------------------------""")
    return dict(fitted=fitted, rmse_cal=rmse_cal, rmse_null=rmse_null, n=len(full))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", default=DEFAULT_DB, help="telemetry archive (read-only)")
    ap.add_argument("--postal-code", help="override the config's postalCode")
    ap.add_argument("--lat", type=float, help="skip geocoding entirely")
    ap.add_argument("--lon", type=float, help="skip geocoding entirely")
    ap.add_argument("--days", type=int, default=31, help="days of public record to pull")
    ap.add_argument("--tz-offset", type=float, default=None,
                    help="local UTC offset in hours; default is the station's")
    ap.add_argument("--refresh", action="store_true", help="ignore the cached fetches")
    args = ap.parse_args(argv)

    print("=" * 78)
    print("WHERE IS THIS GARDEN?")
    print("=" * 78)
    how = "given on the command line"
    if args.lat is not None and args.lon is not None:
        lat, lon = args.lat, args.lon
    else:
        code = read_postal_code(args.postal_code)
        if not code:
            raise SystemExit(
                "No postalCode found in data/config*.json.\n"
                "  A local copy taken before that key existed simply will not have it --\n"
                "  refresh it with `GET /config.json?secrets=1` from the device, or pass\n"
                "  --postal-code / --lat --lon. Nothing here writes to data/config.json.")
        lat, lon, how = geocode(code, args.refresh)
    print(f"  resolved to  lat {lat:.4f}  lon {lon:.4f}")
    print(f"  how          {how}")

    daily_raw = open_meteo_daily(lat, lon, args.days, args.refresh)
    hourly_raw = open_meteo_hourly(lat, lon, args.days, args.refresh)
    gl, gn = daily_raw["latitude"], daily_raw["longitude"]
    dist = math.hypot((gl - lat) * 111.32,
                      (gn - lon) * 111.32 * math.cos(math.radians(lat)))
    print(f"  Open-Meteo grid point lat {gl:.4f} lon {gn:.4f}, elevation "
          f"{daily_raw.get('elevation')} m -- {dist:.1f} km from the garden")
    print(f"  timezone {daily_raw.get('timezone')} "
          f"(UTC{daily_raw.get('utc_offset_seconds', 0) / 3600:+.0f})")
    print("  The public numbers are a MODEL analysis at that grid point, not a "
          "thermometer\n  in this garden. A few km and a hundred metres of "
          "elevation are inside the\n  disagreement reported below, not outside it.")

    off = args.tz_offset if args.tz_offset is not None \
        else daily_raw.get("utc_offset_seconds", 0) / 3600.0
    tz = timezone(timedelta(hours=off))

    d = daily_raw["daily"]
    daily = {t: dict(tmin=a, tmax=b, et0=e, prcp=p, rain=rn, prcp_hours=ph, rad=sr)
             for t, a, b, e, p, rn, ph, sr in zip(
                 d["time"], d["temperature_2m_min"], d["temperature_2m_max"],
                 d["et0_fao_evapotranspiration"], d["precipitation_sum"],
                 d["rain_sum"], d["precipitation_hours"], d["shortwave_radiation_sum"])}

    if not os.path.exists(args.db):
        raise SystemExit(f"archive not found: {args.db}")
    dev_t = load_series(args.db, TEMP_KEY, tz)
    dev_h = load_series(args.db, HUMIDITY_KEY, tz)
    if not dev_t:
        raise SystemExit(f"no {TEMP_KEY!r} rows in {args.db}")
    print(f"\n  archive: {len(dev_t)} {TEMP_KEY} rows, "
          f"{dev_t[0][0]:%Y-%m-%d %H:%M} .. {dev_t[-1][0]:%Y-%m-%d %H:%M} local")

    dev_days = daily_extremes(dev_t)
    report_rain(daily, sorted(dev_days))
    report_siting(dev_t, dev_h, hourly_raw["hourly"], tz)
    report_et0(dev_days, daily, lat)
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
