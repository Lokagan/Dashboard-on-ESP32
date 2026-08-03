#!/usr/bin/env python3
"""
activity_monitor.py — Vue « qui fait quoi » du réseau, à partir des logs DNS d'AdGuard Home

Un serveur DNS ne voit QUE les résolutions de noms (jamais les débits) : cette page
lit le Query Log d'AGH (API REST), mappe chaque domaine vers un service lisible
(services.json), masque le bruit (télémétrie/CDN), groupe par appareil et rafraîchit
en direct. Aucun débit ici — c'est du pur DNS (les Mb/s viennent de la Freebox, ailleurs).

Affichage STABLE : ordre figé par nom d'appareil, état des appareils mémorisé côté
serveur (ils restent visibles PRESENCE_S puis passent « inactif » au lieu de disparaître),
et mise à jour ciblée du DOM côté client (pas de reconstruction -> pas de clignotement).
Chaque appareil a une couleur stable (pastille dans sa carte + part du camembert des
requêtes totales).

Endpoints HTTP :
  GET /           page web temps réel (rafraîchie côté client toutes les 3 s)
  GET /activity   JSON { active_window, presence_window, devices:[...] }
"""

# ----------------------------------------------------------------
# BIBLIOTHÈQUES
# ----------------------------------------------------------------
import os
import re
import json
import time
import threading
import logging
import traceback
import requests
from datetime import datetime
from flask import Flask, Response, jsonify, request

# Werkzeug logge chaque requête HTTP en INFO : /activity est sondé toutes les 3 s.
# ⚠️ Logger global au process : vaut aussi pour le bridge (8090).
logging.getLogger("werkzeug").setLevel(logging.WARNING)

# ----------------------------------------------------------------
# OBJETS GLOBAUX
# ----------------------------------------------------------------
AGH_URL       = os.getenv("AGH_URL", "http://192.168.1.1:3000").rstrip("/")
AGH_USER      = os.getenv("AGH_USER", "")
AGH_PASS      = os.getenv("AGH_PASS", "")
ACTIVITY_PORT = int(os.getenv("ACTIVITY_PORT", "8091"))

# Deux fenêtres, clé de la stabilité de l'affichage :
#   ACTIVE_S   : au-delà, l'appareil est marqué « inactif » mais RESTE affiché.
#   PRESENCE_S : au-delà, l'appareil disparaît de la liste.
# QUERYLOG_LIMIT borne ce qu'on tire d'AGH par appel.
ACTIVE_S       = int(os.getenv("ACTIVE_S", "90"))
PRESENCE_S     = int(os.getenv("PRESENCE_S", "36000"))
INSTANT_S      = int(os.getenv("INSTANT_S", "10"))       # fenêtre glissante du donut « instantané »
QUERYLOG_LIMIT = int(os.getenv("QUERYLOG_LIMIT", "1000"))

HOUR_S, DAY_S, WEEK_S = 3600, 86400, 604800              # fenêtres glissantes des donuts historiques
UNKNOWN_TTL = 6 * 3600      # un domaine inconnu quitte le panneau s'il n'est pas revu depuis N s

# Au démarrage on rejoue le query log d'AGH : l'historique est rempli tout de
# suite et survit à un redémarrage, sans SQLite. Inutile de dépasser la plus
# grande fenêtre affichée (7 j).
BACKFILL_S         = int(os.getenv("BACKFILL_DAYS", "7")) * 86400
BACKFILL_PAGE      = 1000                                 # entrées par page d'API
BACKFILL_MAX_PAGES = 2000                                 # garde-fou (2 M entrées max)

SERVICES_FILE = os.getenv("SERVICES_FILE", "/app/scripts/services.json")

http = requests.Session()

_svc_cache     = {"mtime": -1.0, "services": [], "noise": []}   # rechargé à chaud si le fichier change
_clients_cache = {"ts": 0.0, "names": {}}                       # IP -> nom (AGH), rafraîchi toutes les 30 s
_hist          = {}   # IP -> {minute_epoch: nb} — historique, cumulé par fenêtre (heure/24h/semaine)
_hist_lock     = threading.Lock()   # _hist est touché par les requêtes ET le thread de backfill
_devices_state = {}   # IP -> {name, last_ts, services:{nom:{icon,ts}}, last_domain, last_domain_ts}
_unknown       = {}   # domaine registrable -> {count, last_ts} — panneau « domaines inconnus »
_ingest        = {"ts": 0.0}   # horodatage le plus récent déjà ingéré (n'ingère que le nouveau)
_state_lock    = threading.Lock()   # sérialise _devices_state (page web + freebox_monitor)


def log(msg):
    print(f"[activity] {msg}", flush=True)


# ----------------------------------------------------------------
# ACCÈS ADGUARD HOME
# ----------------------------------------------------------------

# --- GET authentifié — (re)login par cookie de session au besoin ---
def _login():
    r = http.post(f"{AGH_URL}/control/login",
                  json={"name": AGH_USER, "password": AGH_PASS}, timeout=8)
    r.raise_for_status()

def _agh_get(path, params=None):
    r = http.get(f"{AGH_URL}{path}", params=params, timeout=8)
    if r.status_code in (401, 403):
        _login()
        r = http.get(f"{AGH_URL}{path}", params=params, timeout=8)
    r.raise_for_status()
    return r.json()

# --- Correspondance IP -> nom d'appareil (persistants + auto-résolus rDNS) ---
def _client_names():
    now = time.time()
    if now - _clients_cache["ts"] < 30:
        return _clients_cache["names"]
    names = {}
    try:
        data = _agh_get("/control/clients")
        # rDNS d'abord, puis clients persistants (manuels) qui ÉCRASENT : le nom fixé à la
        # main dans AGH prime sur le nom auto-résolu (souvent périmé, ex. hostname DHCP figé).
        for c in data.get("auto_clients") or []:
            if c.get("ip") and c.get("name"):
                names[c["ip"]] = c["name"]
        for c in data.get("clients") or []:
            for cid in c.get("ids") or []:
                if c.get("name"):
                    names[cid] = c["name"]
    except Exception as ex:
        log(f"clients: {ex}")
        names = _clients_cache["names"]     # on garde l'ancienne carte plutôt que rien
    _clients_cache.update(ts=now, names=names)
    return names

# --- Historique par appareil : compartiments d'1 minute, cumulés par fenêtre glissante ---
def _hist_add(ip, ts):
    mb = int(ts // 60)
    with _hist_lock:
        b = _hist.setdefault(ip, {})
        b[mb] = b.get(mb, 0) + 1

def _window_counts(now, window_s):
    floor_min = int((now - window_s) // 60)
    res = {}
    with _hist_lock:
        for ip, buckets in _hist.items():
            c = sum(cnt for m, cnt in buckets.items() if m >= floor_min)
            if c:
                res[ip] = c
    return res

def _hist_list(counts, names):
    out = [{"ip": ip, "name": names.get(ip) or ip, "count": c} for ip, c in counts.items()]
    out.sort(key=lambda d: d["count"], reverse=True)
    return out

def _prune_hist(now):
    floor_min = int((now - WEEK_S) // 60)
    with _hist_lock:
        for ip in list(_hist):
            b = _hist[ip]
            for m in [m for m in b if m < floor_min]:
                del b[m]
            if not b:
                del _hist[ip]

# --- Suggestion de service pour un domaine inconnu : table d'indices (CDN cryptiques),
# sinon le label principal du domaine mis en Majuscule. Renvoie (nom, jeton de match). ---
_SUGGEST_HINTS = {
    "nflx": "Netflix", "ttvnw": "Twitch", "jtvnw": "Twitch", "fbcdn": "Facebook",
    "cdninstagram": "Instagram", "ytimg": "YouTube", "googlevideo": "YouTube",
    "aaplimg": "Apple", "akamai": "CDN Akamai", "edgekey": "CDN Akamai",
    "fastly": "CDN Fastly", "cloudfront": "CDN CloudFront", "llnwd": "CDN Limelight",
    "1e100": "Google", "gvt": "Google", "windows": "Microsoft", "xboxlive": "Xbox",
    "playstation": "PlayStation", "epicgames": "Epic", "riotcdn": "Riot",
}
def _suggest(domain):
    labels = domain.split(".")
    sld = labels[-2] if len(labels) >= 2 else domain
    low = domain.lower()
    for token, name in _SUGGEST_HINTS.items():
        if token in low:
            return name, sld
    return (sld[:1].upper() + sld[1:], sld)     # défaut : label principal en Majuscule

# --- Panneau « domaines inconnus » : les plus fréquents, non encore mappés. Auto-nettoyant :
# un domaine mappé depuis (services.json rechargé) ou trop vieux quitte la liste. ---
def _unknown_list(services, noise, now):
    floor = now - UNKNOWN_TTL
    out = []
    for dom in list(_unknown):
        e = _unknown[dom]
        if e["last_ts"] < floor or _classify(dom, services, noise)[0] != "unknown":
            del _unknown[dom]
            continue
        name, tok = _suggest(dom)
        out.append({"domain": dom, "count": e["count"], "suggest": name, "match": tok})
    out.sort(key=lambda d: d["count"], reverse=True)
    return out[:30]

# --- Backfill : rejoue le query log d'AGH de [t0-BACKFILL_S, t0) pour amorcer l'historique.
# L'ingestion live (build_activity) prend le relais après t0 — pas de double comptage. ---
def _backfill(t0):
    floor = t0 - BACKFILL_S
    older, pages, added = None, 0, 0
    try:
        while pages < BACKFILL_MAX_PAGES:
            params = {"limit": BACKFILL_PAGE}
            if older:
                params["older_than"] = older
            data = _agh_get("/control/querylog", params)
            rows = data.get("data") or []
            if not rows:
                break
            done = False
            for e in rows:
                ts = _parse_time(e.get("time"))
                if ts is None or ts >= t0:
                    continue                         # zone temps réel : gérée par l'ingestion live
                if ts < floor:
                    done = True
                    break                            # entrées triées du + récent au + ancien
                if e.get("client") and (e.get("question") or {}).get("name"):
                    _hist_add(e["client"], ts)
                    added += 1
            older = data.get("oldest")
            pages += 1
            if done or not older:
                break
        log(f"backfill terminé : {added} requêtes, {pages} page(s), fenêtre {BACKFILL_S/86400:.1f} j")
    except Exception as ex:
        log(f"backfill: {ex}")


# ----------------------------------------------------------------
# MAPPING DOMAINE -> SERVICE
# ----------------------------------------------------------------

# --- Rechargement à chaud de services.json (dès que le mtime bouge) ---
def _services():
    try:
        m = os.path.getmtime(SERVICES_FILE)
    except OSError:
        return _svc_cache["services"], _svc_cache["noise"]
    if m != _svc_cache["mtime"]:
        try:
            with open(SERVICES_FILE, encoding="utf-8") as f:
                data = json.load(f)
            _svc_cache["services"] = data.get("services") or []
            _svc_cache["noise"]    = [n.lower() for n in (data.get("noise") or [])]
            _svc_cache["mtime"]    = m
            log(f"services.json rechargé — {len(_svc_cache['services'])} services, "
                f"{len(_svc_cache['noise'])} filtres de bruit")
        except Exception as ex:
            log(f"services.json illisible, on garde l'ancien : {ex}")
    return _svc_cache["services"], _svc_cache["noise"]

# --- Domaine enregistrable approximatif (2 labels, 3 si TLD composé courant) ---
_MULTI_TLD = ("co.uk", "com.au", "co.jp", "com.br", "co.nz", "org.uk", "gouv.fr")
def _registrable(d):
    parts = d.split(".")
    if len(parts) >= 3 and ".".join(parts[-2:]) in _MULTI_TLD:
        return ".".join(parts[-3:])
    return ".".join(parts[-2:]) if len(parts) >= 2 else d

# --- Un token matche-t-il un domaine, en respectant les frontières de labels ? ---
# Token AVEC point (t.co, amazon.fr, pool.ntp) = suite de labels ENTIERS : doit s'aligner sur
#   des labels -> ".token." dans ".domaine." — rejette riot.com/dropbox.com pour "t.co"/"x.com".
# Token SANS point (youtube, fbcdn) = fragment DANS un label (ne traverse jamais un point).
# Les points de tête/fin du token sont ignorés ("yt3." se comporte comme "yt3").
def _match_token(token, wrapped, labels):
    t = token.strip(".")
    if not t:
        return False
    if "." in t:
        return ("." + t + ".") in wrapped
    return any(t in lab for lab in labels)

# --- Classe un domaine : ("service"|"unknown", label, icone) ou ("noise", None, None) ---
# Priorité aux services AVANT le bruit : googlevideo.com = YouTube, pas du bruit Google.
def _classify(domain, services, noise):
    d = domain.lower().rstrip(".")
    wrapped = "." + d + "."
    labels = d.split(".")
    for svc in services:
        for token in svc.get("match") or []:
            if _match_token(token, wrapped, labels):
                return "service", svc["name"], svc.get("icon", "")
    for n in noise:
        if _match_token(n, wrapped, labels):
            return "noise", None, None
    return "unknown", _registrable(d), "\U0001F310"    # 🌐


# ----------------------------------------------------------------
# AGRÉGATION PAR APPAREIL (état mémorisé entre appels)
# ----------------------------------------------------------------
def build_activity():
    with _state_lock:
        return _build_state_and_output()

# Service courant par IP (nom seul, sans emoji ni « ~ ») — pour enrichir freebox/devices.
# Rafraîchit l'état même si aucune page web n'est ouverte.
def current_service_names():
    with _state_lock:
        try:
            _build_state_and_output()
        except Exception as ex:
            log(f"service names: {ex}")
        now = time.time()
        out = {}
        for ip, st in list(_devices_state.items()):
            if now - st["last_ts"] > PRESENCE_S:
                continue
            known = sorted(st["services"].items(), key=lambda kv: kv[1]["ts"], reverse=True)
            if known:
                out[ip] = known[0][0]
            elif st["last_domain"] and now - st["last_domain_ts"] <= PRESENCE_S:
                out[ip] = _suggest(st["last_domain"])[0]
        return out

def _build_state_and_output():
    services, noise = _services()
    names = _client_names()
    data  = _agh_get("/control/querylog", {"limit": QUERYLOG_LIMIT}).get("data") or []
    now   = time.time()

    # 1) Ingestion — QUE les entrées plus récentes que le dernier horodatage vu
    #    (hw), sinon on recompte les mêmes entrées à chaque appel.
    hw       = _ingest["ts"]
    new_high = hw
    for e in data:
        ts = _parse_time(e.get("time"))
        if ts is None or ts <= hw or now - ts > PRESENCE_S:
            continue
        ip  = e.get("client") or ""
        dom = ((e.get("question") or {}).get("name") or "").lower().rstrip(".")
        if not ip or not dom:
            continue

        st = _devices_state.setdefault(ip, {
            "name": ip, "last_ts": 0.0, "services": {},
            "last_domain": "", "last_domain_ts": 0.0,
        })
        st["name"]    = names.get(ip) or st["name"]
        st["last_ts"] = max(st["last_ts"], ts)
        new_high      = max(new_high, ts)

        _hist_add(ip, ts)                           # compartiment minute pour l'historique

        kind, label, icon = _classify(dom, services, noise)
        if kind == "service":
            s = st["services"].get(label)
            if not s or ts > s["ts"]:
                st["services"][label] = {"icon": icon, "ts": ts}
        elif kind == "unknown":
            if ts > st["last_domain_ts"]:
                st["last_domain"], st["last_domain_ts"] = label, ts
            u = _unknown.setdefault(label, {"count": 0, "last_ts": 0.0})
            u["count"] += 1
            u["last_ts"] = ts
        # noise : ne compte que dans queries + last_ts (vivant, mais rien à montrer)
    _ingest["ts"] = new_high

    # Débit instantané : comptage GLISSANT (non cumulé) des requêtes des INSTANT_S
    # dernières secondes, par appareil — alimente le second donut.
    inst = {}
    for e in data:
        ts = _parse_time(e.get("time"))
        if ts is None or now - ts > INSTANT_S:
            continue
        ip = e.get("client") or ""
        if ip and (e.get("question") or {}).get("name"):
            inst[ip] = inst.get(ip, 0) + 1

    # 2) Restitution : purge des vieux, puis sérialisation triée par NOM (ordre figé).
    out = []
    for ip in list(_devices_state):
        st = _devices_state[ip]
        if now - st["last_ts"] > PRESENCE_S:
            del _devices_state[ip]
            continue
        st["services"] = {k: v for k, v in st["services"].items() if now - v["ts"] <= PRESENCE_S}
        known = sorted(st["services"].items(), key=lambda kv: kv[1]["ts"], reverse=True)

        if known:
            current = {"name": known[0][0], "icon": known[0][1]["icon"]}
        elif st["last_domain"] and now - st["last_domain_ts"] <= PRESENCE_S:
            # Auto-étiquetage : nom deviné (marqué guess) plutôt que le domaine brut.
            current = {"name": _suggest(st["last_domain"])[0], "icon": "\U0001F310", "guess": True}
        else:
            current = {"name": "—", "icon": ""}     # —

        out.append({
            "ip": ip,
            "name": st["name"],
            "current": current,
            "services": [{"name": k, "icon": v["icon"]} for k, v in known[:5]],
            "rate": inst.get(ip, 0),
            "idle": (now - st["last_ts"]) > ACTIVE_S,
            "ago": int(now - st["last_ts"]),
        })

    out.sort(key=lambda d: (d["name"].lower(), d["ip"]))
    _prune_hist(now)
    return {
        "active_window": ACTIVE_S, "presence_window": PRESENCE_S, "instant_window": INSTANT_S,
        "hour_window": HOUR_S, "day_window": DAY_S, "week_window": WEEK_S,
        "week": _hist_list(_window_counts(now, WEEK_S), names),
        "day":  _hist_list(_window_counts(now, DAY_S),  names),
        "hour": _hist_list(_window_counts(now, HOUR_S), names),
        "unknown": _unknown_list(services, noise, now),
        "catalog": [{"name": s["name"], "icon": s.get("icon", "")} for s in services],
        "devices": out,
    }

# --- Horodatage AGH -> epoch (tolère nanosecondes et 'Z') ---
def _parse_time(s):
    if not s:
        return None
    try:
        s = s.replace("Z", "+00:00")
        s = re.sub(r"(\.\d{6})\d+", r"\1", s)   # fromisoformat n'accepte que 6 décimales
        return datetime.fromisoformat(s).timestamp()
    except Exception:
        return None


# ----------------------------------------------------------------
# PAGE WEB
# ----------------------------------------------------------------
PAGE = """<!doctype html>
<html lang="fr">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Activite reseau</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  /* Mise en page "app shell" : la page ne defile pas, seule la grille des
     cartes le fait. En-tete et bandeau des donuts restent donc visibles.
     dvh et pas vh : sur mobile, vh compte la barre d'adresse retractable et
     laisse depasser la page de quelques dizaines de pixels. */
  body { margin: 0; font-family: system-ui, sans-serif; background: #0d1117; color: #e6edf3;
         height: 100vh; height: 100dvh; display: flex; flex-direction: column;
         overflow: hidden; }
  header { padding: 18px 22px; border-bottom: 1px solid #21262d; display: flex;
           align-items: baseline; gap: 16px; flex-wrap: wrap;
           background: #0d1117; flex: none; }

  /* Un onglet visible occupe toute la hauteur restante.
     ⚠️ :not(.hidden) est OBLIGATOIRE. Un selecteur d'id (specificite 100) bat
     .hidden (10) : sans lui, le `display: flex` ci-dessous l'emporte sur le
     `display: none` de .hidden et l'onglet Activite ne se cache plus jamais —
     la navigation par onglets est cassee. */
  #view-activity:not(.hidden),
  #view-services:not(.hidden),
  #view-unknowns:not(.hidden) { flex: 1; min-height: 0; }
  #view-activity:not(.hidden) { display: flex; flex-direction: column; }

  /* min-height:0 est OBLIGATOIRE sur un enfant flex qui doit defiler : sans
     lui, min-height vaut auto, l'enfant refuse de descendre sous la hauteur de
     son contenu et c'est la PAGE qui deborde au lieu de la zone. */
  #view-activity > .summary { flex: none; }
  /* ⚠️ align-content: start est OBLIGATOIRE depuis que #grid a une hauteur
     DÉFINIE (flex: 1). Un conteneur grid de hauteur définie repartit l'espace
     libre sur ses rangées implicites — align-content vaut `normal`, qui se
     comporte comme `stretch` : les cartes s'étirent verticalement pour remplir
     la zone. Avant, #grid avait une hauteur auto et les rangées suivaient leur
     contenu. */
  #grid { flex: 1; min-height: 0; overflow-y: auto; overscroll-behavior: contain;
          align-content: start; }
  #view-services, #view-unknowns { overflow-y: auto; }

  /* Repli en defilement normal du document, en gardant l'en-tete visible par
     position:sticky. Deux cas, et le second est celui qui casse sur mobile :
       - fenetre BASSE  : le bandeau des donuts ne laisserait rien a la grille ;
       - fenetre ETROITE: en portrait telephone la hauteur est confortable, mais
         l'en-tete ET les 4 donuts passent a la ligne, et le meme probleme
         revient sans qu'aucun critere de hauteur ne se declenche. */
  @media (max-height: 600px), (max-width: 760px) {
    body { height: auto; overflow: visible; display: block; }
    header { position: sticky; top: 0; z-index: 20; }
    #view-activity:not(.hidden) { display: block; }
    #grid, #view-services, #view-unknowns { overflow: visible; min-height: 0; }
  }
  header h1 { margin: 0; font-size: 18px; letter-spacing: 2px; }
  #status { color: #7d8590; font-size: 13px; }

  /* Flex centre : donut total | legende | donut instantane. Les deux donuts etant
     de meme largeur, la legende se retrouve centree sur la page. */
  .summary { display: flex; justify-content: center; align-items: center; gap: 20px;
             flex-wrap: wrap; padding: 22px; border-bottom: 1px solid #21262d; }
  .pie { position: relative; width: 138px; height: 138px; border-radius: 50%;
         background: #161b22; flex: none; transition: background .4s ease; }
  .piewrap { display: flex; flex-direction: column; align-items: center; flex: none; }
  .pielabel { text-align: center; font-size: 11px; letter-spacing: 1px; text-transform: uppercase;
              color: #7d8590; margin-top: 8px; }
  .pie::after { content: ''; position: absolute; inset: 30%; background: #0d1117; border-radius: 50%; }
  .pietotal { position: absolute; inset: 0; z-index: 2; display: flex; flex-direction: column;
              align-items: center; justify-content: center; line-height: 1.1; }
  .pietotal b { font-size: 20px; }
  .pietotal span { font-size: 11px; color: #7d8590; }
  .legend { display: flex; flex-direction: column; gap: 5px;
            max-height: 168px; overflow: auto; min-width: 190px; }
  .leg { display: flex; align-items: center; gap: 8px; font-size: 13px; }
  .leg .sw { width: 11px; height: 11px; border-radius: 3px; flex: none; }
  .leg .lname { color: #c9d1d9; flex: 1; white-space: nowrap; }
  .leg .lval { color: #7d8590; font-variant-numeric: tabular-nums; }

  main { display: grid; gap: 14px; padding: 22px;
         grid-template-columns: repeat(auto-fill, minmax(250px, 1fr)); }
  .card { position: relative; background: #161b22; border: 1px solid #21262d; border-radius: 12px;
          padding: 16px; min-height: 120px; transition: opacity .35s ease, border-color .35s ease; }
  .card.enter { opacity: 0; }
  .card.idle { opacity: .45; border-color: #1b1f24; }
  .dot { position: absolute; top: 14px; right: 14px; width: 13px; height: 13px; border-radius: 50%;
         box-shadow: 0 0 0 2px #0d1117; }
  .dev { font-weight: 600; font-size: 15px; margin-bottom: 12px; color: #c9d1d9; padding-right: 22px; }
  .cur { font-size: 20px; font-weight: 700; display: flex; align-items: center; gap: 10px; }
  .cur .ic { font-size: 26px; min-width: 28px; }
  .cur .svc { color: #7ee787; }
  .cur .svc.guess { color: #d29922; font-style: italic; }   /* deviné (non curé dans services.json) */
  .card.idle .cur .svc { color: #7d8590; }
  .chips { margin-top: 10px; display: flex; flex-wrap: wrap; gap: 6px; min-height: 8px; }
  .chip { font-size: 12px; background: #21262d; border-radius: 20px; padding: 3px 10px; color: #adbac7; }
  .ago { margin-top: 12px; font-size: 11px; color: #7d8590; }
  .ago .live { color: #3fb950; }
  .empty { color: #7d8590; padding: 40px; text-align: center; grid-column: 1 / -1; }

  /* « Domaines inconnus » vit maintenant dans son propre onglet (bloc normal, plus de footer fixe). */
  .unknowns { margin-bottom: 22px; }
  .unknowns h2 { font-size: 13px; letter-spacing: 1px; text-transform: uppercase; color: #adbac7;
                 margin: 0 0 10px; }
  .unknowns h2 span { text-transform: none; letter-spacing: 0; color: #6e7681; font-weight: 400; font-size: 12px; }
  .ulist { display: grid; gap: 5px; grid-template-columns: repeat(auto-fill, minmax(300px, 1fr)); }
  .urow { display: flex; align-items: center; gap: 10px; background: #161b22;
          border: 1px solid #21262d; border-radius: 6px; padding: 6px 10px; font-size: 13px;
          cursor: pointer; transition: border-color .2s ease; }
  .urow:hover { border-color: #30363d; }
  .udom { flex: 1; font-family: ui-monospace, monospace; color: #c9d1d9; overflow: hidden;
          text-overflow: ellipsis; white-space: nowrap; }
  .usug { color: #7ee787; white-space: nowrap; flex: none; }
  .ucnt { color: #7d8590; font-variant-numeric: tabular-nums; flex: none; min-width: 34px; text-align: right; }
  .uempty { color: #7d8590; font-size: 13px; }

  .modal { position: fixed; inset: 0; background: rgba(0,0,0,.6); display: flex;
           align-items: center; justify-content: center; z-index: 50; }
  .modal.hidden { display: none; }
  .modalbox { background: #161b22; border: 1px solid #30363d; border-radius: 12px; padding: 20px;
              width: min(460px, 92vw); max-height: 88vh; overflow: auto; }
  .mhead { font-size: 15px; font-weight: 600; margin-bottom: 14px; }
  .mhead .mdom { font-family: ui-monospace, monospace; color: #7ee787; }
  .mlabel { font-size: 11px; text-transform: uppercase; letter-spacing: 1px; color: #7d8590; margin: 12px 0 6px; }
  .catalog { display: flex; flex-direction: column; gap: 6px; }
  .cchip { font-size: 13px; background: #21262d; border: 1px solid #30363d; border-radius: 20px;
           padding: 4px 11px; cursor: pointer; }
  .cchip:hover { border-color: #4c9aff; }
  .cchip.sel { background: #1f6feb; border-color: #1f6feb; color: #fff; }
  .mcat { border: 1px solid #21262d; border-radius: 8px; overflow: hidden; }
  .mcat > summary { cursor: pointer; padding: 8px 11px; font-size: 13px; color: #c9d1d9;
                    background: #1b2029; user-select: none; }
  .mcat > summary:hover { background: #21262d; }
  .mcat .mcatcount { color: #7d8590; font-size: 12px; }
  .mcatchips { display: flex; flex-wrap: wrap; gap: 6px; padding: 9px 11px; }
  .mgrid { display: flex; gap: 8px; }
  .mgrid input { flex: 1; }
  .modalbox input { background: #0d1117; border: 1px solid #30363d; border-radius: 6px; color: #e6edf3;
                    padding: 7px 10px; font-size: 14px; width: 100%; }
  .modalbox input.micon { flex: none; width: 64px; text-align: center; }
  .picks { display: flex; flex-wrap: wrap; gap: 5px; margin-top: 8px; }
  .pick { font-size: 18px; cursor: pointer; padding: 2px 4px; border-radius: 6px; }
  .pick:hover { background: #21262d; }
  .mmatch { display: flex; align-items: center; gap: 8px; margin-top: 10px; }
  .mmatch label { font-size: 12px; color: #7d8590; flex: none; }
  .mmsg { color: #f85149; font-size: 12px; min-height: 16px; margin-top: 8px; }
  .mactions { display: flex; justify-content: flex-end; gap: 8px; margin-top: 6px; }
  .mactions button { background: #21262d; border: 1px solid #30363d; color: #e6edf3; border-radius: 6px;
                     padding: 7px 14px; font-size: 13px; cursor: pointer; }
  .mactions button.primary { background: #238636; border-color: #238636; }
  .mactions button:hover { filter: brightness(1.15); }

  .tabs { display: flex; gap: 4px; }
  .tab { background: transparent; border: 1px solid #30363d; color: #adbac7; border-radius: 7px;
         padding: 5px 14px; font-size: 13px; cursor: pointer; }
  .tab.active { background: #1f6feb; border-color: #1f6feb; color: #fff; }
  .hidden { display: none; }

  .editor { padding: 22px; }
  .editbar { display: flex; align-items: center; gap: 12px; flex-wrap: wrap; margin-bottom: 16px; }
  .editbar > button { background: #21262d; border: 1px solid #30363d; color: #e6edf3; border-radius: 7px;
                      padding: 7px 12px; font-size: 13px; cursor: pointer; }
  .editactions { margin-left: auto; display: flex; align-items: center; gap: 10px; }
  .editactions .primary { background: #238636; border-color: #238636; }
  #svc_msg { color: #7d8590; font-size: 12px; }
  .palette { display: flex; flex-wrap: wrap; gap: 3px; }
  .palette .pick { font-size: 17px; }

  .svclist { display: flex; flex-direction: column; gap: 8px; }
  .svcrow { display: flex; align-items: flex-start; gap: 8px; background: #161b22; border: 1px solid #21262d;
            border-radius: 8px; padding: 10px; }
  .svcrow input { background: #0d1117; border: 1px solid #30363d; border-radius: 6px; color: #e6edf3;
                  padding: 6px 9px; font-size: 13px; }
  .svcicon { width: 46px; text-align: center; flex: none; font-size: 15px; }
  .svcname { width: 150px; flex: none; }
  .svcmatch { flex: 1; display: flex; flex-wrap: wrap; gap: 5px; align-items: center; }
  .tok { font-family: ui-monospace, monospace; font-size: 12px; background: #21262d; border-radius: 5px;
         padding: 2px 4px 2px 8px; display: inline-flex; align-items: center; gap: 4px; }
  .tok b { cursor: pointer; color: #7d8590; font-weight: 700; padding: 0 3px; }
  .tok b:hover { color: #f85149; }
  .svcaddtok { width: 120px; }
  .svcdel { background: transparent; border: none; cursor: pointer; font-size: 15px; flex: none; padding: 4px; }
  .svccat { background: #0d1117; border: 1px solid #30363d; border-radius: 6px; color: #e6edf3;
            padding: 6px 8px; font-size: 13px; flex: none; width: 150px; }
  .catgroup { border: 1px solid #21262d; border-radius: 8px; overflow: hidden; }
  .catgroup > summary { cursor: pointer; padding: 9px 12px; background: #1b2029; font-size: 14px;
                        font-weight: 600; color: #c9d1d9; user-select: none; }
  .catgroup > summary:hover { background: #21262d; }
  .caticon { width: 36px; text-align: center; background: #0d1117; border: 1px solid #30363d;
             border-radius: 5px; color: #e6edf3; font-size: 15px; padding: 3px; margin-right: 8px;
             vertical-align: middle; cursor: text; }
  .catdel { background: transparent; border: none; cursor: pointer; font-size: 13px; opacity: .55;
            vertical-align: middle; margin-left: 6px; }
  .catdel:hover { opacity: 1; }
  .noiseitem { cursor: pointer; }
  .catcount { color: #7d8590; font-size: 12px; font-weight: 400; }
  .catrows { padding: 8px; display: flex; flex-direction: column; gap: 8px; }

  .noisebox { margin-top: 22px; border-top: 1px solid #21262d; padding-top: 14px; }
  .noisebox summary { cursor: pointer; color: #adbac7; font-size: 13px; }
  .chips2 { display: flex; flex-wrap: wrap; gap: 5px; margin: 12px 0; }
  .addnoise { display: flex; gap: 6px; }
  .addnoise input { flex: 1; max-width: 300px; background: #0d1117; border: 1px solid #30363d;
                    border-radius: 6px; color: #e6edf3; padding: 6px 9px; font-size: 13px; }
  .addnoise button { background: #21262d; border: 1px solid #30363d; color: #e6edf3; border-radius: 6px;
                     padding: 6px 12px; cursor: pointer; }
</style>
</head>
<body>
<header>
  <h1>ACTIVITE RESEAU</h1>
  <nav class="tabs">
    <button class="tab active" data-view="activity">Activité</button>
    <button class="tab" data-view="services">Domaines Connus</button>
    <button class="tab" data-view="unknowns">Domaines Inconnus</button>
  </nav>
  <span id="status">connexion...</span>
</header>
<div id="view-activity">
<section class="summary">
  <div class="piewrap">
    <div class="pie" id="pieWeek"><span class="pietotal"><b>0</b></span></div>
    <div class="pielabel"></div>
  </div>
  <div class="piewrap">
    <div class="pie" id="pieDay"><span class="pietotal"><b>0</b></span></div>
    <div class="pielabel"></div>
  </div>
  <div class="legend" id="legend"></div>
  <div class="piewrap">
    <div class="pie" id="pieHour"><span class="pietotal"><b>0</b></span></div>
    <div class="pielabel"></div>
  </div>
  <div class="piewrap">
    <div class="pie" id="pieInst"><span class="pietotal"><b>0</b></span></div>
    <div class="pielabel"></div>
  </div>
</section>
<main id="grid"></main>
</div><!-- /view-activity -->
<div id="view-services" class="hidden">
  <div class="editor">
    <div class="editbar">
      <button id="svc_add">+ Nouveau service</button>
      <button id="cat_add">+ Catégorie</button>
      <div class="palette" id="svc_palette"></div>
      <div class="editactions">
        <span id="svc_msg"></span>
        <button id="svc_reload">Recharger</button>
        <button id="svc_save" class="primary">Enregistrer</button>
      </div>
    </div>
    <div id="svc_list" class="svclist"></div>
  </div>
</div>
<div id="view-unknowns" class="hidden">
  <div class="editor">
    <section class="unknowns">
      <h2>À classer <span>&mdash; les plus fréquents, clique pour mapper vers un service</span></h2>
      <div id="unknownlist" class="ulist"></div>
    </section>
    <details class="noisebox" open>
      <summary>Bruit masqué &mdash; domaines cachés (télémétrie, CDN…)</summary>
      <div id="noise_chips" class="chips2"></div>
      <div class="addnoise">
        <input id="noise_input" placeholder="ajouter un fragment à masquer (ex. doubleclick)">
        <button id="noise_add">+ ajouter</button>
        <button id="noise_save">Enregistrer</button>
        <span id="noise_msg"></span>
      </div>
    </details>
  </div>
</div>
<div id="mapmodal" class="modal hidden">
  <div class="modalbox">
    <div class="mhead">Mapper <span id="m_domain" class="mdom"></span></div>
    <div class="mlabel">Rattacher a un service existant</div>
    <div id="m_catalog" class="catalog"></div>
    <div class="mlabel">&hellip; ou créer un service <span style="text-transform:none;letter-spacing:0;opacity:.7;">(rangé dans « À trier »)</span></div>
    <div class="mgrid">
      <input id="m_name" type="text" placeholder="Nom du service">
      <input id="m_icon" type="text" class="micon" placeholder="&#127760;" maxlength="8">
    </div>
    <div id="m_picks" class="picks"></div>
    <div class="mmatch"><label>match</label><input id="m_match" type="text"></div>
    <div id="m_msg" class="mmsg"></div>
    <div class="mactions"><button id="m_noise" style="margin-right:auto;">Mettre en bruit</button><button id="m_cancel">Annuler</button><button id="m_ok" class="primary">Mapper</button></div>
  </div>
</div>
<script>
  const REFRESH = 3000;
  const grid = document.getElementById('grid');
  const statusEl = document.getElementById('status');
  const pieWeek = document.getElementById('pieWeek');
  const pieDay = document.getElementById('pieDay');
  const pieHour = document.getElementById('pieHour');
  const pieInst = document.getElementById('pieInst');
  const legend = document.getElementById('legend');
  const unknownlist = document.getElementById('unknownlist');
  const modal = document.getElementById('mapmodal');
  const mDomain = document.getElementById('m_domain');
  const mCatalog = document.getElementById('m_catalog');
  const mName = document.getElementById('m_name');
  const mIcon = document.getElementById('m_icon');
  const mMatch = document.getElementById('m_match');
  const mMsg = document.getElementById('m_msg');
  const cards = new Map();     // ip -> { el, els, chipSig }
  let lastOrder = '';
  let catalog = [];            // services actuels (payload) — proposés dans la fenêtre de mapping

  // Palette categorielle, couleurs faciles a distinguer (assignees dans l'ordre alphabetique).
  const PALETTE = ['#4c9aff','#36b37e','#ffab00','#ff5630','#8777d9','#00b8d9',
                   '#ff8b00','#57d9a3','#f062c0','#79f2c0','#998dd9','#ffc400',
                   '#c0b6f2','#6554c0','#ff7452','#4c6ef5'];
  const colorMap = new Map();
  let colorIdx = 0;
  function colorOf(ip){
    let c = colorMap.get(ip);
    if (!c){ c = PALETTE[colorIdx % PALETTE.length]; colorIdx++; colorMap.set(ip, c); }
    return c;
  }

  function esc(s){ return (s==null?'':String(s)).replace(/[&<>"]/g,
    c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c])); }
  function fmtAgo(s){ return s < 60 ? s + ' s' : Math.round(s/60) + ' min'; }
  function setText(node, text){ if (node.textContent !== text) node.textContent = text; }

  function makeCard(){
    const el = document.createElement('article');
    el.className = 'card enter';
    el.innerHTML = '<span class="dot"></span>'
      + '<div class="dev"></div>'
      + '<div class="cur"><span class="ic"></span><span class="svc"></span></div>'
      + '<div class="chips"></div>'
      + '<div class="ago"></div>';
    return { el, chipSig: '', els: {
      dot:   el.querySelector('.dot'),
      dev:   el.querySelector('.dev'),
      ic:    el.querySelector('.ic'),
      svc:   el.querySelector('.svc'),
      chips: el.querySelector('.chips'),
      ago:   el.querySelector('.ago'),
    }};
  }

  function updateCard(c, dev){
    c.els.dot.style.background = colorOf(dev.ip);
    setText(c.els.dev, dev.name);
    setText(c.els.ic, dev.current.icon || '');
    const guess = !!dev.current.guess;
    setText(c.els.svc, (guess ? '~ ' : '') + dev.current.name);
    c.els.svc.classList.toggle('guess', guess);
    const chipSig = (dev.services||[]).slice(1).map(s => s.icon + s.name).join('|');
    if (c.chipSig !== chipSig){
      c.els.chips.innerHTML = (dev.services||[]).slice(1)
        .map(s => '<span class="chip">'+(s.icon||'')+' '+esc(s.name)+'</span>').join('');
      c.chipSig = chipSig;
    }
    c.el.classList.toggle('idle', !!dev.idle);
    if (dev.idle) c.els.ago.textContent = 'inactif - vu il y a ' + fmtAgo(dev.ago);
    else c.els.ago.innerHTML = '<span class="live">&#9679; en direct</span>';
  }

  function donut(pieEl, devs, key, sub){
    const total = devs.reduce((a, d) => a + (d[key]||0), 0);
    setText(pieEl.querySelector('b'), sub);                         // fenetre au centre (7J/24H/1H/10s)
    const label = pieEl.parentElement.querySelector('.pielabel');
    if (label) setText(label, String(total));                       // compteur de requetes SOUS le donut
    if (!total){ pieEl.style.background = '#161b22'; return; }
    const sorted = devs.slice().filter(d => (d[key]||0) > 0).sort((a, b) => (b[key]||0) - (a[key]||0));
    let acc = 0; const stops = [];
    for (const d of sorted){
      const to = acc + (d[key]||0) / total;
      stops.push(colorOf(d.ip) + ' ' + (acc*100).toFixed(2) + '% ' + (to*100).toFixed(2) + '%');
      acc = to;
    }
    pieEl.style.background = 'conic-gradient(' + stops.join(',') + ')';
  }

  function render(d){
    const devs = d.devices || [];
    const week = d.week || [], day = d.day || [], hour = d.hour || [];
    catalog = d.catalog || [];

    // Union (appareils vus /7j /24h /1h + actifs) -> couleurs stables (ordre alpha) + legende.
    const merged = new Map();
    function bump(list, key){
      for (const x of list){
        const m = merged.get(x.ip) || { ip:x.ip, name:x.name, week:0, day:0, hour:0, rate:0 };
        m.name = x.name || m.name; m[key] = x.count||0; merged.set(x.ip, m);
      }
    }
    bump(week, 'week'); bump(day, 'day'); bump(hour, 'hour');
    for (const dev of devs){
      const m = merged.get(dev.ip) || { ip:dev.ip, name:dev.name, week:0, day:0, hour:0, rate:0 };
      m.name = dev.name || m.name; m.rate = dev.rate||0; merged.set(dev.ip, m);
    }
    const union = [...merged.values()].sort((a, b) =>
      a.name.toLowerCase().localeCompare(b.name.toLowerCase()) || a.ip.localeCompare(b.ip));
    for (const u of union) colorOf(u.ip);

    statusEl.textContent = devs.length + ' actif(s) - ' + union.length + ' vus sur 7 j';

    const seen = new Set();
    for (const dev of devs){
      seen.add(dev.ip);
      let c = cards.get(dev.ip);
      if (!c){
        c = makeCard();
        cards.set(dev.ip, c);
        grid.appendChild(c.el);
        requestAnimationFrame(() => requestAnimationFrame(() => c.el.classList.remove('enter')));
      }
      updateCard(c, dev);
    }
    for (const [ip, c] of cards){
      if (!seen.has(ip)){ c.el.remove(); cards.delete(ip); }
    }
    const order = devs.map(x => x.ip).join(',');
    if (order !== lastOrder){
      for (const dev of devs){ const c = cards.get(dev.ip); if (c) grid.appendChild(c.el); }
      lastOrder = order;
    }
    donut(pieWeek, week, 'count', '7J');
    donut(pieDay,  day,  'count', '24H');
    donut(pieHour, hour, 'count', '1H');
    donut(pieInst, devs, 'rate', (d.instant_window || 10) + 's');
    legend.innerHTML = union
      .filter(m => m.week > 0 || m.day > 0 || m.rate > 0)
      .sort((a, b) => b.day - a.day || b.week - a.week)
      .map(m => '<div class="leg"><span class="sw" style="background:'+colorOf(m.ip)+'"></span>'
        + '<span class="lname">'+esc(m.name)+'</span>'
        + '<span class="lval">'+m.day+'</span></div>').join('');

    const unk = d.unknown || [];
    unknownlist.innerHTML = unk.length
      ? unk.map(u => '<div class="urow" data-domain="'+esc(u.domain)+'" data-name="'+esc(u.suggest)+'"'
          + ' data-match="'+esc(u.match)+'" title="Cliquer : mapper ce domaine">'
          + '<span class="udom">'+esc(u.domain)+'</span>'
          + '<span class="usug">&rarr; '+esc(u.suggest)+'</span>'
          + '<span class="ucnt">'+u.count+'</span></div>').join('')
      : '<div class="uempty">Aucun domaine inconnu pour le moment.</div>';
  }

  // --- Fenetre de mapping : rattacher un domaine a un service existant ou en creer un ---
  // Deux modes : 'map' (onglet Activite -> POST /map) et 'noise' (onglet Services -> en memoire).
  let modalMode = 'map', modalNoiseIdx = -1, modalCatalog = [];
  const ICONS = ['🏠','🎮','📺','🎬','🎵','🎧','💬','🤖','📱','📷','🛒','🍥','⚽','📰','☁️','🌐'];
  document.getElementById('m_picks').innerHTML = ICONS.map(e => '<span class="pick">'+e+'</span>').join('');
  document.getElementById('m_picks').addEventListener('click', e => {
    if (e.target.classList.contains('pick')) mIcon.value = e.target.textContent;
  });
  mCatalog.addEventListener('click', e => {
    const chip = e.target.closest('.cchip');
    if (!chip) return;
    const s = modalCatalog[+chip.dataset.i];
    mName.value = s.name; mIcon.value = s.icon || '';
    mCatalog.querySelectorAll('.cchip').forEach(c => c.classList.remove('sel'));
    chip.classList.add('sel');
  });
  mName.addEventListener('input', () =>
    mCatalog.querySelectorAll('.cchip').forEach(c => c.classList.remove('sel')));

  // « À trier » toujours en dernier dans une liste de catégories.
  function _catOrder(cats){
    const o = cats.slice();
    const i = o.indexOf('À trier');
    if (i !== -1){ o.splice(i, 1); o.push('À trier'); }
    return o;
  }
  // Services groupés par catégorie en accordéons repliés (chips avec data-i = index dans
  // modalCatalog → le handler de clic ne change pas). CATEGORIES/CAT_ICON viennent de l'éditeur.
  function _renderCatalog(){
    const groups = new Map();
    modalCatalog.forEach((s, i) => {
      const c = s.category || 'À trier';
      if (!groups.has(c)) groups.set(c, []);
      groups.get(c).push(i);
    });
    const base = CATEGORIES.slice();
    for (const c of groups.keys()) if (!base.includes(c)) base.push(c);
    const html = _catOrder(base).filter(c => groups.has(c)).map(cat => {
      const chips = groups.get(cat).map(i =>
        '<span class="cchip" data-i="'+i+'">'+(modalCatalog[i].icon || '')+' '+esc(modalCatalog[i].name)+'</span>').join('');
      return '<details class="mcat"><summary>'+(CAT_ICON[cat] || '📁')+' '+esc(cat)
           + ' <span class="mcatcount">'+groups.get(cat).length+'</span></summary>'
           + '<div class="mcatchips">'+chips+'</div></details>';
    }).join('');
    mCatalog.innerHTML = html || '<span class="uempty">Aucun service pour le moment.</span>';
  }
  function _fillModal(domain, name, match){
    mDomain.textContent = domain;
    mName.value = name; mIcon.value = '🌐'; mMatch.value = match; mMsg.textContent = '';
    // « Mettre en bruit » n'a de sens que depuis un domaine inconnu (mode map), pas quand
    // on reclasse un fragment déjà dans le bruit vers un service (mode noise).
    document.getElementById('m_noise').style.display = (modalMode === 'map') ? '' : 'none';
    _renderCatalog();
    modal.classList.remove('hidden');
    mName.focus(); mName.select();
  }
  async function openModal(domain, name, match){   // depuis « Domaines Inconnus » (À classer) -> POST /map
    modalMode = 'map';
    await ensureEditor();                           // garantit services + catégories chargés (pour le groupage)
    modalCatalog = editServices;
    _fillModal(domain, name, match);
  }
  function openModalNoise(fragment, idx){          // onglet Services -> en memoire
    modalMode = 'noise'; modalNoiseIdx = idx; modalCatalog = editServices;
    _fillModal(fragment, '', fragment);
  }
  function closeModal(){ modal.classList.add('hidden'); }

  document.getElementById('m_cancel').addEventListener('click', closeModal);
  modal.addEventListener('click', e => { if (e.target === modal) closeModal(); });
  // « Mettre en bruit » : ajoute le fragment (match) au bruit, persiste, ferme. Le domaine
  // quitte « À classer » au tick suivant (reclassé en bruit côté serveur).
  document.getElementById('m_noise').addEventListener('click', async () => {
    const frag = mMatch.value.trim().toLowerCase();
    if (!frag){ mMsg.textContent = 'Fragment (match) requis.'; return; }
    if (!editNoise.includes(frag)) editNoise.push(frag);
    if (await saveEditor(mMsg)) closeModal();
  });
  document.getElementById('m_ok').addEventListener('click', async () => {
    const name = mName.value.trim(), icon = mIcon.value.trim(), match = mMatch.value.trim();
    if (!name || !match){ mMsg.textContent = 'Nom et match requis.'; return; }
    if (modalMode === 'noise'){                    // en memoire : sort le fragment du bruit -> service
      let svc = editServices.find(s => s.name.toLowerCase() === name.toLowerCase());
      if (!svc){ svc = { name: name, icon: icon || '🌐', category: 'À trier', match: [] }; editServices.unshift(svc); openCats.add('À trier'); }
      const frag = match.toLowerCase();
      if (!svc.match.includes(frag)) svc.match.push(frag);
      if (modalNoiseIdx >= 0) editNoise.splice(modalNoiseIdx, 1);
      renderEditor(); closeModal();
      return;
    }
    try {
      const r = await fetch('map', { method: 'POST', headers: { 'Content-Type': 'application/json' },
                                     body: JSON.stringify({ name, icon, match }) });
      const d = await r.json();
      if (d.ok){
        // Garder editServices en phase avec ce que /map vient d'écrire côté serveur,
        // sinon l'onglet « Domaines Connus » (chargé une seule fois) resterait périmé.
        const frag = match.toLowerCase();
        const svc = editServices.find(s => s.name.toLowerCase() === name.toLowerCase());
        if (svc){ if (!svc.match.includes(frag)) svc.match.push(frag); if (icon && !svc.icon) svc.icon = icon; }
        else editServices.unshift({ name: name, icon: icon || '🌐', category: 'À trier', match: [frag] });
        closeModal();
      } else mMsg.textContent = d.error || 'Erreur';
    } catch (e) { mMsg.textContent = 'Erreur reseau'; }
  });

  unknownlist.addEventListener('click', e => {
    const row = e.target.closest('.urow');
    if (!row || !row.dataset.name) return;
    openModal(row.dataset.domain, row.dataset.name, row.dataset.match);
  });

  // --- Onglets ---
  const views = { activity: document.getElementById('view-activity'),
                  services: document.getElementById('view-services'),
                  unknowns: document.getElementById('view-unknowns') };
  // L'éditeur (services + bruit) est partagé par les onglets « Domaines Connus » et
  // « Domaines Inconnus » : chargé UNE fois, re-rendu ensuite sans re-fetch (sinon on
  // écraserait les modifs non enregistrées en changeant d'onglet).
  let editorLoaded = false;
  async function ensureEditor(){ if (editorLoaded) { renderEditor(); return; } editorLoaded = true; await loadEditor(); }
  document.querySelectorAll('.tab').forEach(t => t.addEventListener('click', () => {
    document.querySelectorAll('.tab').forEach(x => x.classList.remove('active'));
    t.classList.add('active');
    for (const k in views) views[k].classList.toggle('hidden', k !== t.dataset.view);
    if (t.dataset.view === 'services' || t.dataset.view === 'unknowns') ensureEditor();
  }));

  // --- Editeur services.json (onglet Services) ---
  let editServices = [], editNoise = [], lastIcon = null;
  let categoriesList = [];              // [{name, icon}] chargées depuis /services et persistées
  let CATEGORIES = [], CAT_ICON = {}, openCats = new Set();
  function applyCategories(list){
    categoriesList = (list && list.length) ? list : [{ name: 'À trier', icon: '📁' }];
    CATEGORIES = categoriesList.map(c => c.name);
    CAT_ICON = {}; categoriesList.forEach(c => CAT_ICON[c.name] = c.icon || '📁');
    openCats = new Set();               // tout REPLIÉ à l'ouverture (les actions ré-ouvrent le groupe visé)
  }
  const svcList = document.getElementById('svc_list');
  const noiseChips = document.getElementById('noise_chips');
  const svcMsg = document.getElementById('svc_msg');

  function setCatIcon(name, emoji){
    const c = categoriesList.find(x => x.name === name);
    if (c) c.icon = emoji;
    CAT_ICON[name] = emoji;
  }

  document.getElementById('svc_palette').innerHTML = ICONS.map(e => '<span class="pick">'+e+'</span>').join('');
  document.getElementById('svc_palette').addEventListener('click', e => {
    if (!e.target.classList.contains('pick') || !lastIcon) return;
    lastIcon.value = e.target.textContent;
    const row = lastIcon.closest('.svcrow');
    if (row) editServices[+row.dataset.i].icon = lastIcon.value;
    else if (lastIcon.classList.contains('caticon')) setCatIcon(lastIcon.dataset.cat, lastIcon.value);
  });

  async function loadEditor(){
    svcMsg.textContent = 'chargement...';
    try {
      const r = await fetch('services');
      const d = await r.json();
      applyCategories(d.categories);
      editServices = (d.services || []).map(s => ({ name: s.name||'', icon: s.icon||'', category: s.category||'À trier', match: (s.match||[]).slice() }));
      editNoise = (d.noise || []).slice();
      renderEditor();
      svcMsg.textContent = '';
    } catch (e) { svcMsg.textContent = 'Erreur de chargement'; }
  }

  function rowHtml(s, i){
    const cur = s.category || 'À trier';
    const cats = CATEGORIES.includes(cur) ? CATEGORIES : CATEGORIES.concat([cur]);
    const opts = cats.map(c => '<option'+(c===cur?' selected':'')+'>'+esc(c)+'</option>').join('');
    return '<div class="svcrow" data-i="'+i+'">'
      + '<input class="svcicon" maxlength="8" value="'+esc(s.icon)+'">'
      + '<input class="svcname" placeholder="Nom" value="'+esc(s.name)+'">'
      + '<select class="svccat">'+opts+'</select>'
      + '<div class="svcmatch">'
        + s.match.map((m, j) => '<span class="tok">'+esc(m)+'<b data-rm="'+j+'">&times;</b></span>').join('')
        + '<input class="svcaddtok" placeholder="+ fragment (Entree)">'
      + '</div>'
      + '<button class="svcdel" title="Supprimer">&#128465;</button>'
      + '</div>';
  }

  function renderEditor(){
    // regroupe les index par catégorie, ordre défini puis extras
    const groups = new Map();
    editServices.forEach((s, i) => {
      const c = s.category || 'À trier';
      if (!groups.has(c)) groups.set(c, []);
      groups.get(c).push(i);
    });
    let order = CATEGORIES.slice();     // catégories définies puis extras éventuels
    for (const c of groups.keys()) if (!order.includes(c)) order.push(c);
    order = _catOrder(order);           // « À trier » toujours en dernier

    svcList.innerHTML = order.map(cat => {
      const idxs = groups.get(cat) || [];
      const rows = idxs.map(i => rowHtml(editServices[i], i)).join('');
      return '<details class="catgroup"'+(openCats.has(cat)?' open':'')+' data-cat="'+esc(cat)+'">'
        + '<summary><input class="caticon" maxlength="8" data-cat="'+esc(cat)+'" value="'+esc(CAT_ICON[cat]||'📁')+'"> '+esc(cat)+' <span class="catcount">'+idxs.length+'</span>'
        + (cat === 'À trier' ? '' : ' <button class="catdel" data-cat="'+esc(cat)+'" title="Supprimer la catégorie (ses services vont dans À trier)">&#128465;</button>')
        + '</summary>'
        + '<div class="catrows">'+rows+'</div></details>';
    }).join('') || '<div class="uempty">Aucun service. « + Nouveau service » pour commencer.</div>';

    svcList.querySelectorAll('.catgroup').forEach(dt => dt.addEventListener('toggle', () => {
      if (dt.open) openCats.add(dt.dataset.cat); else openCats.delete(dt.dataset.cat);
    }));

    noiseChips.innerHTML = editNoise.map((n, i) =>
      '<span class="tok noiseitem" data-i="'+i+'" title="Cliquer : reclasser en service">'+esc(n)+'<b data-ni="'+i+'">&times;</b></span>').join('')
      || '<span class="uempty">Aucun fragment de bruit.</span>';
  }

  svcList.addEventListener('input', e => {
    if (e.target.classList.contains('caticon')){ setCatIcon(e.target.dataset.cat, e.target.value); return; }
    const row = e.target.closest('.svcrow'); if (!row) return;
    const i = +row.dataset.i;
    if (e.target.classList.contains('svcname')) editServices[i].name = e.target.value;
    else if (e.target.classList.contains('svcicon')) editServices[i].icon = e.target.value;
  });
  svcList.addEventListener('focusin', e => {
    if (e.target.classList.contains('svcicon') || e.target.classList.contains('caticon')) lastIcon = e.target;
  });
  svcList.addEventListener('change', e => {
    if (!e.target.classList.contains('svccat')) return;
    const i = +e.target.closest('.svcrow').dataset.i;
    editServices[i].category = e.target.value;
    openCats.add(e.target.value);
    renderEditor();                       // la ligne rejoint sa nouvelle catégorie
  });
  svcList.addEventListener('keydown', e => {
    if (e.target.classList.contains('svcaddtok') && e.key === 'Enter'){
      const i = +e.target.closest('.svcrow').dataset.i;
      const v = e.target.value.trim().toLowerCase();
      if (v && !editServices[i].match.includes(v)) editServices[i].match.push(v);
      renderEditor();
    }
  });
  svcList.addEventListener('click', e => {
    if (e.target.classList.contains('caticon')){ e.preventDefault(); return; }   // éditer l'icône ne replie pas le groupe
    if (e.target.classList.contains('catdel')){
      e.preventDefault();
      const cat = e.target.dataset.cat;
      if (cat === 'À trier') return;
      if (!confirm('Supprimer la catégorie « ' + cat + ' » ? Ses services iront dans « À trier ».')) return;
      if (!CATEGORIES.includes('À trier')){ categoriesList.push({ name:'À trier', icon:'📁' }); CATEGORIES.push('À trier'); CAT_ICON['À trier']='📁'; }
      editServices.forEach(s => { if ((s.category||'À trier') === cat) s.category = 'À trier'; });
      categoriesList = categoriesList.filter(c => c.name !== cat);
      CATEGORIES = CATEGORIES.filter(c => c !== cat);
      delete CAT_ICON[cat];
      openCats.delete(cat); openCats.add('À trier');
      renderEditor();
      return;
    }
    const row = e.target.closest('.svcrow'); if (!row) return;
    const i = +row.dataset.i;
    if (e.target.classList.contains('svcdel')){ editServices.splice(i, 1); renderEditor(); }
    else if (e.target.dataset.rm !== undefined){ editServices[i].match.splice(+e.target.dataset.rm, 1); renderEditor(); }
  });
  noiseChips.addEventListener('click', e => {
    if (e.target.dataset.ni !== undefined){ editNoise.splice(+e.target.dataset.ni, 1); renderEditor(); return; }
    const tok = e.target.closest('.noiseitem');
    if (tok) openModalNoise(editNoise[+tok.dataset.i], +tok.dataset.i);
  });
  document.getElementById('noise_add').addEventListener('click', () => {
    const inp = document.getElementById('noise_input');
    const v = inp.value.trim().toLowerCase();
    if (v && !editNoise.includes(v)){ editNoise.push(v); inp.value = ''; renderEditor(); }
  });
  document.getElementById('svc_add').addEventListener('click', () => {
    editServices.unshift({ name: '', icon: '🌐', category: 'À trier', match: [] });
    openCats.add('À trier');
    renderEditor();
    const el = svcList.querySelector('.svcrow[data-i="0"] .svcname'); if (el) el.focus();
  });
  document.getElementById('cat_add').addEventListener('click', () => {
    const name = (prompt('Nom de la nouvelle catégorie ?') || '').trim();
    if (!name) return;
    if (CATEGORIES.includes(name)){ svcMsg.textContent = 'Catégorie « ' + name + ' » déjà existante'; return; }
    categoriesList.push({ name: name, icon: '📁' });
    CATEGORIES.push(name); CAT_ICON[name] = '📁'; openCats.add(name);
    renderEditor();
  });
  document.getElementById('svc_reload').addEventListener('click', loadEditor);
  // Un seul enregistrement pour tout (services + bruit + catégories, payload atomique) —
  // déclenchable depuis « Domaines Connus » (svc_save) ET « Domaines Inconnus » (noise_save).
  async function saveEditor(msgEl){
    msgEl.textContent = 'enregistrement...';
    try {
      const r = await fetch('services', { method: 'POST', headers: { 'Content-Type': 'application/json' },
                                          body: JSON.stringify({ services: editServices, noise: editNoise, categories: categoriesList }) });
      const d = await r.json();
      msgEl.textContent = d.ok ? ('Enregistre : '+d.services+' services, '+d.noise+' bruits') : (d.error || 'Erreur');
      return d.ok;
    } catch (e) { msgEl.textContent = 'Erreur reseau'; return false; }
  }
  document.getElementById('svc_save').addEventListener('click', () => saveEditor(svcMsg));
  document.getElementById('noise_save').addEventListener('click', () =>
    saveEditor(document.getElementById('noise_msg')));

  async function tick(){
    try {
      const r = await fetch('activity');
      const d = await r.json();
      if (d.error){ statusEl.textContent = 'Erreur AGH : ' + d.error; return; }
      render(d);
    } catch (e) {
      statusEl.textContent = 'Hors ligne : ' + e;
    }
  }
  tick();
  setInterval(tick, REFRESH);
</script>
</body>
</html>"""

app = Flask(__name__)

@app.get("/")
def index():
    return Response(PAGE, mimetype="text/html")

@app.get("/activity")
def activity():
    try:
        return jsonify(build_activity())
    except Exception as ex:
        traceback.print_exc()
        return jsonify({"error": str(ex), "devices": []}), 502

# Ajoute (ou complète) une entrée de services.json depuis un clic sur un domaine inconnu.
# Regroupe par NOM : mapper un 2e domaine sur un service existant enrichit son 'match'.
def _add_service(name, icon, match):
    with open(SERVICES_FILE, encoding="utf-8") as f:
        data = json.load(f)
    services = data.setdefault("services", [])
    for svc in services:
        if svc.get("name", "").strip().lower() == name.lower():
            toks = svc.setdefault("match", [])
            if match not in toks:
                toks.append(match)
            if icon and not svc.get("icon"):
                svc["icon"] = icon
            break
    else:
        services.append({"name": name, "icon": icon or "\U0001F310", "category": "À trier", "match": [match]})
    tmp = SERVICES_FILE + ".tmp"                      # écriture atomique (évite une lecture d'un fichier tronqué)
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    os.replace(tmp, SERVICES_FILE)

@app.post("/map")
def map_domain():
    try:
        body  = request.get_json(force=True) or {}
        name  = (body.get("name")  or "").strip()
        icon  = (body.get("icon")  or "").strip()
        match = (body.get("match") or "").strip().lower()
        if not name or not match:
            return jsonify({"error": "nom et match requis"}), 400
        _add_service(name, icon, match)
        return jsonify({"ok": True})
    except Exception as ex:
        traceback.print_exc()
        return jsonify({"error": str(ex)}), 500

DEFAULT_CATEGORIES = [
    {"name": "Vidéo / TV", "icon": "🎬"}, {"name": "Jeux", "icon": "🎮"},
    {"name": "Musique", "icon": "🎵"}, {"name": "Social / Messagerie", "icon": "💬"},
    {"name": "IA", "icon": "🤖"}, {"name": "Domotique", "icon": "🏠"},
    {"name": "Web / Système", "icon": "🌐"}, {"name": "À trier", "icon": "📁"},
]

@app.get("/services")
def get_services():
    try:
        with open(SERVICES_FILE, encoding="utf-8") as f:
            data = json.load(f)
        return jsonify({"services": data.get("services") or [], "noise": data.get("noise") or [],
                        "categories": data.get("categories") or DEFAULT_CATEGORIES})
    except Exception as ex:
        return jsonify({"error": str(ex), "services": [], "noise": [], "categories": DEFAULT_CATEGORIES}), 500

# Enregistre TOUT services.json depuis l'éditeur (onglet Services). Nettoie les entrées
# (nom + au moins un fragment), minusculise les fragments/bruit, préserve la clé _comment.
@app.post("/services")
def save_services():
    try:
        body = request.get_json(force=True) or {}
        services = []
        for s in body.get("services") or []:
            name = (s.get("name") or "").strip()
            icon = (s.get("icon") or "").strip()
            cat  = (s.get("category") or "À trier").strip() or "À trier"
            match = [m.strip().lower() for m in (s.get("match") or []) if m and m.strip()]
            if name and match:
                services.append({"name": name, "icon": icon or "\U0001F310", "category": cat, "match": match})
        noise = sorted({(n or "").strip().lower() for n in (body.get("noise") or []) if n and n.strip()})
        categories = []
        for c in body.get("categories") or []:
            cn = (c.get("name") or "").strip()
            if cn:
                categories.append({"name": cn, "icon": (c.get("icon") or "").strip() or "\U0001F4C1"})
        if not categories:
            categories = DEFAULT_CATEGORIES
        try:
            with open(SERVICES_FILE, encoding="utf-8") as f:
                existing = json.load(f)
        except Exception:
            existing = {}
        data = {}
        if existing.get("_comment"):
            data["_comment"] = existing["_comment"]
        data["categories"] = categories
        data["services"], data["noise"] = services, noise
        tmp = SERVICES_FILE + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
        os.replace(tmp, SERVICES_FILE)
        return jsonify({"ok": True, "services": len(services), "noise": len(noise)})
    except Exception as ex:
        traceback.print_exc()
        return jsonify({"error": str(ex)}), 500


# ----------------------------------------------------------------
# API PUBLIQUE
# ----------------------------------------------------------------
def main():
    log(f"AGH={AGH_URL}  actif={ACTIVE_S}s  presence={PRESENCE_S}s  port={ACTIVITY_PORT}")
    # t0 fige la frontière : le backfill couvre le passé (< t0), l'ingestion live le futur (> t0).
    t0 = time.time()
    _ingest["ts"] = t0
    threading.Thread(target=_backfill, args=(t0,), daemon=True).start()
    app.run(host="0.0.0.0", port=ACTIVITY_PORT, threaded=True)

if __name__ == "__main__":
    main()
