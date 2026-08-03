# MQTT Topics — Dashboard ESP32

## Convention de nommage
```
source/métrique         → valeur simple
source/sous-système     → JSON
```

Ce document décrit les topics MQTT utilisés par le dashboard et les scripts de monitoring.

---

## Topics NAS (Synology DS1522+)
Publiés par : `synology/scripts/nas_monitor.py` — métriques légères toutes les `REFRESH_INTERVAL` s (**5 s** via `monitor.env`), gros JSON (`disks`/`downloads`/`connections`) un cycle sur `BULK_EVERY_N=6` (**~30 s**)

| Topic                  | Type   | Unité  | Exemple            | Description                        |
|------------------------|--------|--------|--------------------|------------------------------------|
| nas/cpu                | int    | %      | 10                 | Charge CPU (user+system+other)     |
| nas/ram                | int    | %      | 12                 | RAM utilisée                       |
| nas/temp               | int    | °C     | 55                 | Température système                |
| nas/net_rx             | int    | KB/s   | 40                 | Trafic réseau entrant              |
| nas/net_tx             | int    | KB/s   | 753                | Trafic réseau sortant              |
| nas/volume1_used_pct   | int    | %      | 46                 | Espace utilisé volume1             |
| nas/volume1_status     | string | -      | "normal"           | État du volume1                    |
| nas/volume1_read_mbs   | float  | MB/s   | 0.71               | Taux de lecture volume1            |
| nas/volume1_write_mbs  | float  | MB/s   | 0.01               | Taux d'écriture volume1            |
| nas/disks              | JSON   | -      | voir ci-dessous    | État des disques (trié par num_id) |
| nas/downloads          | JSON   | -      | voir ci-dessous    | Top 10 torrents (DL en cours en tête, puis tri upload)
| nas/connections        | JSON   | -      | voir ci-dessous    | Connexions utilisateurs            |

### nas/disks — format JSON
Trié par `num_id`.

```json
[
  {
    "name": "Drive 1",
    "num_id": 1,
    "model": "WD20EARS-00MVWB0",
    "smart_status": "normal",
    "status": "normal",
    "temp": 35
  }
]
```

Affiché dans le tableau **NAS - Disques** : colonnes Drive, Modèle, Smart, Status, Température.

### nas/downloads — format JSON
Triés par `speed_download` décroissant, puis `speed_upload`, puis ratio. Top 10.

Les téléchargements en cours (`speed_download > 0`) remontent automatiquement en tête du tableau.

⚠️ `speed_upload`, `speed_download` et `size_uploaded` sont **déjà formatés en chaînes** par le bridge (auto-échelle `B/KB/MB/GB`, débits suffixés `/s`) ; le tri se fait côté bridge sur les octets **bruts** avant formatage. `ratio` reste numérique.

```json
[
  {
    "title": "RecalBox.iso",
    "status": "seeding",
    "speed_upload": "533 KB/s",
    "speed_download": "0 B/s",
    "size_uploaded": "477 GB",
    "ratio": 1.0
  }
]
```

Affiché dans le tableau **NAS - Downloads** : colonnes Titre, Débit (chaîne DL ou UL, unité incluse), Ratio. Les téléchargements actifs apparaissent en vert en haut du tableau.

### nas/connections — format JSON

```json
[
  {
    "user": "User",
    "from": "192.168.1.1",
    "service": "DiskStation Manager",
    "protocol": "HTTP/HTTPS",
    "time": "2026/05/23 20:41:25",
    "connected": true
  }
]
```

Affiché dans le tableau **NAS - Connexions** : colonnes User, IP, Service.

---

## Topics Freebox (Freebox v8)
Publiés par : `synology/scripts/freebox_monitor.py` — métriques légères toutes les `REFRESH_INTERVAL` s (**5 s** via `monitor.env`), gros JSON (`devices`) un cycle sur `BULK_EVERY_N=6` (**~30 s**)

| Topic                    | Type   | Unité | Exemple             | Description                      |
|--------------------------|--------|-------|---------------------|----------------------------------|
| freebox/rate_down        | float  | Mb/s  | 0.5                 | Débit descendant actuel          |
| freebox/rate_up          | float  | Mb/s  | 6.5                 | Débit montant actuel             |
| freebox/bandwidth_down   | int    | Mb/s  | 5000                | Bande passante max descendante   |
| freebox/bandwidth_up     | int    | Mb/s  | 900                 | Bande passante max montante      |
| freebox/state            | string | -     | "up"                | État de la connexion             |
| freebox/ipv4             | string | -     | "8.8.8.8"           | IP publique courante             |
| freebox/devices_active   | int    | -     | 15                  | Appareils joignables             |
| freebox/devices_total    | int    | -     | 38                  | Appareils connus au total        |
| freebox/devices          | JSON   | -     | voir ci-dessous     | Liste des appareils connectés    |

### freebox/devices — format JSON

```json
[
  {
    "name": "PC-Laptop",
    "ip": "192.168.1.42",
    "vendor": "Dell",
    "type": "wifi",
    "band": "5g",
    "signal": -55,
    "phy_rx_rate": 866,
    "phy_tx_rate": 866,
    "rx_rate": "14 B/s",
    "tx_rate": "3 B/s",
    "service": "YouTube"
  }
]
```

`rx_rate`/`tx_rate` = débits **instantanés**, déjà formatés avec unité par le bridge (ex. « 5.0 MB/s ») ; le tri se fait côté bridge sur les octets/s **bruts** avant formatage. (`phy_rx_rate`/`phy_tx_rate` restent des entiers Mbps = débit de liaison Wi-Fi, pas le trafic.)

`service` = nom du service en cours pour cet appareil (« YouTube », « Steam »…), **ajouté par enrichissement** : `freebox_monitor.py` le récupère en mémoire auprès de `activity_monitor.py` (mapping DNS AdGuard → service via `services.json`) — **pas de nouveau topic MQTT**. Chaîne vide si l'appareil n'a émis aucune requête DNS identifiable, ou si AdGuard n'est pas configuré (`AGH_*` de `monitor.env`).

Affiché dans le tableau **Freebox - Devices** : colonnes Nom, Type (ETH / 5G / 2.4G), RX, TX (chaînes avec unité). IP accessible en scrollant horizontalement. Trié par **débit down instantané décroissant, puis up, puis type** (ETH → 5G → 2.4G), puis IP. Le champ `service` alimente en plus le tableau **Activité réseau** (colonnes Appareil, Service, DL, UP).

---

## Topics ESP32
Implémentés dans `esp32/src/config.h` et `esp32/src/mqtt_manager.cpp`.

| Topic          | Type   | Exemple       | Description                        |
|----------------|--------|---------------|------------------------------------|
| esp32/status   | string | "online"      | Statut de l'ESP32                  |
| esp32/cmd      | string | "page:home"   | Commande envoyée à l'ESP32         |

### Commandes implémentées (esp32/cmd)
Format `cmd` ou `cmd:arg`, traité par `mqtt_handle_esp_cmd()` (`mqtt_manager.cpp`).

| Commande             | Effet                                                        |
|----------------------|--------------------------------------------------------------|
| `page:home`          | Affiche l'écran Home                                         |
| `page:nas`           | Affiche l'écran NAS                                          |
| `page:freebox`       | Affiche l'écran Freebox                                      |
| `page:ai`            | Affiche l'écran Companion (IA)                               |
| `page:sysinfo`       | Affiche l'écran SysInfo ; si déjà affiché, passe à la page suivante
|.                     | (bouclage sur 6 pages) — sortie par toucher.                 |
| `page:disks`         | Affiche le tableau NAS - Disques                             |
| `page:downloads`     | Affiche le tableau NAS - Downloads                           |
| `page:connections`   | Affiche le tableau NAS - Connexions                          |
| `page:devices`       | Affiche le tableau Freebox - Devices                         |
| `brightness:1-100`   | Règle la luminosité du retroéclairage (+synchro slider)      |
| `volume:0-100`       | Règle le volume audio                 (+synchro slider)      |
| `mem`                | Journalise l'état mémoire (heap interne/DMA/PSRAM, plus gros bloc libre, pile loopTask)
|                      | bouton « État mémoire » du panneau web, relisible via `GET /serial`
| `saverec`            | Envoie la dernière capture vocale au bridge (`POST /record`),
|                      | qui l'archive en WAV horodaté dans `scripts/captures/` sur le NAS
|                      | — bouton « Capture IA → NAS » du panneau web.
|                      | Y compris les captures écartées pour absence de parole.      |
| `shot`               | Capture l'écran de la dalle dans un buffer PSRAM, récupérable
|                      | en BMP par `GET /screen.bmp` — bouton « Capture d'écran » du
|                      | panneau web, qui arme et récupère d'un seul clic.            |
| `reboot`             | Redémarre l'ESP32 (`ESP.restart()`)                          |
|----------------------|--------------------------------------------------------------|
Chaque commande reçue est journalisée dans le buffer circulaire de `log_manager.cpp` (visible via `GET /serial` sur l'interface web).

> ⚠️ Une valeur `page:X` inconnue ou `brightness`/`volume` hors plage est journalisée sans effet — aucun crash, la commande est simplement ignorée.

> Ces mêmes commandes sont aussi déclenchables via `POST /cmd` sur l'interface web embarquée de l'ESP32 (panneau "Commandes ESP32", `http_manager.cpp`) — même fonction `mqtt_handle_esp_cmd()` sous-jacente.

> Elles peuvent aussi être émises par **le bridge** (`bridge_monitor.py`) suite à une **commande vocale** (« pilote, affiche le NAS ») : le LLM traduit la phrase en action via tool-calling. Voir `CLAUDE.md` (section « Commandes vocales »).

## QoS et messages retenus

Par convention les messages d'état (`esp32/status`) sont publiés avec `retain=true` afin que le broker conserve le dernier statut disponible. Les commandes (`esp32/cmd`) sont publiées sans retenue et avec `QoS=0` en général.

```bash
# Publier l'état disponible (retenu)
mosquitto_pub -h <BROKER> -t esp32/status -m "online" -r -q 1

# Envoyer une commande (non retenue)
mosquitto_pub -h <BROKER> -t esp32/cmd -m "page:nas" -q 0
```

### Payload `esp32/status`

Chaîne simple : `online`, `offline` ou `booting`.

> En pratique (depuis la migration esp-mqtt) : le firmware publie `"online"` sur `MQTT_EVENT_CONNECTED`, et un **Last Will Testament** est configuré (`cfg.session.last_will`) — le broker publie donc `"offline"` automatiquement si l'ESP32 se déconnecte brutalement (crash, coupure secteur, perte WiFi). `booting` n'est publié à aucun moment du code actuel.

---

## Topics IA (Companion)
Consommés/publiés par : `esp32/src/ai_manager.cpp` (écran Companion) et `synology/scripts/bridge_monitor.py` côté NAS (HTTP + MQTT).
|----------------|--------|-------------------------|--------------------------|-------------------------------------------------------------------------------------------------------|
| Topic          | Type   | Exemple                 | Sens                     | Description                                                                                           |
|----------------|--------|-------------------------|--------------------------|-------------------------------------------------------------------------------------------------------|
| ai/status      | string | "listening"             | ESP32 → broker           | État courant : `idle`, `listening`, `thinking`, `speaking`, `error`.
|                |        |                         |                          | Publié par l'ESP32 à chaque changement d'état, pour observation par un tiers (Home Assistant, etc.).
|                |        |                         |                          | ⚠️ **L'ESP32 ne s'y abonne pas** et un tiers ne peut donc PAS forcer un état affiché.
|                |        |                         |                          | L'abonnement a existé puis été retiré : le firmware s'appliquait son propre écho, et
|                |        |                         |                          | dès que deux états distincts étaient en vol, chaque message reçu différait de l'état
|                |        |                         |                          | courant → changement → republication, soit une oscillation permanente
|                |        |                         |                          | (`thinking`/`speaking`/`idle` en boucle au rythme de l'aller-retour broker).
|                |        |                         |                          | Le bridge publie bien `"error"` ici, mais à titre informatif seulement : l'ESP32
|                |        |                         |                          | détecte les échecs par le code HTTP de `/ask`.
|----------------|--------|-------------------------|--------------------------|-------------------------------------------------------------------------------------------------------|
| ai/ask         | string | "Quelle heure est-il ?" | → ESP32                  | Déclenche une question en texte (sans passer par le micro). Anti-doublon :
|                |        |                         |                          | ignoré si un `ai/ask` arrive moins de 2s après le précédent,
|                |        |                         |                          | ou si l'IA est déjà en cours d'utilisation (`listening`/`thinking`/`speaking`).
|                |        |                         |                          | Si la phrase contient un **mot-clé de pilotage** (`command_keywords`, défaut « pilote »),
|                |        |                         |                          | elle est traitée comme une **commande vocale** (cf. bridge) au lieu d'une question —
|                |        |                         |                          | action émise sur `esp32/cmd`.
|----------------|--------|-------------------------|--------------------------|-------------------------------------------------------------------------------------------------------|
| ai/transcript  | string | "Quelle heure est-il ?" | → ESP32                  | Texte de la question (résultat STT), affiché en sous-titre sur l'écran Companion.
|----------------|--------|-------------------------|--------------------------|-------------------------------------------------------------------------------------------------------|
| ai/answer      | string | "Il est 14h32."         | → ESP32                  | Texte de la réponse (LLM), affiché en sous-titre sur l'écran Companion.
|----------------|--------|-------------------------|--------------------------|-------------------------------------------------------------------------------------------------------|

### Bridge IA — HTTP (hors MQTT)
En complément de MQTT, l'ESP32 appelle directement en HTTP `bridge_monitor.py` côté NAS (adresse définie par `AI_BRIDGE_URL` / `AI_BRIDGE_TEXT_URL` dans `config.h`) :

- **`AI_BRIDGE_URL`** (`POST`, `Content-Type: application/octet-stream`) : upload direct du buffer audio PCM16 mono 16 kHz capturé en PSRAM (pas de fichier, pas d'écriture flash). En-têtes `X-Sample-Rate` et `X-Samples` envoyés avec la requête.
- **`AI_BRIDGE_TEXT_URL`** (`POST`, JSON `{"text": "..."}`) : utilisé pour les questions envoyées via `ai/ask`.
- **Réponse du bridge** : en-têtes `X-Transcript` et `X-Answer` (texte URL-encodé, car les en-têtes HTTP ne supportent que l'ISO-8859-1), suivis du flux audio TTS en **PCM brut** (int16 mono 16 kHz, pas de header WAV) en corps de réponse. Le firmware lit ce flux directement en PSRAM et le joue immédiatement — **rien n'est écrit sur la flash**. L'écriture d'un `/tts.wav` après lecture a été retirée : elle suspendait le cache d'instructions et gelait LVGL ~2,9 s après chaque réponse. Le rejeu (`ai_replay_answer()`) redemande la synthèse au bridge via `POST /say`. En-tête optionnel **`X-Listen-After: 1`** sur les réponses de confirmation d'une commande vocale : le firmware ré-arme l'écoute à la fin de la lecture, sans nouveau « Jarvis ».

---

## Notes

- L'ESP32 publie `esp32/status` à la connexion MQTT.
- Le client MQTT est **esp-mqtt** (natif ESP-IDF, migré depuis PubSubClient le 2026-07-28) : réception/parsing sur sa propre tâche `mqtt_task`, dispatch marshallé vers `loopTask` par une file FreeRTOS. `cfg.buffer.size = 5120` (> 4096 → alloué en PSRAM) pour absorber les gros payloads JSON (`freebox/devices` dépasse 4 Ko), réassemblés dans un tampon PSRAM.
- Les valeurs flottantes (`rate_down`, `rate_up`, `volume1_read_mbs`, etc.) sont affichées sans `%f` (non supporté par newlib nano) via des macros de conversion entière dans `display_manager.cpp`.

## Sources
- `synology/scripts/nas_monitor.py`
- `synology/scripts/freebox_monitor.py`
- `esp32/src/config.h`
- `esp32/src/mqtt_manager.cpp`