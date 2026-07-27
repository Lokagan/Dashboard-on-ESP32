#!/usr/bin/env python3
"""
freebox_monitor.py — Collecte les métriques Freebox v8 et publie sur MQTT
Validé sur Freebox OS API v6

Topics MQTT publiés :
  freebox/rate_down       float   Mb/s débit descendant actuel
  freebox/rate_up         float   Mb/s débit montant actuel
  freebox/bandwidth_down  int     Mb/s bande passante max descendante
  freebox/bandwidth_up    int     Mb/s bande passante max montante
  freebox/state           string  état de la connexion
  freebox/ipv4            string  IP publique courante
  freebox/devices_active  int     appareils joignables
  freebox/devices_total   int     appareils connus au total
  freebox/devices         JSON    liste des appareils connectés avec détails
"""

# ----------------------------------------------------------------
# RESSOURCES BIBLIOTHÈQUES
# ----------------------------------------------------------------
import os
import json
import time
import hmac
import hashlib
import traceback
import requests
import paho.mqtt.client as mqtt
from paho.mqtt.enums import CallbackAPIVersion
import activity_monitor

# ----------------------------------------------------------------
# OBJETS GLOBAUX
# ----------------------------------------------------------------
MQTT_BROKER      = os.getenv("MQTT_BROKER",  "192.168.1.1")
MQTT_PORT        = int(os.getenv("MQTT_PORT", "1883"))
FREEBOX_HOST     = os.getenv("FREEBOX_HOST",  "192.168.1.254")
FREEBOX_API      = os.getenv("FREEBOX_API",   "v6")
APP_ID           = os.getenv("APP_ID",        "dashboard.esp32")
APP_TOKEN        = os.getenv("APP_TOKEN",     "")
REFRESH_INTERVAL = int(os.getenv("REFRESH_INTERVAL", "30"))

# freebox/devices est le plus gros payload du projet (jusqu'à 4 Ko, d'où le
# buffer PubSubClient à 4096 côté ESP32) et n'est publié qu'un cycle sur N :
# PubSubClient lit le socket octet par octet, ce message bloquait donc
# loopTask des dizaines de ms à chaque cycle. La liste des appareils du réseau
# bouge lentement ; les débits, eux, gardent la cadence pleine.
BULK_EVERY_N = int(os.getenv("BULK_EVERY_N", "6"))
_cycle = 0

FREEBOX_URL   = f"http://{FREEBOX_HOST}/api/{FREEBOX_API}"
http          = requests.Session()
session_token = None

_TYPE_ORDER = {"ETH": 0, "5G": 1, "2.4G": 2}

# ----------------------------------------------------------------
# HELPERS
# ----------------------------------------------------------------

# --- Classement des appareils — type abrégé + ordre d'affichage ---
def _abbrev_type(conn_type, band):
    t = (conn_type or "").lower()
    b = (band or "").lower()

    if t.startswith("ethernet") or t.startswith("eth"):
        return "ETH"
    if t.startswith("wifi"):
        if b.startswith("5g"):
            return "5G"
        if b.startswith("2d4g"):
            return "2.4G"
        return "WiFi"
    if t.startswith("vpn"):
        return "VPN"
    return conn_type or "--"

def _type_order(abbrev_type):
    return _TYPE_ORDER.get(abbrev_type, 3)

# Octets -> chaîne lisible auto-échelle. Le TRI se fait sur les octets bruts en
# amont, jamais sur cette chaîne (sinon "9 KB" > "1 MB" en tri lexical).
def _fmt_bytes(n):
    n = int(n or 0)
    if n >= 1 << 30: return f"{n / (1 << 30):.1f} GB"
    if n >= 1 << 20: return f"{n / (1 << 20):.1f} MB"
    if n >= 1 << 10: return f"{n / (1 << 10):.0f} KB"
    return f"{n} B"

def headers():
    return {"X-Fbx-App-Auth": session_token}

# ----------------------------------------------------------------
# API LOCALES
# ----------------------------------------------------------------

# --- Authentification ---
def freebox_login():
    global session_token

    if not APP_TOKEN:
        print("[Freebox] ERREUR : APP_TOKEN non défini !")
        return False

    try:
        r = http.get(f"{FREEBOX_URL}/login/", timeout=10)
        data = r.json()
        if not data.get("success"):
            print(f"[Freebox] Erreur challenge : {data}")
            return False

        challenge = data["result"]["challenge"]
        password  = hmac.new(
            APP_TOKEN.encode(),
            challenge.encode(),
            hashlib.sha1
        ).hexdigest()

        r2 = http.post(f"{FREEBOX_URL}/login/session/", json={
            "app_id":   APP_ID,
            "password": password
        }, timeout=10)
        data2 = r2.json()

        if data2.get("success"):
            session_token = data2["result"]["session_token"]
            print("[Freebox] Session ouverte OK")
            return True

        print(f"[Freebox] Erreur login : {data2}")
        return False

    except Exception as e:
        print(f"[Freebox] Erreur login : {e}")
        return False

# --- Collecte — connexion ---
def get_connection():
    r = http.get(f"{FREEBOX_URL}/connection/", headers=headers(), timeout=10)
    data = r.json()
    if not data.get("success") and data.get("error_code") == "auth_required":
        print("[Freebox] Session expirée, reconnexion...")
        if freebox_login():
            r = http.get(f"{FREEBOX_URL}/connection/", headers=headers(), timeout=10)
            data = r.json()
    return data["result"] if data.get("success") else None

# --- Collecte — appareils réseau ---
def get_devices():
    """Retourne (actifs, total, liste) — ou (None, None, None) si l'appel
    échoue, pour ne pas publier des compteurs à zéro sur une erreur
    transitoire : le dashboard affichait « 0 appareil » réseau intact."""
    r = http.get(f"{FREEBOX_URL}/lan/browser/pub/",
                 headers=headers(), timeout=15)
    data = r.json()

    if not data.get("success"):
        return None, None, None

    devices     = data["result"]
    active_list = []

    for d in devices:
        if not d.get("reachable"):
            continue

        ap        = d.get("access_point", {})
        wifi_info = ap.get("wifi_information", {})

        # Récupérer l'IP IPv4 active
        ip = ""
        for l3 in d.get("l3connectivities", []):
            if l3.get("active") and l3.get("af") == "ipv4":
                ip = l3.get("addr", "")
                break

        conn_type = ap.get("connectivity_type", "")   # wifi / ethernet
        band      = wifi_info.get("band", "")         # 2g / 5g / 6g

        active_list.append({
            "name":        d.get("primary_name", d.get("default_name", "?")).strip(),
            "ip":          ip,
            "vendor":      d.get("vendor_name", ""),
            "type":        _abbrev_type(conn_type, band),     # déjà abrégé : ETH/5G/2.4G/WiFi/VPN
            "signal":      wifi_info.get("signal", 0),        # dBm
            "phy_rx_rate": wifi_info.get("phy_rx_rate", 0),   # Mbps liaison down
            "phy_tx_rate": wifi_info.get("phy_tx_rate", 0),   # Mbps liaison up
            "rx_rate":     ap.get("rx_rate", 0),              # bytes/s trafic down (brut, formaté après tri)
            "tx_rate":     ap.get("tx_rate", 0),              # bytes/s trafic up
        })

    # Tri d'affichage : débit down INSTANTANÉ décroissant, puis up, puis type
    # (ETH, 5G, 2.4G, reste), puis IP. Sur les octets/s BRUTS (avant formatage).
    active_list.sort(key=lambda d: (-d["rx_rate"], -d["tx_rate"],
                                    _type_order(d["type"]), d["ip"]))
    # Puis on formate les débits avec unité (le tri a déjà eu lieu sur le brut) et on
    # ajoute le service courant (DNS) par IP — un souci côté DNS ne casse jamais la liste.
    try:
        svc = activity_monitor.current_service_names()
    except Exception:
        svc = {}
    for d in active_list:
        d["rx_rate"] = _fmt_bytes(d["rx_rate"]) + "/s"
        d["tx_rate"] = _fmt_bytes(d["tx_rate"]) + "/s"
        d["service"] = svc.get(d["ip"], "")

    return len(active_list), len(devices), active_list

# --- Publication MQTT ---
def publish_metrics(client):
    global _cycle
    _cycle += 1
    bulk = (_cycle % BULK_EVERY_N == 1)   # 1er cycle inclus : pas d'écran vide au démarrage

    try:
        # --- Connexion ---
        conn = get_connection()
        if not conn:
            print("[Freebox] Connexion indisponible")
            return

        rate_down = round(conn["rate_down"] * 8 / 1_000_000, 1)
        rate_up   = round(conn["rate_up"]   * 8 / 1_000_000, 1)
        bw_down   = int(conn["bandwidth_down"] / 1_000_000)
        bw_up     = int(conn["bandwidth_up"]   / 1_000_000)
        state     = conn.get("state", "unknown")
        ipv4      = conn.get("ipv4",  "")

        client.publish("freebox/rate_down",      str(rate_down))
        client.publish("freebox/rate_up",        str(rate_up))
        client.publish("freebox/bandwidth_down", str(bw_down))
        client.publish("freebox/bandwidth_up",   str(bw_up))
        client.publish("freebox/state",          state)
        client.publish("freebox/ipv4",           ipv4)

        # --- Appareils ---
        # Les compteurs (petits) à chaque cycle ; la LISTE complète (jusqu'à
        # 4 Ko) un cycle sur BULK_EVERY_N. Rien n'est publié si l'appel a
        # échoué, sinon un incident transitoire afficherait « 0 appareil ».
        active, total, device_list = get_devices()
        if active is not None:
            client.publish("freebox/devices_active", str(active))
            client.publish("freebox/devices_total",  str(total))
            if bulk:
                client.publish("freebox/devices", json.dumps(device_list))

    except requests.exceptions.RequestException as e:
        print(f"[Freebox] Erreur réseau : {e}")
        freebox_login()
    except Exception as e:
        print(f"[Freebox] Erreur inattendue : {e}")
        traceback.print_exc()

# ----------------------------------------------------------------
# API PUBLIQUES
# ----------------------------------------------------------------
def main():
    print(f"[Freebox Monitor] Démarrage — {FREEBOX_HOST} → MQTT {MQTT_BROKER}:{MQTT_PORT}")

    if not APP_TOKEN:
        print("[Freebox] ERREUR : APP_TOKEN non défini !")
        return

    mqtt_client = mqtt.Client(
        callback_api_version=CallbackAPIVersion.VERSION2,
        client_id="freebox-monitor"
    )

    def on_connect(client, userdata, flags, reason_code, properties):
        if reason_code == 0:
            print("[Freebox] MQTT connecté")
        else:
            print(f"[Freebox] MQTT erreur : {reason_code}")

    def on_disconnect(client, userdata, flags, reason_code, properties):
        print(f"[Freebox] MQTT déconnecté : {reason_code}")

    mqtt_client.on_connect    = on_connect
    mqtt_client.on_disconnect = on_disconnect

    try:
        mqtt_client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
    except Exception as e:
        print(f"[Freebox] MQTT impossible de se connecter : {e}")
        return

    mqtt_client.loop_start()

    while not freebox_login():
        print(f"[Freebox] Nouvelle tentative dans {REFRESH_INTERVAL}s...")
        time.sleep(REFRESH_INTERVAL)

    while True:
        publish_metrics(mqtt_client)
        time.sleep(REFRESH_INTERVAL)

if __name__ == "__main__":
    main()
