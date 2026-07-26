# Notes de tests — commandes utiles
# À lancer depuis synology/ (monitor.env y est, les scripts dans scripts/).

*Test du freebox_monitor
set -a && source monitor.env && set +a && python3 scripts/freebox_monitor.py
mosquitto_sub -h 192.168.1.1 -t 'freebox/#' -v

*Test du nas_monitor
set -a && source monitor.env && set +a && python3 scripts/nas_monitor.py
mosquitto_sub -h 192.168.1.1 -t 'nas/#' -v

*Test du bridge IA
# bridge_monitor.py lit bridge_defaults.json via un chemin conteneur (/app/scripts/…).
# En local, forcer les chemins sinon il log FATAL et ne démarre pas.
set -a && source monitor.env && set +a && \
  BRIDGE_DEFAULTS_FILE=scripts/bridge_defaults.json \
  BRIDGE_SETTINGS_FILE=scripts/bridge_settings.json \
  python3 scripts/bridge_monitor.py
mosquitto_sub -h 192.168.1.1 -t 'ai/#' -v


# --- Piloter l'écran Companion via MQTT ---

# ai/status : SORTANT côté ESP32 — il le publie mais ne s'y abonne PAS.
# Publier ici n'affecte QUE des observateurs tiers (Home Assistant), jamais l'écran.
mosquitto_pub -h 192.168.1.1 -t ai/status -m "listening"

# Poser une question en TEXTE (l'ESP32 saute le STT -> LLM + TTS).
# Anti-doublon : ignoré si < 2 s après la précédente, ou si l'IA est occupée.
mosquitto_pub -h 192.168.1.1 -t ai/ask -m "Quelle heure est-il ?"
mosquitto_pub -h 192.168.1.1 -t ai/ask -m "Quelles sont les nouvelles du jour ?"

# Sous-titres seuls (l'ESP32 s'abonne à ces deux topics et les affiche).
mosquitto_pub -h 192.168.1.1 -t ai/transcript -m "Quelle heure est-il ?"
mosquitto_pub -h 192.168.1.1 -t ai/answer -m "Il est 14h32, il fait beau dehors."


# --- Commandes ESP32 (esp32/cmd) ---
mosquitto_pub -h 192.168.1.1 -t esp32/cmd -m "page:nas"
mosquitto_pub -h 192.168.1.1 -t esp32/cmd -m "page:sysinfo1"
mosquitto_pub -h 192.168.1.1 -t esp32/cmd -m "brightness:60"
mosquitto_pub -h 192.168.1.1 -t esp32/cmd -m "volume:40"
mosquitto_pub -h 192.168.1.1 -t esp32/cmd -m "mem"
mosquitto_pub -h 192.168.1.1 -t esp32/cmd -m "reboot"


*S'inscrire au MQTT global
mosquitto_sub -h 192.168.1.1 -p 1883 -t "#" -v
