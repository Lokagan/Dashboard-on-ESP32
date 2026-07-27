#!/usr/bin/env python3
"""
freebox_get_token.py — Obtention du token d'authentification Freebox

Ce script est à exécuter UNE SEULE FOIS pour obtenir l'app_token
qui sera ensuite placé dans monitor.env (variable APP_TOKEN).

Usage :
    python3 freebox_get_token.py

Prérequis :
    pip install requests
"""

# ----------------------------------------------------------------
# BIBLIOTHÈQUES
# ----------------------------------------------------------------
import time
import hmac
import hashlib
import requests

# ----------------------------------------------------------------
# OBJETS GLOBAUX
# ----------------------------------------------------------------
FREEBOX_HOST = "192.168.1.254"
FREEBOX_API  = "v6"
APP_ID       = "dashboard.esp32"
APP_NAME     = "Dashboard ESP32"
APP_VERSION  = "1.0"
DEVICE_NAME  = "NAS Monitor"

FREEBOX_URL  = f"http://{FREEBOX_HOST}/api/{FREEBOX_API}"

# ----------------------------------------------------------------
# API LOCALES
# ----------------------------------------------------------------

# --- Étape 1 — Vérification de l'API Freebox ---
def check_api():
    print("\n[1/4] Vérification de l'API Freebox...")
    try:
        r = requests.get(f"http://{FREEBOX_HOST}/api_version", timeout=5)
        data = r.json()
        print(f"  Modèle   : {data.get('box_model_name', '?')}")
        print(f"  API      : v{data.get('api_version', '?')}")
        print(f"  Appareil : {data.get('device_name', '?')}")
        return True
    except Exception as e:
        print(f"  ERREUR : impossible de joindre la Freebox ({e})")
        print(f"  Vérifiez que FREEBOX_HOST={FREEBOX_HOST} est correct.")
        return False

# --- Étape 2 — Demande d'enregistrement de l'application ---
def request_authorization():
    print("\n[2/4] Demande d'autorisation...")
    r = requests.post(f"{FREEBOX_URL}/login/authorize/", json={
        "app_id":      APP_ID,
        "app_name":    APP_NAME,
        "app_version": APP_VERSION,
        "device_name": DEVICE_NAME
    }, timeout=10)
    data = r.json()
    if not data.get("success"):
        print(f"  ERREUR : {data}")
        return None, None

    app_token = data["result"]["app_token"]
    track_id  = data["result"]["track_id"]
    print(f"  app_token : {app_token[:16]}...")
    print(f"  track_id  : {track_id}")
    return app_token, track_id

# --- Étape 3 — Attente de la validation sur la Freebox ---
def wait_for_grant(track_id):
    print("\n[3/4] ⚠️  APPUYEZ SUR LE BOUTON ▶ DE LA FREEBOX pour autoriser !")
    print("       (vous avez 30 secondes)\n")

    for i in range(30):
        time.sleep(1)
        r = requests.get(f"{FREEBOX_URL}/login/authorize/{track_id}", timeout=5)
        status = r.json().get("result", {}).get("status", "unknown")

        # Affichage progression
        print(f"  Statut : {status} {'.' * (i % 4)}", end="\r")

        if status == "granted":
            print("\n  ✅ Autorisation accordée !")
            return True
        elif status == "denied":
            print("\n  ❌ Autorisation refusée.")
            return False
        elif status == "timeout":
            print("\n  ⏱  Délai expiré.")
            return False

    print("\n  ⏱  Délai expiré.")
    return False

# --- Étape 4 — Vérification du token par ouverture de session ---
def verify_token(app_token):
    print("\n[4/4] Vérification du token...")

    # Récupérer le challenge
    r = requests.get(f"{FREEBOX_URL}/login/", timeout=10)
    challenge = r.json()["result"]["challenge"]

    # Calculer le password
    password = hmac.new(
        app_token.encode(),
        challenge.encode(),
        hashlib.sha1
    ).hexdigest()

    # Ouvrir la session
    r2 = requests.post(f"{FREEBOX_URL}/login/session/", json={
        "app_id":   APP_ID,
        "password": password
    }, timeout=10)
    data = r2.json()

    if data.get("success"):
        perms = data["result"].get("permissions", {})
        print("  ✅ Token valide ! Permissions accordées :")
        for perm, val in perms.items():
            if val:
                print(f"     ✓ {perm}")
        return True
    else:
        print(f"  ❌ Erreur validation : {data}")
        return False

# ----------------------------------------------------------------
# API PUBLIQUES
# ----------------------------------------------------------------
def main():
    print("=" * 55)
    print("  Freebox Token Generator — Dashboard ESP32")
    print("=" * 55)

    # Étape 1
    if not check_api():
        return

    # Étape 2
    app_token, track_id = request_authorization()
    if not app_token:
        return

    # Étape 3
    if not wait_for_grant(track_id):
        return

    # Étape 4
    if not verify_token(app_token):
        return

    # Résultat final
    print("\n" + "=" * 55)
    print("  ✅ SUCCÈS — Copie cette ligne dans monitor.env :")
    print("=" * 55)
    print(f"\nAPP_TOKEN={app_token}\n")
    print("=" * 55)

if __name__ == "__main__":
    main()