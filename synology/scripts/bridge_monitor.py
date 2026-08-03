#!/usr/bin/env python3
"""
bridge_monitor.py — Pont STT/LLM/TTS pour l'écran Companion IA de l'ESP32
Reçoit du PCM brut (int16, mono, 16kHz) en HTTP POST sur /ask, renvoie
la réponse vocale dans le même format (PCM brut, pas de conteneur WAV).

Chaîne :
  1. STT    — Groq Whisper (whisper-large-v3-turbo, gratuit, multilingue)
  2. Météo  — Open-Meteo si l'intention est détectée (actuel/demain/semaine)
  2b. Actu  — flux RSS si l'intention est détectée (mots-clés simples)
  3. LLM    — n'importe quel endpoint compatible OpenAI (LLM_BASE_URL),
             pour rester modulable (Groq / OpenRouter / Mistral / Ollama...)
  4. TTS    — edge-tts (voix françaises Microsoft, gratuit, sans clé)

Topics MQTT publiés (texte uniquement — l'audio reste en HTTP) :
  ai/transcript   string   question transcrite (STT)
  ai/answer       string   réponse texte (LLM) — publié une fois le TTS prêt,
                           pour rester synchronisé avec l'état "speaking"
  ai/status       string   "error" en cas d'échec d'une étape

Endpoints HTTP (pas des topics MQTT), voir les routes Flask ci-dessous :
  POST /ask       PCM brut  -> chaîne complète STT/LLM/TTS
  POST /ask_text  {"text"}  -> saute le STT, LLM + TTS
  POST /say       {"text"}  -> TTS seul : invite système prononcée telle quelle,
                              sans LLM et sans mémorisation dans l'historique

Le firmware ESP32 (ai_manager.cpp) publie déjà lui-même "listening",
"thinking" et "speaking" — ce bridge ne publie que ce qu'il est seul
à connaître : le texte, et les erreurs survenant après l'envoi HTTP.
"""

# ----------------------------------------------------------------
# BIBLIOTHÈQUES
# ----------------------------------------------------------------
import os
import io
import re
import json
import time
import wave
import asyncio
from collections import deque
from datetime import datetime
import zoneinfo
from xml.etree import ElementTree
import requests
import paho.mqtt.client as mqtt
from paho.mqtt.enums import CallbackAPIVersion
from flask import Flask, request, Response
from openai import OpenAI
import edge_tts
from pydub import AudioSegment
import static_ffmpeg
static_ffmpeg.add_paths()   # télécharge ffmpeg+ffprobe (une fois) et les ajoute au PATH
from urllib.parse import quote
try:
    from ddgs import DDGS       # recherche web (outil web_search)
except ImportError:             # image pas reconstruite : l'outil se dira
    DDGS = None                 # indisponible, le reste du bridge tourne

# ----------------------------------------------------------------
# OBJETS GLOBAUX
# ----------------------------------------------------------------
MQTT_BROKER = os.getenv("MQTT_BROKER", "192.168.1.1")
MQTT_PORT   = int(os.getenv("MQTT_PORT", "1883"))

HTTP_HOST = os.getenv("AI_BRIDGE_HOST", "0.0.0.0")
HTTP_PORT = int(os.getenv("AI_BRIDGE_PORT", "8090"))

SAMPLE_RATE = int(os.getenv("AUDIO_SAMPLE_RATE", "16000"))  # doit matcher AUDIO_SAMPLE_RATE (config.h)

# Marge sous la saturation après normalisation de l'audio envoyé au STT, en dB.
# Compense la distance variable au micro. 0 (ou vide) désactive.
_hr = os.getenv("STT_NORMALIZE_HEADROOM", "3")
STT_NORMALIZE_HEADROOM = float(_hr) if _hr.strip() not in ("", "0") else None

# Archivage des captures vocales envoyées par l'ESP32 (POST /record) — sous
# ./scripts, monté depuis le NAS, donc directement accessibles depuis DSM.
RECORD_DIR = os.getenv("RECORD_DIR", "/app/scripts/captures")

# --- STT (Groq — Whisper) ---
GROQ_API_KEY = os.getenv("GROQ_API_KEY", "")
STT_MODEL    = os.getenv("STT_MODEL", "whisper-large-v3-turbo")
STT_URL      = "https://api.groq.com/openai/v1/audio/transcriptions"

# --- LLM (compatible OpenAI — endpoint + secret ; le MODÈLE est un paramètre IA,
#     déplacé dans bridge_defaults.json, cf. plus bas) ---
LLM_BASE_URL = os.getenv("LLM_BASE_URL", "https://api.groq.com/openai/v1")

# ⚠️ reasoning_effort / reasoning_format passent par extra_body et NON en
# kwargs : absents de la signature de create(), le SDK lèverait un TypeError.
# ⚠️ Seuls les modèles À RAISONNEMENT les acceptent — un llama/kimi répond 400,
# d'où le refus mémorisé par modèle (cf. le retry de llm_answer).
LLM_IS_GROQ = "groq.com" in LLM_BASE_URL
_no_reasoning_models: set[str] = set()

def _llm_extra_body() -> dict:
    if not LLM_IS_GROQ or _settings["llm_model"] in _no_reasoning_models:
        return {}
    return {"reasoning_effort": "none", "reasoning_format": "hidden"}

# --- Paramètres IA modifiables À CHAUD (page http://<NAS>:8090/) ---
#   bridge_defaults.json (repo, versionné) -> valeurs par DÉFAUT
#   bridge_settings.json (NAS, hors repo)  -> overrides SAUVÉS par l'interface web
# monitor.env ne garde QUE le structurel (broker/hôtes/ports/secrets/cadence).
# Gotchas des mots-clés, le JSON ne portant pas de commentaire :
#   météo : fragments COURTS ; "pleuv" et non "pleu" (qui prendrait "pleurer") ;
#           weather_city n'est qu'un libellé, lat/lon font la requête.
#   actus : seuls les <title> RSS 2.0 sont lus (pas Atom).
#   prompt : ⚠️ pas de date/heure ici (jamais passé à .format()) — ajoutée dans
#            llm_answer() via _current_datetime_fr().
DEFAULTS_FILE          = os.getenv("BRIDGE_DEFAULTS_FILE", "/app/scripts/bridge_defaults.json")
SETTINGS_FILE          = os.getenv("BRIDGE_SETTINGS_FILE", "/app/scripts/bridge_settings.json")
TTS_VOICE_FILE_LEGACY  = "/app/scripts/tts_voice.json"   # ancien fichier voix seule, migré puis ignoré

# ⚠️ Peuplés au démarrage (main), PAS à l'import : monitor.py importe les trois
# scripts dans le même process, une erreur de fichier ne doit tuer que le bridge.
DEFAULT_SETTINGS: dict = {}
_settings: dict = {}

# --- Date du jour (injectée dans le prompt système — aucun LLM ne connaît la
#     date courante par lui-même, quel que soit le modèle) ---
_JOURS = ["lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi", "dimanche"]
_MOIS  = ["janvier", "février", "mars", "avril", "mai", "juin", "juillet",
          "août", "septembre", "octobre", "novembre", "décembre"]
# --- Timezone
_TZ = os.getenv("TIMEZONE","Europe/Paris")

# --- Historique conversationnel ---
# Deux réglages pilotables par env (monitor.env). ⚠️ La PROFONDEUR se paie à
# CHAQUE requête (tout le fil renvoyé au LLM) ; le TTL, lui, est gratuit.
CONVERSATION_TTL_S         = int(os.getenv("AI_CONVERSATION_TTL_S", "600"))          # inactivité, en s
CONVERSATION_MAX_EXCHANGES = int(os.getenv("AI_CONVERSATION_MAX_EXCHANGES", "10"))   # nb d'échanges gardés
conversation = deque(maxlen=CONVERSATION_MAX_EXCHANGES * 2)   # ×2 : 1 échange = user + assistant
_last_exchange_ts = 0.0

llm_client = OpenAI(base_url=LLM_BASE_URL, api_key=GROQ_API_KEY)
mqtt_client = mqtt.Client(callback_api_version=CallbackAPIVersion.VERSION2, client_id="ai-bridge")

app = Flask(__name__)

# ----------------------------------------------------------------
# API LOCALES
# ----------------------------------------------------------------

# --- Audio — normalisation du niveau avant STT ---
def normalize_pcm(pcm_bytes: bytes) -> bytes:
    """Ramène le pic à STT_NORMALIZE_HEADROOM dB sous la saturation.

    C'est la compensation que faisait l'ALC matérielle de l'ES8311, désactivée
    depuis : elle agissait en temps réel et écrasait le gain 300 ms après une
    phrase forte. Ici on travaille sur l'enregistrement COMPLET — gain
    uniforme, SNR inchangé, plus de syllabe perdue.
    """
    if not pcm_bytes or STT_NORMALIZE_HEADROOM is None:
        return pcm_bytes

    seg = AudioSegment(data=pcm_bytes, sample_width=2, frame_rate=SAMPLE_RATE, channels=1)
    if seg.max == 0:   # silence total : normaliser amplifierait le néant
        return pcm_bytes

    seg = seg.normalize(headroom=STT_NORMALIZE_HEADROOM)
    print(f"[Bridge] Normalisation : pic {seg.max_dBFS:+.1f} dBFS "
          f"(cible -{STT_NORMALIZE_HEADROOM:.1f})")
    return seg.raw_data

# --- Audio — conversion PCM brut <-> WAV (pour l'API Groq STT) ---
def pcm_to_wav(pcm_bytes: bytes) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)  # int16
        w.setframerate(SAMPLE_RATE)
        w.writeframes(pcm_bytes)
    return buf.getvalue()

def audio_to_pcm(audio_bytes: bytes, source_format: str) -> bytes:
    """Convertit un format audio quelconque (le MP3 d'edge-tts) en PCM brut
    int16 mono 16 kHz — le seul format que l'ESP32 sait lire, sans header
    (cf. audio_play_psram_stream_queue dans audio_manager.cpp)."""
    seg = AudioSegment.from_file(io.BytesIO(audio_bytes), format=source_format)
    seg = seg.set_frame_rate(SAMPLE_RATE).set_channels(1).set_sample_width(2)
    return seg.raw_data

# --- Paramètres — chargement/sauvegarde ---
def _settings_load() -> bool:
    """Charge les défauts (repo) puis applique les overrides sauvés (NAS).
    Renvoie False si les défauts sont illisibles (démarrage bridge annulé)."""
    global DEFAULT_SETTINGS, _settings
    try:
        with open(DEFAULTS_FILE, "r", encoding="utf-8") as f:
            DEFAULT_SETTINGS = json.load(f)
    except Exception as e:
        print(f"[Bridge] FATAL : défauts IA illisibles ({DEFAULTS_FILE}) : {e}")
        return False
    _settings = dict(DEFAULT_SETTINGS)

    try:
        with open(SETTINGS_FILE, "r", encoding="utf-8") as f:
            saved = json.load(f)
        # Seules les clés connues sont reprises : une clé disparue des défauts ne
        # ressuscite pas, une clé inconnue du fichier est ignorée.
        for k in _settings:
            if k in saved:
                _settings[k] = saved[k]
        print(f"[Bridge] Paramètres restaurés depuis {SETTINGS_FILE}")
    except FileNotFoundError:
        # Migration depuis l'ancien fichier voix-seule, si présent.
        try:
            with open(TTS_VOICE_FILE_LEGACY, "r", encoding="utf-8") as f:
                v = json.load(f).get("voice", "").strip()
            if v:
                _settings["voice"] = v
                print(f"[Bridge] Voix migrée depuis tts_voice.json : {v}")
        except Exception:
            pass   # vrai premier démarrage : défauts seuls
    except Exception as e:
        print(f"[Bridge] Lecture paramètres impossible ({e}) — défauts")
    return True

def _settings_save():
    try:
        with open(SETTINGS_FILE, "w", encoding="utf-8") as f:
            json.dump(_settings, f, ensure_ascii=False, indent=1)
    except Exception as e:
        print(f"[Bridge] Paramètres non persistés ({e}) — actifs jusqu'au redémarrage")

# Liste des voix francophones, récupérée une seule fois (appel réseau ~1 s).
_voices_cache = None
def _voices_fr():
    global _voices_cache
    if _voices_cache is None:
        try:
            allv = asyncio.run(edge_tts.list_voices())
            _voices_cache = sorted(
                [{"name": v["ShortName"], "gender": v["Gender"], "locale": v["Locale"]}
                 for v in allv if v["Locale"].startswith("fr-")],
                key=lambda v: (v["locale"], v["name"]))
        except Exception as e:
            print(f"[Bridge] Liste des voix indisponible : {e}")
            return [{"name": _settings["voice"], "gender": "?", "locale": "?"}]
    return _voices_cache

# --- MQTT — publication simple (texte + erreurs) ---
def mqtt_pub(topic: str, payload: str):
    try:
        mqtt_client.publish(topic, payload)
    except Exception as e:
        print(f"[Bridge] Erreur publication MQTT {topic} : {e}")

def _current_datetime_fr() -> str:
    now = datetime.now(zoneinfo.ZoneInfo(_TZ))
    return (f"{_JOURS[now.weekday()]} {now.day} {_MOIS[now.month - 1]} {now.year}, "
            f"il est {now:%H:%M}")

# --- Historique conversationnel — purge après inactivité (cf. CONVERSATION_TTL_S) ---
def _conversation_clear() -> int:
    """Vide l'historique et remet l'horodatage à zéro — sans ce reset, le TTL
    se redéclenchait à chaque appel suivant. Retourne le nombre d'échanges."""
    global _last_exchange_ts
    n = len(conversation) // 2
    conversation.clear()
    _last_exchange_ts = 0.0
    return n

def _conversation_history() -> list:
    if _last_exchange_ts and time.monotonic() - _last_exchange_ts > CONVERSATION_TTL_S:
        _conversation_clear()

    hist = list(conversation)
    # maxlen coupe par le début : le fil peut commencer par un "assistant"
    # orphelin, que certains backends refusent.
    if hist and hist[0]["role"] == "assistant":
        hist.pop(0)
    return hist

def _conversation_remember(question: str, answer: str):
    global _last_exchange_ts
    conversation.append({"role": "user", "content": question})
    conversation.append({"role": "assistant", "content": answer})
    _last_exchange_ts = time.monotonic()

# --- 1. STT — Groq Whisper ---
def stt_transcribe(pcm_bytes: bytes) -> str | None:
    # Normalisation AVANT l'emballage WAV : l'ESP32 capture en linéaire, le
    # niveau dépend donc directement de la distance au micro.
    wav_bytes = pcm_to_wav(normalize_pcm(pcm_bytes))
    try:
        r = requests.post(
            STT_URL,
            headers={"Authorization": f"Bearer {GROQ_API_KEY}"},
            files={"file": ("audio.wav", wav_bytes, "audio/wav")},
            data={"model": STT_MODEL, "language": "fr"},
            timeout=15
        )
        r.raise_for_status()
        return r.json().get("text", "").strip()
    except Exception as e:
        print(f"[Bridge] Erreur STT : {e}")
        return None

# Codes météo WMO (Open-Meteo) -> libellé court FR. Table partielle ; un code
# absent retombe sur "" (pas de condition mentionnée).
_WMO = {0: "ciel dégagé", 1: "peu nuageux", 2: "partiellement nuageux", 3: "couvert",
        45: "brouillard", 48: "brouillard givrant",
        51: "bruine légère", 53: "bruine", 55: "bruine dense",
        61: "pluie faible", 63: "pluie", 65: "pluie forte",
        71: "neige faible", 73: "neige", 75: "neige forte",
        80: "averses", 81: "averses", 82: "fortes averses",
        95: "orage", 96: "orage grêleux", 99: "orage grêleux"}

def _wmo_label(code) -> str:
    try:
        return _WMO.get(int(code), "")
    except (TypeError, ValueError):
        return ""

# --- 2. Météo — Open-Meteo (outil get_weather) ---
# horizon : "now" | 1 (demain) | 2 (après-demain) | "week".
def fetch_weather(horizon="now") -> str | None:
    params = {"latitude": _settings["weather_lat"], "longitude": _settings["weather_lon"],
              "timezone": "auto"}
    if horizon == "now":
        params["current"] = "temperature_2m,precipitation,weather_code"
    else:
        params["daily"] = "weather_code,temperature_2m_max,temperature_2m_min,precipitation_sum"
        params["forecast_days"] = 7 if horizon == "week" else horizon + 1

    try:
        r = requests.get("https://api.open-meteo.com/v1/forecast", params=params, timeout=10)
        r.raise_for_status()
        data = r.json()
    except Exception as e:
        print(f"[Bridge] Erreur météo : {e}")
        return None

    city = _settings["weather_city"]
    if horizon == "now":
        cur = data.get("current", {})
        cond = _wmo_label(cur.get("weather_code"))
        return (f"Météo actuelle à {city} : {cur.get('temperature_2m')}°C, "
                f"précipitations {cur.get('precipitation')}mm"
                f"{', ' + cond if cond else ''}.")

    daily  = data.get("daily", {})
    dates  = daily.get("time", [])
    tmax   = daily.get("temperature_2m_max", [])
    tmin   = daily.get("temperature_2m_min", [])
    precip = daily.get("precipitation_sum", [])
    codes  = daily.get("weather_code", [])
    if not dates:
        return None

    def _ligne(i: int) -> str:
        d = datetime.fromisoformat(dates[i])
        jour = f"{_JOURS[d.weekday()]} {d.day} {_MOIS[d.month - 1]}"
        cond = _wmo_label(codes[i]) if i < len(codes) else ""
        return (f"{jour} : {cond + ', ' if cond else ''}"
                f"{tmin[i]} à {tmax[i]}°C, précipitations {precip[i]}mm")

    if horizon == "week":
        lignes = " ; ".join(_ligne(i) for i in range(len(dates)))
        return f"Prévisions météo à {city} pour les prochains jours : {lignes}."
    return f"Prévisions météo à {city} — {_ligne(horizon)}."

# --- 2b. Actualités — flux RSS (outil get_news) ---
def fetch_news(count: int | None = None) -> str | None:
    count = count or _settings["news_count"]
    # Titres collectés PAR flux puis ENTRELACÉS : sans ça, un flux volumineux
    # remplit à lui seul news_count et un flux local n'apparaît JAMAIS.
    per_feed: list[tuple[str, list[str]]] = []          # (source, titres)
    for url in _settings["news_feeds"]:
        try:
            r = requests.get(url, timeout=10, headers={"User-Agent": "dashboard-bridge"})
            r.raise_for_status()
            root = ElementTree.fromstring(r.content)
            # Le <title> du <channel> nomme la source : sans lui,
            #  un titre local n'est PAS rattachable à sa région par le LLM.
            src = (root.findtext("channel/title") or url).strip()
            per_feed.append((src, [t for it in root.iter("item")     # RSS 2.0
                                   if (t := (it.findtext("title") or "").strip())]))
        except Exception as e:
            print(f"[Bridge] Erreur actualités ({url}) : {e}")
    # Round-robin plafonné à news_count, la SOURCE conservée pour regrouper ensuite.
    picked: list[tuple[str, str]] = []                  # (source, titre)
    seen: set[str] = set()
    for rank in range(max((len(t) for _, t in per_feed), default=0)):
        for src, feed in per_feed:
            if rank < len(feed) and feed[rank] not in seen:
                seen.add(feed[rank])
                picked.append((src, feed[rank]))
        if len(picked) >= count:
            break
    picked = picked[:count]
    if not picked:
        return None
    # Regroupé par source : le LLM répond « en Charente » via la section dédiée.
    par_src: dict[str, list[str]] = {}
    for src, t in picked:
        par_src.setdefault(src, []).append(t)
    blocs = "\n".join(f"[{src}]\n" + "\n".join(f"- {t}" for t in ts)
                      for src, ts in par_src.items())
    return ("Titres d'actualité du jour, groupés par source (résume à l'oral les "
            "principaux, sans tous les énumérer ; si la question cible un lieu ou "
            "une région, privilégie la source correspondante) :\n" + blocs)

# --- 2c. Registre d'outils — schémas function-calling exposés au LLM ---
# Un outil = un schéma OpenAI littéral + le callable qui l'exécute. Tous en
# LECTURE SEULE : le pilotage MQTT a son propre chemin (maybe_handle_command).

# Horizons nommés : un enum se raisonne mieux qu'un entier de jours.
_HORIZONS = {"now": "now", "tomorrow": 1, "day_after": 2, "week": "week"}

def _tool_get_weather(horizon: str = "now") -> str:
    return fetch_weather(_HORIZONS.get(horizon, "now")) or "Météo indisponible."

def _tool_get_news(count=None) -> str:
    n = _clamp_int(count, 1, 15) if count is not None else None
    return fetch_news(n) or "Aucune actualité disponible."

# 5 × 300 caractères ≈ 400 tokens : au-delà, les snippets noient la question.
WEB_SEARCH_RESULTS  = 5
WEB_SEARCH_SNIPPET  = 300

def _tool_web_search(query: str) -> str:
    if DDGS is None:
        print("[Bridge] web_search : ddgs absent de l'image")
        return "Échec de la recherche. Réessaie ou réponds sans."
    q = str(query).strip()
    if not q:
        return "Requête vide : rappelle l'outil avec des mots-clés."
    res = DDGS().text(q, region="fr-fr", max_results=WEB_SEARCH_RESULTS)
    if not res:
        # Formulé pour que le modèle ne conclue pas à une infirmation.
        return (f"Aucun résultat exploitable pour « {q} ». L'absence de résultat ne prouve "
                f"RIEN : réessaie avec d'autres mots-clés, sinon dis que tu n'as pas pu vérifier.")
    # URL jetées : illisibles en TTS.
    lignes = []
    for r in res:
        titre = (r.get("title") or "").strip()
        corps = " ".join((r.get("body") or "").split())[:WEB_SEARCH_SNIPPET]
        if titre and corps:
            lignes.append(f"- {titre} : {corps}")
        elif titre or corps:
            lignes.append(f"- {titre or corps}")
    return (f"Résultats de recherche web pour « {q} » "
            f"(extraits bruts, à recouper avant d'affirmer) :\n" + "\n".join(lignes))

_TOOLS = {
    "get_weather": ({"type": "function", "function": {
        "name": "get_weather",
        "description": "Météo actuelle ou prévisions, pour la ville configurée.",
        "parameters": {"type": "object", "properties": {
            "horizon": {"type": "string",
                        "enum": ["now", "tomorrow", "day_after", "week"],
                        "description": "now = maintenant, week = 7 jours"}},
            "required": []}}}, _tool_get_weather),

    "get_news": ({"type": "function", "function": {
        "name": "get_news",
        "description": "Titres d'actualité du jour, depuis les flux RSS configurés.",
        "parameters": {"type": "object", "properties": {
            "count": {"type": "integer", "minimum": 1, "maximum": 15,
                      "description": "Nombre de titres ; défaut = réglage de la page"}},
            "required": []}}}, _tool_get_news),

    "web_search": ({"type": "function", "function": {
        "name": "web_search",
        "description": (
            "Recherche sur le web. À utiliser quand la question porte sur un événement "
            "récent, une actualité, un prix, un résultat sportif, une personne peu connue, "
            "l'actualité ou le statut d'une personnalité (dont un décès), "
            "ou toute information postérieure à ton entraînement — et quand l'utilisateur "
            "affirme un fait que tu ignores. NE PAS utiliser pour des connaissances "
            "générales stables (histoire, science, définitions, calcul)."),
        "parameters": {"type": "object", "properties": {
            "query": {"type": "string", "description": "Requête, en mots-clés"}},
            "required": ["query"]}}}, _tool_web_search),
}

def _tool_schemas() -> list:
    # web_search est débrayable à chaud (page config) : ses snippets peuvent
    # noyer une question à laquelle le modèle répond bien seul.
    off = () if _settings.get("web_search_enabled", True) else ("web_search",)
    return [s for n, (s, _) in _TOOLS.items() if n not in off]

def _tool_run(name: str, args: dict) -> str:
    """Tout échec ressort en CHAÎNE, jamais en exception. Les messages évitent
    « indisponible » : le modèle en déduirait qu'il n'a aucun outil et le dirait."""
    entry = _TOOLS.get(name)
    if not entry:
        return f"Outil inconnu : {name}. Utilise un des outils proposés."
    try:
        return entry[1](**args)
    except Exception as e:
        print(f"[Bridge] Outil {name} en échec : {e}")
        return f"Échec de {name} pour cet appel. Réessaie ou réponds sans."

# --- 3. LLM — endpoint compatible OpenAI (modulable via LLM_BASE_URL/MODEL) ---
# Un appel au modèle, avec le retry "reasoning" (2 tentatives max). Renvoie le
# MESSAGE brut — qui peut porter des tool_calls au lieu d'un contenu —, ou None.
def _llm_create(messages: list, max_tokens: int, tools: list | None = None):
    # 2 tentatives : un refus des paramètres reasoning (400) est mémorisé par
    # modèle et la requête rejouée sans extra_body.
    for attempt in (1, 2):
        try:
            kwargs = dict(model=_settings["llm_model"], messages=messages,
                          temperature=_settings["temperature"],
                          max_tokens=max_tokens, extra_body=_llm_extra_body())
            if tools:
                kwargs["tools"], kwargs["tool_choice"] = tools, "auto"
            return llm_client.chat.completions.create(**kwargs).choices[0].message
        except Exception as e:
            if attempt == 1 and "reasoning" in str(e).lower():
                _no_reasoning_models.add(_settings["llm_model"])
                print(f"[Bridge] {_settings['llm_model']} refuse les paramètres reasoning — retry sans")
                continue
            print(f"[Bridge] Erreur LLM ({LLM_BASE_URL}, {_settings['llm_model']}) : {e}")
            # NB : un 429 (quota Groq atteint) n'est pas distingué du reste — le
            # firmware ne reçoit qu'un 500 générique et affiche "erreur".
            return None

# Consigne outils, ajoutée au prompt système de la page config.
_TOOLS_SYSTEM = ("Tu disposes d'outils pour obtenir des informations que tu ne connais pas. "
                 "Appelle-les quand la réponse dépend de données récentes ou locales ; "
                 "sinon réponds directement, sans outil. Si l'utilisateur affirme un fait "
                 "récent que tu ignores, ne le contredis pas : vérifie avant de répondre. "
                 "N'affirme JAMAIS qu'un événement récent n'a pas eu lieu sans avoir vérifié.")

# Garde-fou : un modèle qui rappelle indéfiniment ses outils bloquerait la requête.
TOOL_ROUNDS_MAX = 3

def llm_answer(question: str, context: str | None, max_tokens: int | None = None) -> str | None:
    # Contexte, modèle, température et longueur viennent de _settings :
    # modifiables à chaud depuis la page http://<NAS>:8090/.
    system = f"{_settings['system_prompt']}\n\nNous sommes le {_current_datetime_fr()}."
    if context:
        system += f"\n\n{context}"
    tools = _tool_schemas()
    if tools:
        system += f"\n\n{_TOOLS_SYSTEM}"

    messages = [{"role": "system", "content": system}]
    messages.extend(_conversation_history())
    messages.append({"role": "user", "content": question})

    # Budget serré tant qu'aucun outil n'a répondu : un appel d'outil y tient.
    budget = max_tokens or _settings["max_tokens"]
    vus: set = set()
    for tour in range(TOOL_ROUNDS_MAX):
        # Dernier tour SANS outils : le modèle doit rédiger avec ce qu'il a.
        dernier = tour == TOOL_ROUNDS_MAX - 1
        msg = _llm_create(messages, budget, None if dernier else tools)
        if msg is None:
            return None
        calls = getattr(msg, "tool_calls", None)
        if not calls:
            break
        # Le message porteur des tool_calls revient dans le fil, sinon le modèle
        # ne rattache pas les résultats à ses appels. Reconstruit à la main : les
        # champs propres au fournisseur ne repartent pas.
        messages.append({"role": "assistant", "content": msg.content or "",
                         "tool_calls": [{"id": c.id, "type": "function",
                                         "function": {"name": c.function.name,
                                                      "arguments": c.function.arguments}}
                                        for c in calls]})
        for c in calls:
            try:
                args = json.loads(c.function.arguments or "{}")
            except Exception:
                args = {}
            # Appel déjà joué à l'identique : le rejouer donnerait le même résultat.
            cle = (c.function.name, c.function.arguments)
            if cle in vus:
                out = ("Tu as déjà appelé cet outil avec ces arguments, le résultat est "
                       "identique. Réponds avec ce que tu as, ou dis ce qui manque.")
            else:
                vus.add(cle)
                out = _tool_run(c.function.name, args)
            print(f"[Bridge] Outil {c.function.name}({args}) → {out[:80]}")
            messages.append({"role": "tool", "tool_call_id": c.id, "content": out})
        # La rédaction qui suit peut avoir à résumer plusieurs titres.
        budget = max(budget, _settings.get("tools_max_tokens", 200))
    else:
        print(f"[Bridge] {TOOL_ROUNDS_MAX} tours d'outils sans réponse — abandon")
        return None

    answer = (msg.content or "").strip()
    if not answer:
        return None
    _conversation_remember(question, answer)
    return answer

# --- 4. TTS — edge-tts (voix française, gratuit) ---
async def _edge_tts_generate(text: str, voice: str) -> bytes:
    communicate = edge_tts.Communicate(text, voice)
    chunks = bytearray()
    async for chunk in communicate.stream():
        if chunk["type"] == "audio":
            chunks.extend(chunk["data"])
    return bytes(chunks)

# voice=None → la voix courante (modifiable à chaud). Le paramètre n'est renseigné
# que par /preview, pour écouter une voix sans encore l'adopter.
def tts_speak(text: str, voice: str | None = None) -> bytes | None:
    try:
        # edge-tts rend du MP3, jamais du PCM : on le décode ici, l'ESP32 ne
        # sait lire que du PCM brut.
        mp3_bytes = asyncio.run(_edge_tts_generate(text, voice or _settings["voice"]))
        return audio_to_pcm(mp3_bytes, "mp3")
    except Exception as e:
        print(f"[Bridge] Erreur TTS : {e}")
        return None

# ----------------------------------------------------------------
# ROUTES HTTP
# ----------------------------------------------------------------

# --- Endpoint HTTP — appelé par ai_manager.cpp (_ask_bridge) ---
@app.route("/ask", methods=["POST"])
def ask():
    pcm_in = request.get_data()
    if not pcm_in:
        return "", 400

    text = stt_transcribe(pcm_in)
    if not text:
        mqtt_pub("ai/status", "error")
        return "", 500
    print(f"[Bridge] Question (voix) : {text}")
    mqtt_pub("ai/transcript", text)

    return _answer_and_speak(text)


@app.route("/ask_text", methods=["POST"])
def ask_text():
    """Question posée en texte (ex: via MQTT ai/ask côté ESP32, tant que
    l'écran companion est affiché) — saute le STT, mais publie quand même
    ai/transcript pour garder l'affichage des sous-titres cohérent."""
    data = request.get_json(silent=True) or {}
    text = (data.get("text") or "").strip()
    if not text:
        return "", 400

    print(f"[Bridge] Question (texte) : {text}")
    mqtt_pub("ai/transcript", text)

    return _answer_and_speak(text)


@app.route("/say", methods=["POST"])
def say():
    """TTS seul — ni STT, ni LLM, ni mémorisation dans l'historique.
    Permet à l'ESP32 de faire prononcer une invite système (« je n'ai pas
    entendu », erreurs, notifications) sans embarquer de fichier audio en flash
    et sans polluer le contexte conversationnel : ce n'est pas un échange.
    Renvoie du PCM brut, comme /ask — même chemin de lecture côté firmware."""
    data = request.get_json(silent=True) or {}
    text = (data.get("text") or "").strip()
    if not text:
        return "", 400

    print(f"[Bridge] Invite vocale : {text}")

    pcm_out = tts_speak(text)
    if not pcm_out:
        mqtt_pub("ai/status", "error")
        return "", 500

    resp = Response(pcm_out, mimetype="application/octet-stream")
    resp.headers["X-Answer"] = quote(text)
    return resp


@app.route("/record", methods=["POST"])
def record():
    """Archive une capture vocale de l'ESP32 en WAV horodaté.

    Décharge l'ESP32 de l'écriture WAV : sur LittleFS elle prenait ~3 s
    (écriture flash qui suspend le cache d'instructions et gèle LVGL) et
    écrasait la capture précédente. Ici : ~100 ms de HTTP, un historique
    complet, et les fichiers sont déjà sur le NAS pour analyse."""
    pcm = request.get_data()
    if not pcm:
        return "", 400

    os.makedirs(RECORD_DIR, exist_ok=True)
    name = datetime.now().strftime("%Y%m%d-%H%M%S") + ".wav"
    with open(os.path.join(RECORD_DIR, name), "wb") as f:
        f.write(pcm_to_wav(pcm))

    seconds = round(len(pcm) / 2 / SAMPLE_RATE, 2)
    print(f"[Bridge] Capture archivée : {name} ({seconds} s)")
    return {"file": name, "seconds": seconds}


# --- Page de paramétrage IA — servie sur le port du bridge ---
# Tout est same-origin (page, listes, aperçu, validation). Le bouton
# « Config NAS » du panneau web de l'ESP32 pointe ici (AI_BRIDGE_UI_URL).

CONFIG_PAGE = """<!DOCTYPE html><html lang="fr"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Paramètres de Jarvis</title><style>
/* ---- BASE / LAYOUT ---- */
body{font-family:'Segoe UI',Tahoma,sans-serif;background:#121212;color:#e0e0e0;margin:0;padding:20px;
     display:flex;flex-direction:column;align-items:center;gap:20px}
.layout{display:flex;gap:20px;width:100%;max-width:1240px;flex-wrap:wrap;align-items:flex-start}
.col{flex:1;min-width:320px;display:flex;flex-direction:column;gap:20px}
.box{background:#1e1e1e;padding:24px;border-radius:12px;box-shadow:0 4px 20px rgba(0,0,0,.5);
     width:100%;box-sizing:border-box}
h1{color:#03dac6;font-size:22px;margin:0}
h2{color:#03dac6;font-size:16px;margin:0 0 4px}
p.sub{color:#888;font-size:13px;margin:0 0 14px}
/* ---- FORMULAIRES (COMMUN) ---- */
.hint{color:#888;font-size:12px;margin:0 0 6px}
label{display:block;color:#03dac6;font-size:12px;text-transform:uppercase;letter-spacing:.5px;margin:14px 0 6px}
select,input,textarea{width:100%;padding:10px;border-radius:6px;border:1px solid #444;background:#2a2a2a;
             color:#e0e0e0;font-size:14px;box-sizing:border-box;font-family:inherit}
select:focus,input:focus,textarea:focus{outline:none;border-color:#03dac6}
textarea{resize:vertical;min-height:120px;line-height:1.45}
.row{display:flex;gap:10px;margin-top:16px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:0 16px}
button{flex:1;padding:11px;border:none;border-radius:6px;font-size:14px;cursor:pointer;transition:.2s}
.sec{background:#333;color:#e0e0e0}.sec:hover{background:#3700B3;color:#fff}
.pri{background:#03dac6;color:#121212;font-weight:600}.pri:hover{background:#00b3a1}
.warn{background:#333;color:#ffab40}.warn:hover{background:#ff4081;color:#fff}
.cur{margin-top:14px;padding:12px;background:#2a2a2a;border-radius:6px;font-size:13px}
.cur b{color:#03dac6}
.msg{margin-top:10px;font-size:13px;min-height:17px}
.ok{color:#00c853}.ko{color:#ff4081}
.foot{color:#666;font-size:12px}
/* ---- PANNEAU CONVERSATION ---- */
.histbox{margin-top:12px;max-height:340px;overflow-y:auto;display:flex;flex-direction:column;gap:8px}
.histbox .u,.histbox .a{padding:8px 10px;border-radius:8px;font-size:14px;white-space:pre-wrap;word-break:break-word;max-width:92%}
.histbox .u{background:#2a2f45;align-self:flex-end}
.histbox .a{background:#1e2233;align-self:flex-start}
.histbox .empty{color:#888;font-style:italic}
/* ---- PANNEAU VOIX ---- */
audio{width:100%;margin-top:12px}
/* ---- PANNEAU COMMANDES ---- */
.tool{background:#232323;border:1px solid #333;border-radius:10px;margin-top:12px}
.tool.op{border-color:#03dac6}
.tool .hd{display:flex;align-items:center;gap:8px;padding:12px 14px;cursor:pointer}
.tool .hd .tw{color:#e0e0e0;font-size:14px;font-weight:500}
.tool .hd .st{margin-left:auto;color:#666;font-size:12px}
.tool .bd{padding:0 14px 14px}
.trow{display:grid;grid-template-columns:1.1fr .9fr 1.5fr auto;gap:6px;align-items:center;margin-top:6px}
.chip{cursor:pointer;background:#0c3a35;color:#03dac6;border:1px solid #03dac6;border-radius:12px;padding:2px 9px;font-size:12px;display:inline-block;margin:3px 3px 0 0}
.mini{flex:none;background:transparent;border:1px dashed #555;color:#aaa;border-radius:6px;padding:6px 10px;font-size:12px;cursor:pointer;margin-top:8px}
.xbtn{flex:none;background:#2a2a2a;border:1px solid #444;color:#ff6e6e;border-radius:6px;padding:8px 11px;cursor:pointer}
.tdel{flex:none;background:transparent;border:none;color:#ff6e6e;cursor:pointer;font-size:13px;padding:0;margin-left:auto}
/* ---- EN-TÊTE + ONGLETS ---- */
.header{width:100%;max-width:1240px;display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:12px;border-bottom:1px solid #333;padding-bottom:14px}
.tabs{display:flex;gap:6px;flex-wrap:wrap}
.tab{background:transparent;border:1px solid #444;color:#adbac7;border-radius:7px;padding:7px 18px;font-size:14px;cursor:pointer;white-space:nowrap}
.tab.active{background:#03dac6;border-color:#03dac6;color:#121212;font-weight:600}
.view{width:100%;display:flex;flex-direction:column;align-items:center}
.hidden{display:none}
</style></head><body>

<div class="header">
<h1>Paramètres de Jarvis</h1>
<nav class="tabs">
  <button class="tab active" data-view="conv">Conversations</button>
  <button class="tab" data-view="llm">Paramètres LLM</button>
  <button class="tab" data-view="tools">Outils</button>
</nav>
</div>

<div id="v-conv" class="view">
<div class="layout">
<div class="col">

<div class="box">
<h2>Conversation</h2>
<p class="sub">L'historique donné au LLM comme contexte (purge auto après inactivité).</p>
<div class="cur" style="display:flex;align-items:center;gap:8px;flex-wrap:wrap;font-size:13px">
  <span><b id="histN">…</b> échange(s) en mémoire<span id="resetIn" style="color:#888"></span></span>
  <span style="margin-left:auto;display:flex;align-items:center;gap:8px;color:#aaa">
    Réglages : Profondeur
    <input id="limEx" type="number" min="1" max="100" style="width:58px"
           title="Nombre d'échanges question-réponse gardés dans le contexte du LLM.">
    échanges · purge après
    <input id="limTtl" type="number" min="30" max="86400" style="width:78px"
           title="Durée d'inactivité (en secondes) avant vidage automatique de l'historique.">
    s
    <button class="sec" onclick="applyLimits()" style="flex:none;padding:6px 12px">Appliquer</button>
  </span>
</div>
<p class="hint">Réglage à chaud, <b>non sauvegardé</b> : au redémarrage du bridge, retour aux défauts de <code>monitor.env</code>. Plus la profondeur est grande, plus de contexte est renvoyé au LLM à chaque requête (tokens, latence).</p>
<div class="row">
  <button class="warn" onclick="clearConv()">Nouvelle conversation</button>
</div>
<label for="chatInput">Requête directe au LLM (hors dashboard — s'ajoute à l'historique)</label>
<div class="row">
  <input id="chatInput" placeholder="Pose ta question…" style="flex:1"
         onkeydown="if(event.key==='Enter')sendChat()">
  <button class="pri" onclick="sendChat()" style="flex:none">Envoyer</button>
</div>
<div id="histBox" class="histbox"></div>
<div class="msg" id="msgC"></div>
</div>

</div>
</div>
</div>

<div id="v-llm" class="view hidden">
<div class="layout">
<div class="col">

<div class="box">
<h2>Modèle LLM</h2>
<p class="sub">La liste vient de l'endpoint configuré — ce sont les modèles réellement disponibles.</p>
<label for="m">Modèle</label>
<select id="m" onchange="modelChanged()"></select>
<input id="mCustom" style="display:none;margin-top:8px"
       placeholder="nom exact du modèle (ex: qwen/qwen3.6-27b)">
<div class="grid">
  <div><label for="tp">Température</label>
       <p class="hint">(0 – 1.5)</p>
       <input id="tp" type="number" min="0" max="1.5" step="0.1"></div>
  <div><label for="mt">Longueur max</label>
       <p class="hint">(tokens, 20 – 1000)</p>
       <input id="mt" type="number" min="20" max="1000" step="10"></div>
</div>
<div class="row"><button class="pri" onclick="saveLlm()">Enregistrer</button></div>
<div class="cur foot">Endpoint : <span id="baseUrl">…</span></div>
<div class="msg" id="msgL"></div>
</div>

</div>
<div class="col">

<div class="box">
<h2>Personnalité</h2>
<p class="sub">Le contexte système envoyé au LLM. La date et l'heure y sont ajoutées automatiquement — inutile de les mentionner.</p>
<textarea id="sp" maxlength="4000" oninput="autoGrow(this)"></textarea>
<div class="row">
  <button class="warn" onclick="resetPrompt()">Rétablir le défaut</button>
  <button class="pri" onclick="savePrompt()">Enregistrer</button>
</div>
<div class="msg" id="msgP"></div>
</div>

</div>
<div class="col">

<div class="box">
<h2>Voix</h2>
<p class="sub">Écoute puis adopte — prise en compte immédiate.</p>
<label for="v">Voix disponibles</label>
<select id="v"></select>
<label for="t">Phrase de test</label>
<input id="t" value="Bonjour, il est vingt-deux heures et tout va bien.">
<div class="row">
  <button class="sec" onclick="preview()">Écouter</button>
  <button class="pri" onclick="adopt()">Adopter cette voix</button>
</div>
<audio id="a" controls></audio>
<div class="cur">Voix actuelle : <b id="curV">…</b></div>
<div class="msg" id="msgV"></div>
</div>

</div>
</div>
</div>

<div id="v-tools" class="view hidden">
<div class="layout">
<div class="col">

<div class="box">
<h2>Outils</h2>
<p class="sub">Le LLM décide lui-même d'appeler météo, actualités ou recherche web. Le pilotage MQTT n'est pas concerné, il garde son mot-clé déclencheur.</p>
<label for="tmt">Longueur MAX après appel d'outil (tokens)</label>
<input id="tmt" type="number" min="20" max="1000" step="10">
<div style="display:flex;align-items:center;gap:8px;margin-top:8px"><input type="checkbox" id="ws" style="width:auto"><span style="font-size:13px">Recherche web (DuckDuckGo) disponible</span></div>
<div class="row"><button class="pri" onclick="saveToolsMode()">Enregistrer</button></div>
<div class="msg" id="msgTM"></div>
</div>

<div class="box">
<h2>Météo</h2>
<p class="sub">Open-Meteo, appelé par l'outil get_weather ; la ville n'est qu'un libellé, la requête utilise lat/lon.</p>
<label for="wc">Ville</label>
<input id="wc" maxlength="60">
<div class="grid">
  <div><label for="wla">Latitude</label>
       <input id="wla" type="number" min="-90" max="90" step="0.01"></div>
  <div><label for="wlo">Longitude</label>
       <input id="wlo" type="number" min="-180" max="180" step="0.01"></div>
</div>
<div class="row"><button class="pri" onclick="saveWeather()">Enregistrer</button></div>
<div class="msg" id="msgW"></div>
</div>

</div>
<div class="col">

<div class="box">
<h2>Actualités</h2>
<p class="sub">Flux RSS appelés par l'outil get_news ; les titres sont fusionnés, dédoublonnés puis résumés à l'oral par le LLM.</p>
<label for="nf">Flux RSS (une URL par ligne)</label>
<textarea id="nf" style="min-height:60px" maxlength="1200"></textarea>
<label for="nc">Titres max</label>
<input id="nc" type="number" min="1" max="15" step="1">
<div class="row"><button class="pri" onclick="saveNews()">Enregistrer</button></div>
<div class="msg" id="msgN"></div>
</div>

</div>
<div class="col">

<div class="box">
<h2>Commandes vocales</h2>
<p class="sub">Jarvis pilote le dashboard quand la phrase contient un mot-clé déclencheur. Chaque outil traduit une demande en action MQTT (construite à la volée, éditable ici).</p>
<label for="ck">Mot(s)-clé déclencheur(s) (séparés par des virgules)</label>
<input id="ck" maxlength="200">
<p class="hint">Sans ce mot, la phrase repart en question normale — aucune action.</p>
<div id="toolBox"></div>
<div class="row">
  <button class="sec" onclick="addTool()">+ Nouvel outil</button>
  <button class="pri" onclick="saveCmds()">Enregistrer</button>
</div>
<div class="msg" id="msgK"></div>
</div>

</div>
</div>
</div>

<script>
// ---- HELPERS COMMUNS ----
let DEFAULTS = {};
function say(id,m,ok){const e=document.getElementById(id);e.textContent=m;e.className='msg '+(ok?'ok':'ko');
  if(ok&&m)setTimeout(()=>{e.textContent='';},4000);}
// Ajuste la hauteur d'un textarea à son contenu (scrollHeight nul si masqué :
// à rappeler quand l'onglet devient visible).
function autoGrow(el){ if(!el) return; el.style.height='auto'; el.style.height=(el.scrollHeight+4)+'px'; }

async function postConfig(body, msgId, okText){
  try{
    const r=await fetch('/config',{method:'POST',headers:{'Content-Type':'application/json'},
                                   body:JSON.stringify(body)});
    const d=await r.json();
    if(!r.ok) throw new Error((d.errors||['HTTP '+r.status]).join(' ; '));
    say(msgId, okText, true);
  }catch(e){ say(msgId,'Échec : '+e.message,false); }
}

// ---- PANNEAU MODÈLE LLM ----
function modelChanged(){
  const custom = document.getElementById('m').value === '__custom__';
  const inp = document.getElementById('mCustom');
  inp.style.display = custom ? 'block' : 'none';
  if (custom) inp.focus();
}
function saveLlm(){
  const sel = document.getElementById('m').value;
  const model = sel === '__custom__' ? document.getElementById('mCustom').value.trim() : sel;
  if (!model) { say('msgL','Indique un nom de modèle.',false); return; }
  postConfig({llm_model:model,
              temperature:document.getElementById('tp').value,
              max_tokens:document.getElementById('mt').value},'msgL','Réglages LLM enregistrés.');
}

// ---- PANNEAU PERSONNALITÉ ----
function savePrompt(){ postConfig({system_prompt:document.getElementById('sp').value},'msgP','Contexte enregistré.'); }
function resetPrompt(){ document.getElementById('sp').value=DEFAULTS.system_prompt;
                        postConfig({system_prompt:DEFAULTS.system_prompt},'msgP','Défaut rétabli.'); }

// ---- PANNEAU CONVERSATION ----
async function clearConv(){
  try{
    const r=await fetch('/conversation/clear',{method:'POST'});
    const d=await r.json();
    document.getElementById('histN').textContent=0;
    document.getElementById('histBox').innerHTML='';
    say('msgC',`Historique vidé (${d.cleared} échange(s)).`,true);
  }catch(e){ say('msgC','Échec : '+e.message,false); }
}
async function sendChat(){
  const inp=document.getElementById('chatInput');
  const text=inp.value.trim();
  if(!text) return;
  inp.value='';
  say('msgC','Envoi au LLM…',true);
  try{
    const r=await fetch('/chat',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({text})});
    const d=await r.json();
    if(!r.ok) throw new Error(d.error||('HTTP '+r.status));
    say('msgC','',true);
    loadConv();   // l'échange vient d'entrer dans l'historique
  }catch(e){ say('msgC','Échec : '+e.message,false); inp.value=text; }
}
// À chaud, NON sauvé : au redémarrage du bridge on repart des défauts ENV.
async function applyLimits(){
  const ex=parseInt(document.getElementById('limEx').value,10);
  const ttl=parseInt(document.getElementById('limTtl').value,10);
  try{
    const r=await fetch('/conversation/limits',{method:'POST',headers:{'Content-Type':'application/json'},
                                                body:JSON.stringify({max_exchanges:ex,ttl_s:ttl})});
    const d=await r.json();
    if(!r.ok) throw new Error((d.errors||['HTTP '+r.status]).join(' ; '));
    say('msgC','Appliqué à chaud (non sauvé — défaut ENV au redémarrage).',true);
  }catch(e){ say('msgC','Échec : '+e.message,false); }
}
function esc(s){return String(s).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));}
// Compte à rebours avant purge : la valeur serveur est resynchronisée à chaque
// loadConv (4 s), le ticker 1 s ne fait que l'égrener entre deux.
let _resetIn=null;
function renderReset(){
  const el=document.getElementById('resetIn');
  if(el) el.textContent=(_resetIn!=null && _resetIn>=0) ? ' — Reset dans '+_resetIn+' s' : '';
}
setInterval(()=>{ if(_resetIn!=null && _resetIn>0){ _resetIn--; renderReset(); } },1000);

async function loadConv(){
  try{
    const d=await (await fetch('/conversation')).json();
    document.getElementById('histN').textContent=Math.floor(d.history.length/2)+'/'+d.max_exchanges;
    _resetIn=d.reset_in_s; renderReset();
    const box=document.getElementById('histBox');
    const html = d.history.length
      ? d.history.map(m=>`<div class="${m.role==='user'?'u':'a'}">${esc(m.content)}</div>`).join('')
      : '<div class="empty">Aucun échange en mémoire.</div>';
    if(box.innerHTML!==html){ box.innerHTML=html; box.scrollTop=box.scrollHeight; }
  }catch(e){}
}

// ---- PANNEAU VOIX ----
async function preview(){
  const v=document.getElementById('v').value, t=document.getElementById('t').value;
  say('msgV','Synthèse en cours…',true);
  try{
    const r=await fetch(`/preview?voice=${encodeURIComponent(v)}&text=${encodeURIComponent(t)}`);
    if(!r.ok) throw new Error('HTTP '+r.status);
    const a=document.getElementById('a');
    a.src=URL.createObjectURL(await r.blob()); a.play(); say('msgV','',true);
  }catch(e){ say('msgV','Échec : '+e.message,false); }
}
async function adopt(){
  const v=document.getElementById('v').value;
  try{
    const r=await fetch('/voice',{method:'POST',headers:{'Content-Type':'application/json'},
                                  body:JSON.stringify({voice:v})});
    if(!r.ok) throw new Error('HTTP '+r.status);
    document.getElementById('curV').textContent=v;
    say('msgV','Adoptée.',true);
  }catch(e){ say('msgV','Échec : '+e.message,false); }
}

// ---- PANNEAU OUTILS ----
function saveToolsMode(){
  postConfig({tools_max_tokens:document.getElementById('tmt').value,
              web_search_enabled:document.getElementById('ws').checked},'msgTM','Outils enregistrés.');
}

// ---- PANNEAU MÉTÉO ----
function saveWeather(){
  postConfig({weather_city:document.getElementById('wc').value,
              weather_lat:document.getElementById('wla').value,
              weather_lon:document.getElementById('wlo').value},'msgW','Météo enregistrée.');
}

// ---- PANNEAU ACTUALITÉS ----
function saveNews(){
  postConfig({news_feeds:document.getElementById('nf').value,
              news_count:document.getElementById('nc').value},'msgN','Actualités enregistrées.');
}

// ---- PANNEAU COMMANDES VOCALES ----
let CTOOLS=[];
let lastTmpl=null;
function ecs(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));}
function renderTools(){
  const box=document.getElementById('toolBox');
  box.innerHTML=CTOOLS.map((t,ti)=>{
    if(!t._open){
      return `<div class="tool"><div class="hd" data-t="${ti}"><span>▸</span><span class="tw">${ecs(t.name||'sans nom')}</span><span class="st">${t.confirm?'confirmation':'immédiat'}</span></div></div>`;
    }
    const chips=(t.params||[]).filter(p=>p.name).map(p=>`<span class="chip" data-t="${ti}" data-p="${ecs(p.name)}">{${ecs(p.name)}}</span>`).join('');
    const prows=(t.params||[]).map((p,pi)=>`<div class="trow">
        <input class="f" data-t="${ti}" data-p="${pi}" data-k="name" value="${ecs(p.name||'')}" placeholder="nom">
        <select class="f" data-t="${ti}" data-p="${pi}" data-k="type"><option${p.type!=='choix'?' selected':''}>nombre</option><option${p.type==='choix'?' selected':''}>choix</option></select>
        <input class="f" data-t="${ti}" data-p="${pi}" data-k="spec" value="${ecs(p.spec||'')}" placeholder="${p.type==='choix'?'valeurs, séparées par des virgules':'min - max'}">
        <button class="xbtn delP" data-t="${ti}" data-p="${pi}" title="retirer">✕</button>
        <input class="f" data-t="${ti}" data-p="${pi}" data-k="desc" value="${ecs(p.desc||'')}" placeholder="aide pour le LLM (optionnel)" style="grid-column:1 / 4">
      </div>`).join('');
    return `<div class="tool op"><div class="hd" data-t="${ti}"><span>▾</span><span class="tw">${ecs(t.name||'sans nom')}</span><button class="tdel" data-t="${ti}">supprimer</button></div>
      <div class="bd">
      <label>Nom (identifiant)</label><input class="f" data-t="${ti}" data-k="name" value="${ecs(t.name||'')}">
      <label>Quand l'utiliser — décrit au LLM</label><input class="f" data-t="${ti}" data-k="description" value="${ecs(t.description||'')}">
      <label>Paramètres</label>${prows}
      <button class="mini addP" data-t="${ti}">+ paramètre</button>
      <label>Action — topic MQTT + charge utile</label>
      <div style="display:grid;grid-template-columns:150px 1fr;gap:6px">
        <input class="f" data-t="${ti}" data-k="topic" value="${ecs(t.topic||'esp32/cmd')}">
        <input class="f tmpl" data-t="${ti}" data-k="action" value="${ecs(t.action||'')}" placeholder="ex: volume:{percent}">
      </div>
      <p class="hint">Insérer un paramètre : ${chips||'(ajoute un paramètre)'}</p>
      <div style="display:flex;align-items:center;gap:8px;margin-top:8px"><input type="checkbox" class="cfx" data-t="${ti}" ${t.confirm?'checked':''} style="width:auto"><span style="font-size:13px">Demander confirmation avant d'exécuter</span></div>
      ${t.confirm?`<label>Question de confirmation</label><input class="f tmpl" data-t="${ti}" data-k="prompt" value="${ecs(t.prompt||'')}">`:''}
      <label>Phrase parlée</label><input class="f tmpl" data-t="${ti}" data-k="speak" value="${ecs(t.speak||'')}">
      </div></div>`;
  }).join('');
  bindTools();
}
function syncT(){
  document.querySelectorAll('#toolBox .f').forEach(el=>{
    const ti=+el.dataset.t,k=el.dataset.k;
    if(el.dataset.p!==undefined){CTOOLS[ti].params[+el.dataset.p][k]=el.value;}
    else{CTOOLS[ti][k]=el.value;}
  });
  document.querySelectorAll('#toolBox .cfx').forEach(el=>{CTOOLS[+el.dataset.t].confirm=el.checked;});
}
function bindTools(){
  const B=document.getElementById('toolBox');
  B.querySelectorAll('.hd').forEach(el=>el.onclick=e=>{if(e.target.closest('.tdel'))return;syncT();const t=CTOOLS[+el.dataset.t];t._open=!t._open;renderTools();});
  B.querySelectorAll('.tdel').forEach(el=>el.onclick=e=>{e.stopPropagation();syncT();CTOOLS.splice(+el.dataset.t,1);renderTools();});
  B.querySelectorAll('.cfx').forEach(el=>el.onchange=()=>{syncT();renderTools();});
  B.querySelectorAll('select.f').forEach(el=>el.onchange=()=>{syncT();renderTools();});
  B.querySelectorAll('.tmpl').forEach(el=>el.onfocus=()=>{lastTmpl=el;});
  B.querySelectorAll('.chip').forEach(el=>el.onclick=()=>{const f=(lastTmpl&&lastTmpl.dataset.t===el.dataset.t)?lastTmpl:B.querySelector('.tmpl[data-t="'+el.dataset.t+'"]');if(f){f.value+='{'+el.dataset.p+'}';f.focus();syncT();}});
  B.querySelectorAll('.addP').forEach(el=>el.onclick=()=>{syncT();CTOOLS[+el.dataset.t].params.push({name:'',type:'nombre',spec:'0 - 100',desc:''});renderTools();});
  B.querySelectorAll('.delP').forEach(el=>el.onclick=()=>{syncT();CTOOLS[+el.dataset.t].params.splice(+el.dataset.p,1);renderTools();});
}
function addTool(){syncT();CTOOLS.forEach(t=>t._open=false);CTOOLS.push({name:'nouvel_outil',description:'',confirm:true,topic:'esp32/cmd',action:'',speak:'',prompt:'',params:[],_open:true});renderTools();}
function saveCmds(){
  syncT();
  postConfig({command_keywords:document.getElementById('ck').value,
              command_tools:CTOOLS.map(t=>({name:t.name,description:t.description,confirm:!!t.confirm,
                topic:t.topic,action:t.action,speak:t.speak,prompt:t.prompt,params:t.params}))},
             'msgK','Commandes enregistrées.');
}

// ---- INITIALISATION ----
async function loadAll(){
  const cfg = await (await fetch('/config')).json();
  DEFAULTS = cfg.defaults;
  document.getElementById('sp').value = cfg.settings.system_prompt;
  document.getElementById('tp').value = cfg.settings.temperature;
  document.getElementById('mt').value = cfg.settings.max_tokens;
  document.getElementById('curV').textContent = cfg.settings.voice;
  document.getElementById('baseUrl').textContent = cfg.info.llm_base_url;
  document.getElementById('histN').textContent = cfg.info.history_len + '/' + cfg.info.conversation_max_exchanges;
  document.getElementById('limEx').value  = cfg.info.conversation_max_exchanges;
  document.getElementById('limTtl').value = cfg.info.conversation_ttl_s;
  document.getElementById('tmt').value = cfg.settings.tools_max_tokens || 200;
  document.getElementById('ws').checked = cfg.settings.web_search_enabled !== false;
  document.getElementById('wc').value  = cfg.settings.weather_city;
  document.getElementById('wla').value = cfg.settings.weather_lat;
  document.getElementById('wlo').value = cfg.settings.weather_lon;
  document.getElementById('nf').value  = cfg.settings.news_feeds.join('\\n');
  document.getElementById('nc').value  = cfg.settings.news_count;
  document.getElementById('ck').value  = (cfg.settings.command_keywords||[]).join(', ');
  CTOOLS = JSON.parse(JSON.stringify(cfg.settings.command_tools||[]));
  renderTools();   // tous repliés par défaut

  const vs = await (await fetch('/voices')).json();
  const sel = document.getElementById('v');
  vs.voices.forEach(v=>{
    const o=document.createElement('option');
    o.value=v.name; o.textContent=`${v.name}  —  ${v.gender}  (${v.locale})`;
    if(v.name===vs.current) o.selected=true; sel.appendChild(o);
  });

  const md = await (await fetch('/llm_models')).json();
  const ms = document.getElementById('m');
  const cur = cfg.settings.llm_model;
  const list = md.models.slice();
  if (cur && !list.includes(cur)) list.unshift(cur);
  list.forEach(id=>{
    const o=document.createElement('option');
    o.value=id; o.textContent=id + (id===cur ? '   (actuel)' : '');
    if(id===cur) o.selected=true;
    ms.appendChild(o);
  });
  const oc=document.createElement('option');
  oc.value='__custom__'; oc.textContent='Autre modèle (saisie libre)…';
  ms.appendChild(oc);
}
// ---- ONGLETS (calqué sur activity_monitor) ----
const _views = {conv:document.getElementById('v-conv'), llm:document.getElementById('v-llm'), tools:document.getElementById('v-tools')};
document.querySelectorAll('.tab').forEach(t=>t.addEventListener('click',()=>{
  document.querySelectorAll('.tab').forEach(x=>x.classList.remove('active'));
  t.classList.add('active');
  for(const k in _views) _views[k].classList.toggle('hidden', k!==t.dataset.view);
  if(t.dataset.view==='llm') autoGrow(document.getElementById('sp'));   // scrollHeight fiable une fois visible
}));

loadAll();
loadConv();
setInterval(loadConv, 4000);
</script></body></html>"""


@app.route("/", methods=["GET"])
def config_page():
    return Response(CONFIG_PAGE, mimetype="text/html; charset=utf-8")


@app.route("/voices", methods=["GET"])
def voices():
    return {"voices": _voices_fr(), "current": _settings["voice"]}


@app.route("/voice", methods=["POST"])
def set_voice():
    """Change la voix à chaud et la persiste. Aucun redémarrage nécessaire."""
    data  = request.get_json(silent=True) or {}
    voice = (data.get("voice") or "").strip()
    if not voice:
        return "", 400
    if voice not in [v["name"] for v in _voices_fr()]:
        return "voix inconnue", 400

    _settings["voice"] = voice
    _settings_save()
    print(f"[Bridge] Voix changée : {voice}")
    return {"voice": voice}


@app.route("/preview", methods=["GET"])
def preview():
    """Aperçu d'une voix SANS l'adopter. Renvoie du WAV (et non du PCM brut comme
    /say) parce que le destinataire est ici un <audio> de navigateur, pas l'ESP32."""
    voice = (request.args.get("voice") or _settings["voice"]).strip()
    text  = (request.args.get("text") or "Bonjour, ceci est un essai de voix.").strip()

    pcm = tts_speak(text, voice=voice)
    if not pcm:
        return "", 500
    return Response(pcm_to_wav(pcm), mimetype="audio/wav")


@app.route("/config", methods=["GET"])
def get_config():
    return {"settings": _settings, "defaults": DEFAULT_SETTINGS,
            "info": {"llm_base_url": LLM_BASE_URL,
                     "conversation_ttl_s": CONVERSATION_TTL_S,
                     "conversation_max_exchanges": CONVERSATION_MAX_EXCHANGES,
                     "history_len": len(conversation) // 2}}


@app.route("/config", methods=["POST"])
def set_config():
    """Mise à jour partielle des paramètres, avec bornes : un contexte vide ou une
    température délirante casseraient l'assistant depuis un simple formulaire."""
    data = request.get_json(silent=True) or {}
    errors = []

    if "system_prompt" in data:
        sp = str(data["system_prompt"]).strip()
        if 10 <= len(sp) <= 4000:
            _settings["system_prompt"] = sp
        else:
            errors.append("contexte : 10 à 4000 caractères")

    if "llm_model" in data:
        m = str(data["llm_model"]).strip()
        if m:
            _settings["llm_model"] = m
        else:
            errors.append("modèle vide")

    if "temperature" in data:
        try:
            t = float(data["temperature"])
            if not (0.0 <= t <= 1.5):
                raise ValueError
            _settings["temperature"] = t
        except (TypeError, ValueError):
            errors.append("température : 0 à 1.5")

    if "max_tokens" in data:
        try:
            n = int(data["max_tokens"])
            if not (20 <= n <= 1000):
                raise ValueError
            _settings["max_tokens"] = n
        except (TypeError, ValueError):
            errors.append("max_tokens : 20 à 1000")

    if "tools_max_tokens" in data:
        try:
            n = int(data["tools_max_tokens"])
            if not (20 <= n <= 1000):
                raise ValueError
            _settings["tools_max_tokens"] = n
        except (TypeError, ValueError):
            errors.append("tools_max_tokens : 20 à 1000")

    if "web_search_enabled" in data:
        _settings["web_search_enabled"] = bool(data["web_search_enabled"])

    if "weather_city" in data:
        c = str(data["weather_city"]).strip()
        if 1 <= len(c) <= 60:
            _settings["weather_city"] = c
        else:
            errors.append("ville : 1 à 60 caractères")

    if "weather_lat" in data:
        try:
            lat = float(data["weather_lat"])
            if not (-90.0 <= lat <= 90.0):
                raise ValueError
            _settings["weather_lat"] = lat
        except (TypeError, ValueError):
            errors.append("latitude : -90 à 90")

    if "weather_lon" in data:
        try:
            lon = float(data["weather_lon"])
            if not (-180.0 <= lon <= 180.0):
                raise ValueError
            _settings["weather_lon"] = lon
        except (TypeError, ValueError):
            errors.append("longitude : -180 à 180")

    if "news_feeds" in data:
        raw = data["news_feeds"]
        # Une URL par ligne depuis la page ; virgules et liste JSON tolérées.
        if isinstance(raw, str):
            feeds = [u.strip() for u in raw.replace(",", "\n").splitlines()]
        elif isinstance(raw, list):
            feeds = [str(u).strip() for u in raw]
        else:
            feeds = []
        feeds = [u for u in feeds if u]
        if (1 <= len(feeds) <= 8 and
                all(u.startswith(("http://", "https://")) and len(u) <= 300 for u in feeds)):
            _settings["news_feeds"] = feeds
        else:
            errors.append("flux RSS : 1 à 8 URL http(s) de 300 caractères max")

    if "news_count" in data:
        try:
            n = int(data["news_count"])
            if not (1 <= n <= 15):
                raise ValueError
            _settings["news_count"] = n
        except (TypeError, ValueError):
            errors.append("titres max : 1 à 15")

    if "command_keywords" in data:
        raw = data["command_keywords"]
        if isinstance(raw, str):
            kws = [k.strip().lower() for k in raw.split(",")]
        elif isinstance(raw, list):
            kws = [str(k).strip().lower() for k in raw]
        else:
            kws = []
        kws = [k for k in kws if k]
        if 1 <= len(kws) <= 10 and all(len(k) <= 40 for k in kws):
            _settings["command_keywords"] = kws
        else:
            errors.append("mots-clés déclencheurs : 1 à 10 entrées de 40 caractères max")

    if "command_tools" in data:
        # Outils construits côté page : on borne tout (nom = identifiant sûr pour le
        # function-calling, longueurs, 20 outils / 6 params max) avant de garder.
        raw = data["command_tools"]
        if not isinstance(raw, list) or len(raw) > 20:
            errors.append("outils : liste de 20 maximum")
        else:
            def _ident(s):
                return "".join(c for c in str(s).strip() if c.isalnum() or c in "_-")[:40]
            def _clip(s, n):
                return str(s).strip()[:n]
            tools = []
            for t in raw:
                if not isinstance(t, dict):
                    continue
                name = _ident(t.get("name", ""))
                if not name:
                    continue
                params = []
                for p in (t.get("params") or [])[:6]:
                    if not isinstance(p, dict):
                        continue
                    pn = _ident(p.get("name", ""))
                    if not pn:
                        continue
                    params.append({"name": pn,
                                   "type": "choix" if p.get("type") == "choix" else "nombre",
                                   "spec": _clip(p.get("spec", ""), 200),
                                   "desc": _clip(p.get("desc", ""), 200)})
                tools.append({"name": name,
                              "description": _clip(t.get("description", ""), 200),
                              "confirm": bool(t.get("confirm")),
                              "topic": _clip(t.get("topic", "") or "esp32/cmd", 100) or "esp32/cmd",
                              "action": _clip(t.get("action", ""), 200),
                              "speak": _clip(t.get("speak", ""), 200),
                              "prompt": _clip(t.get("prompt", ""), 200),
                              "params": params})
            _settings["command_tools"] = tools

    # Sauvegarde AVANT le compte-rendu d'erreurs : les champs valides sont déjà
    # appliqués en mémoire, ils doivent l'être sur disque aussi.
    _settings_save()
    if errors:
        return {"errors": errors}, 400

    # Valeurs affichées, tronquées : « clé mise à jour » ne dit pas si la valeur
    # reçue est celle qu'on croit.
    print("[Bridge] Paramètres appliqués : "
          + ", ".join(f"{k}={repr(_settings[k])[:40]}" for k in sorted(data) if k in _settings))
    return {"settings": _settings}


@app.route("/llm_models", methods=["GET"])
def llm_models():
    """Modèles RÉELLEMENT disponibles sur l'endpoint LLM configuré (GET /models de
    l'API OpenAI-compatible) — pas une liste codée en dur qui périmerait. Les
    modèles non conversationnels (whisper, tts, guard...) sont écartés."""
    try:
        exclude = ("whisper", "tts", "guard", "embed", "moderation")
        ids = sorted(m.id for m in llm_client.models.list()
                     if not any(x in m.id.lower() for x in exclude))
        return {"models": ids}
    except Exception as e:
        print(f"[Bridge] Liste des modèles indisponible : {e}")
        return {"models": []}


@app.route("/conversation/clear", methods=["POST"])
def conversation_clear():
    """Vide l'historique conversationnel — le bouton « nouvelle conversation »
    qui manquait quand une hallucination Whisper s'installait dans le contexte."""
    n = _conversation_clear()
    print(f"[Bridge] Historique vidé ({n} échanges)")
    return {"cleared": n}

@app.route("/conversation", methods=["GET"])
def conversation_get():
    """Historique conversationnel courant (purge TTL appliquée au passage) + de
    quoi alimenter le compteur : profondeur max et compte à rebours avant purge."""
    hist = _conversation_history()
    reset_in = None
    if hist and _last_exchange_ts:
        reset_in = max(0, int(CONVERSATION_TTL_S - (time.monotonic() - _last_exchange_ts)))
    return {"history": hist, "max_exchanges": CONVERSATION_MAX_EXCHANGES, "reset_in_s": reset_in}

@app.route("/conversation/limits", methods=["POST"])
def conversation_limits():
    """Ajuste À CHAUD la profondeur (nb d'échanges) et le TTL de l'historique.
    NON persisté : au redémarrage on repart des valeurs d'env (défauts). Bornes
    pour éviter d'envoyer un contexte délirant au LLM à chaque requête."""
    global conversation, CONVERSATION_TTL_S, CONVERSATION_MAX_EXCHANGES
    data = request.get_json(silent=True) or {}
    errors = []
    try:
        ex = int(data["max_exchanges"])
        if not (1 <= ex <= 100):
            raise ValueError
    except (KeyError, TypeError, ValueError):
        errors.append("échanges : entier 1 à 100")
    try:
        ttl = int(data["ttl_s"])
        if not (30 <= ttl <= 86400):
            raise ValueError
    except (KeyError, TypeError, ValueError):
        errors.append("durée : entier 30 à 86400 s")
    if errors:
        return {"errors": errors}, 400

    CONVERSATION_MAX_EXCHANGES = ex
    CONVERSATION_TTL_S = ttl
    conversation = deque(conversation, maxlen=ex * 2)   # préserve le fil, tronque le plus vieux
    print(f"[Bridge] Limites conversation (à chaud) : {ex} échanges, TTL {ttl}s")
    return {"max_exchanges": ex, "ttl_s": ttl, "history_len": len(conversation) // 2}

@app.route("/chat", methods=["POST"])
def chat():
    """Requête LLM DIRECTE depuis la page config : ni STT, ni TTS, ni MQTT, ni
    détection de commande — juste le LLM. L'échange entre dans l'historique
    PARTAGÉ (llm_answer appelle _conversation_remember), la page le rafraîchit
    ensuite via /conversation."""
    data = request.get_json(silent=True) or {}
    text = (data.get("text") or "").strip()
    if not text:
        return "", 400
    print(f"[Bridge] Chat direct : {text}")
    answer = llm_answer(text, None)
    if answer is None:
        return {"error": "LLM indisponible"}, 500
    return {"answer": answer}


# ----------------------------------------------------------------
# COMMANDES VOCALES — pilotage du dashboard (esp32/cmd)
# ----------------------------------------------------------------
# Portillon "pilote" (command_keywords) : une phrase n'est routée vers le
# classificateur QUE si elle contient un mot-clé de pilotage — sinon le flux Q-R
# reste intact, sans appel LLM supplémentaire.
# Le classificateur traduit la phrase en action via le TOOL-CALLING du LLM. Les
# réglages (volume/luminosité) passent par une confirmation vocale, la
# navigation s'exécute direct.
_CMD_YES = ("oui", "ouais", "ouaip", "vas-y", "vas y", "confirme", "c'est bon",
            "ok", "d'accord", "yes", "carrément")
_CMD_NO  = ("non", "annule", "laisse tomber", "laisse", "stop", "négatif")

def _said(low: str, words) -> bool:
    """Mot-clé cherché comme MOT ENTIER, jamais en sous-chaîne : « non » est
    contenu dans « annonce », « ok » dans « stock », « laisse » dans « délaisse ».
    Une confirmation en attente était donc annulée par une phrase anodine.
    (?<!\\w)/(?!\\w) plutôt que \\b : les entrées commencent/finissent parfois
    par autre chose qu'un caractère de mot."""
    return any(re.search(r"(?<!\w)" + re.escape(w) + r"(?!\w)", low) for w in words)

# Outils = DONNÉES (éditables via la page config). Le schéma function-calling et
# l'action MQTT sont construits À LA VOLÉE : ajouter un outil = une entrée dans
# command_tools, zéro code. Un param est de type "nombre" (spec "min - max") ou
# "choix" (spec "a, b, c"). Les {param} du gabarit/phrases sont substitués.
def _parse_range(spec):
    """(min, max) extrait d'un « 0 - 100 » (les 2 premiers nombres trouvés)."""
    tok, nums = "", []
    for ch in str(spec) + " ":
        if ch.isdigit():
            tok += ch
        elif tok:
            nums.append(int(tok)); tok = ""
    if len(nums) >= 2:
        return min(nums[0], nums[1]), max(nums[0], nums[1])
    return 0, 100

def _choices(spec):
    return [c.strip() for c in str(spec).split(",") if c.strip()]

def _tool_schema(t: dict):
    """Un outil (données) -> schéma function-calling OpenAI/Groq (None si sans nom)."""
    name = (t.get("name") or "").strip()
    if not name:
        return None
    props, required = {}, []
    for p in t.get("params", []):
        pn = (p.get("name") or "").strip()
        if not pn:
            continue
        if p.get("type") == "choix":
            props[pn] = {"type": "string", "enum": _choices(p.get("spec"))}
        else:
            lo, hi = _parse_range(p.get("spec"))
            props[pn] = {"type": "integer", "minimum": lo, "maximum": hi}
        if p.get("desc"):
            props[pn]["description"] = p["desc"]
        required.append(pn)
    return {"type": "function", "function": {
        "name": name, "description": t.get("description", ""),
        "parameters": {"type": "object", "properties": props, "required": required}}}

# Une seule action en attente de confirmation (un seul ESP32), avec TTL : un
# "oui" tardif ne doit pas déclencher une commande oubliée.
_pending_command: dict | None = None
_pending_ts: float = 0.0
COMMAND_PENDING_TTL_S = 30

def _clamp_int(v, lo, hi):
    try:
        return max(lo, min(hi, int(v)))
    except (TypeError, ValueError):
        return None

def _execute_command(action: dict):
    mqtt_pub(action["topic"], action["payload"])   # one-shot, non retained
    print(f"[Bridge] Commande vocale exécutée : {action['topic']} = {action['payload']}")

def _classify_command(text: str) -> dict | None:
    """Phrase pilotée -> action structurée, via le tool-calling sur les outils
    définis dans les settings. Renvoie None si aucun outil ne correspond (la
    phrase repart en flux normal) ou en cas d'échec du modèle."""
    tools = _settings.get("command_tools", [])
    schemas = [s for s in (_tool_schema(t) for t in tools) if s]
    if not schemas:
        return None
    try:
        resp = llm_client.chat.completions.create(
            model=_settings["llm_model"],
            messages=[
                {"role": "system", "content":
                    "Tu pilotes un dashboard domestique. Traduis la demande en appel "
                    "d'outil si elle correspond à une action disponible ; sinon "
                    "n'appelle aucun outil."},
                {"role": "user", "content": text},
            ],
            # ⚠️ Raisonnement COUPÉ (extra_body) : sinon un modèle à raisonnement
            # brûle le budget de tokens à « réfléchir » et l'appel d'outil sort
            # tronqué (400 tool_use_failed).
            tools=schemas, tool_choice="auto",
            temperature=0, max_tokens=512, extra_body=_llm_extra_body(),
        )
    except Exception as e:
        print(f"[Bridge] Classification commande impossible : {e}")
        return None

    calls = resp.choices[0].message.tool_calls
    if not calls:
        return None
    fn = calls[0].function
    try:
        args = json.loads(fn.arguments or "{}")
    except Exception:
        return None

    tdef = next((t for t in tools if (t.get("name") or "").strip() == fn.name), None)
    if not tdef:
        return None

    # Validation (bornes/choix) puis substitution des {param} dans les gabarits.
    values = {}
    for p in tdef.get("params", []):
        pn = (p.get("name") or "").strip()
        if not pn:
            continue
        raw = args.get(pn)
        if p.get("type") == "choix":
            allowed = _choices(p.get("spec"))
            sv = str(raw).strip() if raw is not None else ""
            if allowed and sv not in allowed:
                return None
            values[pn] = sv
        else:
            lo, hi = _parse_range(p.get("spec"))
            iv = _clamp_int(raw, lo, hi)
            if iv is None:
                return None
            values[pn] = iv

    def _fill(s: str) -> str:
        for k, v in values.items():
            s = s.replace("{" + k + "}", str(v))
        return s

    return {"topic":   (tdef.get("topic") or "esp32/cmd").strip() or "esp32/cmd",
            "payload":  _fill(tdef.get("action", "")),
            "confirm":  bool(tdef.get("confirm")),
            "prompt":   _fill(tdef.get("prompt", "")) or "Tu veux confirmer ?",
            "speak":    _fill(tdef.get("speak", "")) or "C'est fait."}

def _speak_response(text: str, transcript: str, listen_after: bool = False):
    """Synthétise `text`, publie ai/answer et renvoie la réponse HTTP (même
    forme que _answer_and_speak). listen_after -> en-tête X-Listen-After : le
    firmware ré-arme l'écoute à la fin de la lecture (cf. ai_manager)."""
    pcm_out = tts_speak(text)
    if not pcm_out:
        mqtt_pub("ai/status", "error")
        return "", 500
    mqtt_pub("ai/answer", text)
    resp = Response(pcm_out, mimetype="application/octet-stream")
    resp.headers["X-Transcript"] = quote(transcript)
    resp.headers["X-Answer"] = quote(text)
    if listen_after:
        resp.headers["X-Listen-After"] = "1"
    return resp

def maybe_handle_command(text: str):
    """Court-circuit du flux Q-R si `text` est une commande ou une réponse à une
    confirmation en attente. Renvoie une Response, ou None pour laisser passer."""
    global _pending_command, _pending_ts
    # Apostrophe typographique normalisée : Whisper rend « c'est bon » avec U+2019.
    low = text.lower().replace("’", "'")

    # 1) Réponse à une confirmation en attente (non périmée) ?
    if _pending_command and (time.monotonic() - _pending_ts) <= COMMAND_PENDING_TTL_S:
        if _said(low, _CMD_YES):
            action, _pending_command = _pending_command, None
            _execute_command(action)
            return _speak_response(action["speak"], text)
        if _said(low, _CMD_NO):
            _pending_command = None
            return _speak_response("D'accord, j'annule.", text)
        # Ni oui ni non : on oublie l'attente et on traite la phrase normalement.
        _pending_command = None
    else:
        _pending_command = None   # périmée

    # 2) Portillon : sans mot-clé de pilotage, ce n'est pas une commande.
    if not any(k in low for k in _settings.get("command_keywords", [])):
        return None

    action = _classify_command(text)
    if not action:
        return _speak_response("Je n'ai pas compris la commande.", text)

    if action["confirm"]:
        _pending_command, _pending_ts = action, time.monotonic()
        return _speak_response(action["prompt"], text, listen_after=True)

    _execute_command(action)          # navigation : immédiat, sans confirmation
    return _speak_response(action["speak"], text)

def _answer_and_speak(text: str):
    """Partie commune à /ask et /ask_text, une fois le texte de la
    question connu : commande -> sinon LLM (+ outils) -> TTS -> réponse."""
    cmd_resp = maybe_handle_command(text)
    if cmd_resp is not None:
        return cmd_resp
    answer = llm_answer(text, None)          # le modèle décide seul de ses outils
    if not answer:
        mqtt_pub("ai/status", "error")
        return "", 500
    print(f"[Bridge] Réponse : {answer}")

    pcm_out = tts_speak(answer)
    if not pcm_out:
        mqtt_pub("ai/status", "error")
        return "", 500

    # Publié seulement une fois le TTS prêt — évite que le texte apparaisse
    # sur MQTT avant que l'ESP32 ne passe réellement en "speaking".
    mqtt_pub("ai/answer", answer)

    resp = Response(pcm_out, mimetype="application/octet-stream")
    resp.headers["X-Transcript"] = quote(text)
    resp.headers["X-Answer"] = quote(answer)
    return resp

# ----------------------------------------------------------------
# API PUBLIQUES
# ----------------------------------------------------------------
def main():
    if not _settings_load():   # défauts repo absents/corrompus -> bridge KO, nas/freebox OK
        return
    print(f"[Bridge] Démarrage — LLM: {LLM_BASE_URL} ({_settings['llm_model']}) "
          f"— TTS: {_settings['voice']}")
    print(f"[Bridge] Paramètres IA : http://<NAS>:{HTTP_PORT}/")

    if not GROQ_API_KEY:
        print("[Bridge] ERREUR : GROQ_API_KEY non défini (requis pour le STT) !")
        return

    def on_connect(client, userdata, flags, reason_code, properties):
        if reason_code == 0:
            print("[Bridge] MQTT connecté OK")
        else:
            print(f"[Bridge] MQTT erreur : {reason_code}")

    mqtt_client.on_connect = on_connect

    try:
        mqtt_client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
    except Exception as e:
        print(f"[Bridge] MQTT impossible de se connecter : {e}")
        return

    mqtt_client.loop_start()

    app.run(host=HTTP_HOST, port=HTTP_PORT, threaded=True)

if __name__ == "__main__":
    main()
