#!/usr/bin/env python3
"""
nas_monitor.py — Collecte les métriques Synology DS1522+ et publie sur MQTT
Validé sur DSM 7.3.2

Topics MQTT publiés :
  nas/cpu               int     % charge CPU (user+system+other)
  nas/ram               int     % RAM utilisée
  nas/temp              int     °C température système
  nas/net_rx            int     KB/s trafic entrant
  nas/net_tx            int     KB/s trafic sortant
  nas/volume1_used_pct  int     % espace utilisé volume1
  nas/volume1_status    string  état du volume1
  nas/volume1_read_mbs  float   MB/s lecture volume1
  nas/volume1_write_mbs float   MB/s écriture volume1
  nas/disks             JSON    état des disques (trié par num_id)
  nas/downloads         JSON    top 10 torrents par vitesse upload
  nas/connections       JSON    connexions utilisateurs

Convention des collecteurs : None = échec de l'appel API, [] = réellement
vide. Sans cette distinction, une erreur transitoire vidait l'affichage.
"""

# ----------------------------------------------------------------
# RESSOURCES BIBLIOTHÈQUES
# ----------------------------------------------------------------
import os
import sys
import time
import json
import traceback
import requests
import paho.mqtt.client as mqtt
from paho.mqtt.enums import CallbackAPIVersion
from ipaddress import ip_address, ip_network

# ----------------------------------------------------------------
# OBJETS GLOBAUX
# ----------------------------------------------------------------
MQTT_BROKER      = os.getenv("MQTT_BROKER",      "192.168.1.1")
MQTT_PORT        = int(os.getenv("MQTT_PORT",     "1883"))
NAS_HOST         = os.getenv("NAS_HOST",          "192.168.1.1")
NAS_PORT         = os.getenv("NAS_PORT",          "5000")
NAS_USER         = os.getenv("NAS_USER")
NAS_PASSWORD     = os.getenv("NAS_PASSWORD")
REFRESH_INTERVAL = int(os.getenv("REFRESH_INTERVAL", "30"))

# Les payloads volumineux (disques, downloads, connexions — jusqu'à 4 Ko) ne
# sont publiés qu'un cycle sur N : côté ESP32, PubSubClient lit le socket OCTET
# PAR OCTET, donc un message de 4 Ko coûte ~4096 lectures et bloquait loopTask
# 21 à 79 ms par cycle. Ces trois listes bougent lentement ; les métriques
# rapides gardent la cadence pleine. À 1 = comportement historique.
BULK_EVERY_N = int(os.getenv("BULK_EVERY_N", "6"))
_cycle = 0

if not NAS_USER or not NAS_PASSWORD:
    print("[NAS] ERREUR : NAS_USER ou NAS_PASSWORD non défini !")
    sys.exit(1)

NAS_URL = f"http://{NAS_HOST}:{NAS_PORT}/webapi"

http = requests.Session()
sid  = None

# ----------------------------------------------------------------
# API LOCALES
# ----------------------------------------------------------------

# --- Authentification ---
def nas_login():
    global sid
    print(f"[NAS] Connexion à l'API DSM ({NAS_HOST})...")
    try:
        r = http.get(f"{NAS_URL}/auth.cgi", params={
            "api":     "SYNO.API.Auth",
            "version": "3",
            "method":  "login",
            "account": NAS_USER,
            "passwd":  NAS_PASSWORD,
            "session": "nas_monitor",
            "format":  "sid"
        }, timeout=10)
        data = r.json()
        if data.get("success"):
            sid = data["data"]["sid"]
            print("[NAS] Connecté OK")
            return True
        print(f"[NAS] Erreur auth : {data}")
    except Exception as e:
        print(f"[NAS] Erreur login : {e}")
    return False

# --- Appel API générique (code 119 = session expirée -> une reconnexion) ---
def api(params, timeout=10):
    global sid
    params["_sid"] = sid
    r = http.get(f"{NAS_URL}/entry.cgi", params=params, timeout=timeout)
    data = r.json()
    if not data.get("success") and data.get("error", {}).get("code") == 119:
        print("[NAS] Session expirée, reconnexion...")
        if nas_login():
            params["_sid"] = sid
            r = http.get(f"{NAS_URL}/entry.cgi", params=params, timeout=timeout)
            data = r.json()
    return data

# --- Collecte — None si l'appel échoue (cf. docstring) ---
def get_system():
    data = api({"api": "SYNO.Core.System", "version": "1", "method": "info"})
    return data["data"] if data.get("success") else None

def get_utilization():
    data = api({"api": "SYNO.Core.System.Utilization", "version": "1", "method": "get"})
    return data["data"] if data.get("success") else None

def get_storage():
    data = api({"api": "SYNO.Storage.CGI.Storage", "version": "1", "method": "load_info"})
    return data["data"] if data.get("success") else None

def get_downloads():
    r = http.get(f"{NAS_URL}/DownloadStation/task.cgi", params={
        "api":        "SYNO.DownloadStation.Task",
        "version":    "1",
        "method":     "list",
        "additional": "transfer",
        "_sid":       sid
    }, timeout=10)
    data = r.json()
    return data["data"].get("tasks", []) if data.get("success") else None

def get_connections():
    data = api({"api": "SYNO.Core.CurrentConnection", "version": "1", "method": "list"})
    return data["data"].get("items", []) if data.get("success") else None

# --- Mise en forme des payloads volumineux ---
def build_disks(storage):
    disks = [{
        "name":         d.get("name", d["id"]),
        "num_id":       d.get("num_id", 0),
        "model":        d.get("model", ""),
        "smart_status": d.get("smart_status", "unknown"),
        "status":       d.get("status", "unknown"),
        "temp":         d.get("temp", 0),
    } for d in storage["disks"]]
    return sorted(disks, key=lambda d: d["num_id"])

# Octets -> chaîne lisible auto-échelle. ⚠️ Miroir de _fmt_bytes dans
# freebox_monitor.py (scripts séparés) : garder les deux alignés.
def _fmt_bytes(n):
    n = int(n or 0)
    if n >= 1 << 30: return f"{n / (1 << 30):.1f} GB"
    if n >= 1 << 20: return f"{n / (1 << 20):.1f} MB"
    if n >= 1 << 10: return f"{n / (1 << 10):.0f} KB"
    return f"{n} B"

def build_downloads(tasks):
    # Tri : téléchargements actifs d'abord (par débit descendant), puis les
    # seeders (par débit d'upload), puis par ratio. (sur les octets BRUTS)
    top = sorted(tasks, key=lambda t: (
        t["additional"]["transfer"]["speed_download"],
        t["additional"]["transfer"]["speed_upload"],
        t["additional"]["transfer"]["size_uploaded"] / t["size"] if t["size"] > 0 else 0,
    ), reverse=True)[:10]

    return [{
        "title":          t["title"],
        "status":         t["status"],
        "speed_upload":   _fmt_bytes(t["additional"]["transfer"]["speed_upload"]) + "/s",
        "speed_download": _fmt_bytes(t["additional"]["transfer"]["speed_download"]) + "/s",
        "size_uploaded":  _fmt_bytes(t["additional"]["transfer"]["size_uploaded"]),  # octets -> "1.2 GB"
        "ratio": round(t["additional"]["transfer"]["size_uploaded"] / t["size"]
                       if t["size"] > 0 else 0, 2),
    } for t in top]

def build_connections(connections):
    # Les conteneurs Docker (172.16/12) ne sont pas des connexions utilisateur.
    docker_net = ip_network("172.16.0.0/12")
    kept = sorted(
        (c for c in connections if ip_address(c.get("from", "0.0.0.0")) not in docker_net),
        key=lambda c: ip_address(c.get("from", "0.0.0.0")))

    return [{
        "user":      c.get("who", ""),
        "from":      c.get("from", ""),
        "service":   c.get("descr", ""),
        "protocol":  c.get("protocol", ""),
        "time":      c.get("time", ""),
        "connected": c.get("is_current_connected", False),
    } for c in kept]

# --- Publication MQTT ---
def publish_metrics(client):
    global _cycle
    _cycle += 1
    bulk = (_cycle % BULK_EVERY_N == 1)   # 1er cycle inclus : pas d'écran vide au démarrage

    try:
        util    = get_utilization()
        system  = get_system() or {}
        storage = get_storage()

        if not util:
            print("[NAS] Utilization indisponible — reconnexion...")
            nas_login()
            return

        cpu = (util["cpu"]["user_load"]
             + util["cpu"]["system_load"]
             + util["cpu"]["other_load"])
        ram = util["memory"]["real_usage"]

        client.publish("nas/cpu",    str(cpu))
        client.publish("nas/ram",    str(ram))
        client.publish("nas/temp",   str(system.get("sys_temp", 0)))
        client.publish("nas/net_rx", str(util["network"][0]["rx"] // 1024))   # B/s -> KB/s
        client.publish("nas/net_tx", str(util["network"][0]["tx"] // 1024))

        # --- Volume 1 ---
        if storage and storage.get("volumes"):
            vol = storage["volumes"][0]
            vol_total = int(vol["size"]["total"])
            vol_used  = int(vol["size"]["used"])
            vol_pct   = int(vol_used / vol_total * 100) if vol_total > 0 else 0

            vol_io = util.get("space", {}).get("volume", [{}])[0]

            client.publish("nas/volume1_used_pct",  str(vol_pct))
            client.publish("nas/volume1_status",    vol.get("status", "unknown"))
            client.publish("nas/volume1_read_mbs",  str(round(vol_io.get("read_byte",  0) / 1_000_000, 2)))
            client.publish("nas/volume1_write_mbs", str(round(vol_io.get("write_byte", 0) / 1_000_000, 2)))

        # --- Payloads volumineux (cf. BULK_EVERY_N) ---
        # `is not None` et non `if x` : une liste VIDE est une information
        # légitime (plus aucun torrent, plus aucune connexion) qu'il faut
        # publier, alors qu'un échec d'API ne doit rien écraser.
        if bulk:
            if storage and storage.get("disks"):
                client.publish("nas/disks", json.dumps(build_disks(storage)))

            tasks = get_downloads()
            if tasks is not None:
                client.publish("nas/downloads", json.dumps(build_downloads(tasks)))

            connections = get_connections()
            if connections is not None:
                client.publish("nas/connections", json.dumps(build_connections(connections)))

    except requests.exceptions.RequestException as e:
        print(f"[NAS] Erreur réseau : {e}")
        nas_login()
    except Exception as e:
        print(f"[NAS] Erreur inattendue : {e}")
        traceback.print_exc()

# ----------------------------------------------------------------
# API PUBLIQUES
# ----------------------------------------------------------------
def main():
    print(f"[NAS Monitor] Démarrage — {NAS_HOST}:{NAS_PORT} → MQTT {MQTT_BROKER}:{MQTT_PORT}")

    mqtt_client = mqtt.Client(callback_api_version=CallbackAPIVersion.VERSION2,
                              client_id="nas-monitor")

    def on_connect(client, userdata, flags, reason_code, properties):
        print("[NAS] MQTT connecté" if reason_code == 0 else f"[NAS] MQTT erreur : {reason_code}")

    def on_disconnect(client, userdata, flags, reason_code, properties):
        print(f"[NAS] MQTT déconnecté : {reason_code}")

    mqtt_client.on_connect    = on_connect
    mqtt_client.on_disconnect = on_disconnect

    try:
        mqtt_client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
    except Exception as e:
        print(f"[NAS] MQTT impossible de se connecter : {e}")
        return

    mqtt_client.loop_start()

    while not nas_login():
        print(f"[NAS] Nouvelle tentative dans {REFRESH_INTERVAL}s...")
        time.sleep(REFRESH_INTERVAL)

    while True:
        publish_metrics(mqtt_client)
        time.sleep(REFRESH_INTERVAL)

if __name__ == "__main__":
    main()
