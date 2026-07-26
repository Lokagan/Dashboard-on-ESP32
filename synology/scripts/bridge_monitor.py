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
# RESSOURCES BIBLIOTHÈQUES
# ----------------------------------------------------------------
import os
import io
import json
import time
import wave
import asyncio
from collections import deque
from datetime import datetime
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

# ----------------------------------------------------------------
# OBJETS GLOBAUX
# ----------------------------------------------------------------
MQTT_BROKER = os.getenv("MQTT_BROKER", "192.168.1.1")
MQTT_PORT   = int(os.getenv("MQTT_PORT", "1883"))

HTTP_HOST = os.getenv("AI_BRIDGE_HOST", "0.0.0.0")
HTTP_PORT = int(os.getenv("AI_BRIDGE_PORT", "8090"))

SAMPLE_RATE = int(os.getenv("AUDIO_SAMPLE_RATE", "16000"))  # doit matcher AUDIO_SAMPLE_RATE (config.h)

# Marge sous la saturation après normalisation de l'audio envoyé au STT, en dB.
# Compense la distance variable au micro — l'ESP32 capture désormais en linéaire
# (ALC matérielle désactivée, cf. normalize_pcm). Mettre à 0 (ou vide) désactive.
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

# reasoning_effort / reasoning_format sont des extensions Groq, à passer par
# extra_body et NON en kwargs : absents de la signature de create(), le SDK
# lèverait un TypeError avant même d'émettre la requête.
# ⚠️ Et seuls les modèles À RAISONNEMENT les acceptent — un llama/kimi répond
# 400. Le refus est donc mémorisé par modèle (cf. le retry de llm_answer).
LLM_IS_GROQ = "groq.com" in LLM_BASE_URL
_no_reasoning_models: set[str] = set()

def _llm_extra_body() -> dict:
    if not LLM_IS_GROQ or _settings["llm_model"] in _no_reasoning_models:
        return {}
    return {"reasoning_effort": "none", "reasoning_format": "hidden"}

# --- Paramètres IA modifiables À CHAUD (page http://<NAS>:8090/) ---
# DEUX fichiers, DEUX rôles, plus AUCUN doublon avec monitor.env :
#   bridge_defaults.json (repo, versionné) -> valeurs par DÉFAUT, éditées à froid
#   bridge_settings.json (NAS, hors repo)  -> overrides SAUVÉS par l'interface web
# monitor.env ne garde QUE le structurel (broker/hôtes/ports/secrets/cadence) ;
# voix, modèle, prompt, météo et actus n'y sont plus. Le défaut est sans secret,
# donc versionnable. Gotchas des mots-clés (le JSON ne porte pas de commentaire) :
#   météo : fragments COURTS ("quel temps fait-il" ratait "quel temps il fait") ;
#           "pleuv" et non "pleu" (qui prendrait "pleurer") ; weather_city n'est
#           qu'un libellé, ce sont lat/lon qui font la requête.
#   actus : "que se passe" attrape "que se passe-t-il" ; "actu" couvre
#           actualité/actualités ; seuls les <title> RSS 2.0 sont lus (pas Atom).
#   prompt : pas de date/heure ici (jamais passé à .format()) — ajoutée dans
#            llm_answer() via _current_datetime_fr().
DEFAULTS_FILE          = os.getenv("BRIDGE_DEFAULTS_FILE", "/app/scripts/bridge_defaults.json")
SETTINGS_FILE          = os.getenv("BRIDGE_SETTINGS_FILE", "/app/scripts/bridge_settings.json")
TTS_VOICE_FILE_LEGACY  = "/app/scripts/tts_voice.json"   # ancien fichier voix seule, migré puis ignoré

# Peuplés au démarrage (main) depuis DEFAULTS_FILE, PAS à l'import : une erreur de
# fichier ne doit tuer que le thread bridge, jamais nas/freebox_monitor —
# monitor.py importe les trois dans le même process.
DEFAULT_SETTINGS: dict = {}
_settings: dict = {}

# --- Date du jour (injectée dans le prompt système — aucun LLM ne connaît la
#     date courante par lui-même, quel que soit le modèle) ---
_JOURS = ["lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi", "dimanche"]
_MOIS  = ["janvier", "février", "mars", "avril", "mai", "juin", "juillet",
          "août", "septembre", "octobre", "novembre", "décembre"]

# --- Historique conversationnel ---
# Le prompt système demande explicitement de garder le contexte, il faut donc
# le purger : sans TTL, une question posée le lendemain arriverait avec dix
# échanges périmés que le modèle prendrait pour la suite de la conversation.
CONVERSATION_TTL_S = int(os.getenv("AI_CONVERSATION_TTL_S", "600"))  # 10 min d'inactivité
conversation = deque(maxlen=20)   # 10 derniers échanges (Q+R = 2 entrées)
_last_exchange_ts = 0.0

llm_client = OpenAI(base_url=LLM_BASE_URL, api_key=GROQ_API_KEY)
mqtt_client = mqtt.Client(callback_api_version=CallbackAPIVersion.VERSION2, client_id="ai-bridge")

app = Flask(__name__)

# ----------------------------------------------------------------
# HELPERS
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
    now = datetime.now()
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
    # maxlen coupe par le début : on peut se retrouver à commencer par un
    # message "assistant" orphelin (sans le "user" correspondant). Groq
    # l'accepte, d'autres backends non.
    if hist and hist[0]["role"] == "assistant":
        hist.pop(0)
    return hist

def _conversation_remember(question: str, answer: str):
    global _last_exchange_ts
    conversation.append({"role": "user", "content": question})
    conversation.append({"role": "assistant", "content": answer})
    _last_exchange_ts = time.monotonic()

# ----------------------------------------------------------------
# API LOCALES
# ----------------------------------------------------------------

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
# absent retombe sur "" (pas de condition mentionnée). Sert au présent ET aux
# prévisions — jusqu'ici weather_code était récupéré puis ignoré.
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

# --- 2. Météo — Open-Meteo (contexte injecté dans le prompt si pertinent) ---
def maybe_fetch_weather(text: str) -> str | None:
    low = text.lower()
    if not any(k in low for k in _settings["weather_keywords"]):
        return None
    # Horizon détecté par mots-clés, défaut = météo actuelle. "après-demain"
    # contient "demain", donc testé d'abord. Ces mots sont intrinsèques (comme
    # "pleuv"), non configurables via la page.
    if any(k in low for k in ("semaine", "prochains jours", "jours à venir", "jours qui viennent")):
        horizon = "week"
    elif "après-demain" in low or "apres-demain" in low:
        horizon = 2
    elif "demain" in low:
        horizon = 1
    else:
        horizon = "now"

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

# --- 2b. Actualités — flux RSS (contexte injecté dans le prompt si pertinent) ---
def maybe_fetch_news(text: str) -> str | None:
    if not any(k in text.lower() for k in _settings["news_keywords"]):
        return None
    # Titres collectés PAR flux, puis ENTRELACÉS (round-robin) : sans ça, un flux
    # volumineux en tête (franceinfo, 35 titres) remplit à lui seul news_count et
    # un flux local ajouté après (Angoulême) n'apparaît JAMAIS. L'entrelacement
    # garantit que chaque flux contribue.
    per_feed: list[list[str]] = []
    for url in _settings["news_feeds"]:
        try:
            r = requests.get(url, timeout=10, headers={"User-Agent": "dashboard-bridge"})
            r.raise_for_status()
            root = ElementTree.fromstring(r.content)
            per_feed.append([t for it in root.iter("item")          # RSS 2.0
                             if (t := (it.findtext("title") or "").strip())])
        except Exception as e:
            print(f"[Bridge] Erreur actualités ({url}) : {e}")
    titles: list[str] = []
    for rank in range(max((len(f) for f in per_feed), default=0)):
        for feed in per_feed:
            if rank < len(feed) and feed[rank] not in titles:
                titles.append(feed[rank])
        if len(titles) >= _settings["news_count"]:
            break
    titles = titles[:_settings["news_count"]]
    if not titles:
        return None
    lignes = "\n".join(f"{i}. {t}" for i, t in enumerate(titles, 1))
    return ("Titres d'actualité du jour (résume à l'oral les principaux, "
            f"sans tous les énumérer) :\n{lignes}")

# --- 3. LLM — endpoint compatible OpenAI (modulable via LLM_BASE_URL/MODEL) ---
def llm_answer(question: str, context: str | None, max_tokens: int | None = None) -> str | None:
    # Contexte, modèle, température et longueur viennent de _settings :
    # modifiables à chaud depuis la page http://<NAS>:8090/. max_tokens peut
    # être forcé par l'appelant (réponse actu plus longue), sinon défaut settings.
    system = f"{_settings['system_prompt']}\n\nNous sommes le {_current_datetime_fr()}."
    if context:
        system += f"\n\n{context}"
    messages = [{"role": "system", "content": system}]
    messages.extend(_conversation_history())
    messages.append({"role": "user", "content": question})

    # 2 tentatives : si le modèle refuse les paramètres reasoning (400 sur les
    # modèles non-raisonnement de Groq), on mémorise le refus et on rejoue
    # immédiatement sans — l'aller-retour d'erreur n'est payé qu'une fois par
    # modèle, les requêtes suivantes partent directement sans extra_body.
    for attempt in (1, 2):
        try:
            resp = llm_client.chat.completions.create(
                model=_settings["llm_model"],
                messages=messages,
                temperature=_settings["temperature"],
                max_tokens=max_tokens or _settings["max_tokens"],
                extra_body=_llm_extra_body()
            )
            answer = resp.choices[0].message.content.strip()
            _conversation_remember(question, answer)
            return answer
        except Exception as e:
            if attempt == 1 and "reasoning" in str(e).lower():
                _no_reasoning_models.add(_settings["llm_model"])
                print(f"[Bridge] {_settings['llm_model']} refuse les paramètres reasoning — retry sans")
                continue
            print(f"[Bridge] Erreur LLM ({LLM_BASE_URL}, {_settings['llm_model']}) : {e}")
            # NB : un 429 (quota Groq atteint) n'est pas distingué du reste — le
            # firmware ne reçoit qu'un 500 générique et affiche "erreur".
            return None

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
# Tout est same-origin (page, listes, aperçu, validation) : aucun souci de CORS,
# aucune modification du firmware ESP32, aucun redémarrage du conteneur. Le
# bouton « Config NAS » du panneau web de l'ESP32 pointe ici (AI_BRIDGE_UI_URL).

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
</style></head><body>

<h1>Paramètres de Jarvis</h1>

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

<div class="box">
<h2>Personnalité</h2>
<p class="sub">Le contexte système envoyé au LLM. La date et l'heure y sont ajoutées automatiquement — inutile de les mentionner.</p>
<textarea id="sp" maxlength="4000"></textarea>
<div class="row">
  <button class="warn" onclick="resetPrompt()">Rétablir le défaut</button>
  <button class="pri" onclick="savePrompt()">Enregistrer</button>
</div>
<div class="msg" id="msgP"></div>
</div>

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
<div class="col">

<div class="box">
<h2>Conversation</h2>
<p class="sub">L'historique donné au LLM comme contexte (purge auto après inactivité).</p>
<div class="cur"><b id="histN">…</b> échange(s) en mémoire</div>
<div class="row">
  <button class="warn" onclick="clearConv()">Nouvelle conversation</button>
</div>
<div id="histBox" class="histbox"></div>
<div class="msg" id="msgC"></div>
</div>

</div>
<div class="col">

<div class="box">
<h2>Météo</h2>
<p class="sub">Open-Meteo est interrogé quand la question contient un mot-clé ; la ville n'est qu'un libellé, la requête utilise lat/lon.</p>
<label for="wc">Ville</label>
<input id="wc" maxlength="60">
<div class="grid">
  <div><label for="wla">Latitude</label>
       <input id="wla" type="number" min="-90" max="90" step="0.01"></div>
  <div><label for="wlo">Longitude</label>
       <input id="wlo" type="number" min="-180" max="180" step="0.01"></div>
</div>
<label for="wk">Mots-clés déclencheurs (séparés par des virgules)</label>
<textarea id="wk" style="min-height:70px" maxlength="800"></textarea>
<div class="row"><button class="pri" onclick="saveWeather()">Enregistrer</button></div>
<div class="msg" id="msgW"></div>
</div>

<div class="box">
<h2>Actualités</h2>
<p class="sub">Flux RSS interrogés quand la question contient un mot-clé ; les titres sont fusionnés, dédoublonnés puis résumés à l'oral par le LLM.</p>
<label for="nf">Flux RSS (une URL par ligne)</label>
<textarea id="nf" style="min-height:60px" maxlength="1200"></textarea>
<div class="grid">
  <div><label for="nc">Titres max</label>
       <input id="nc" type="number" min="1" max="15" step="1"></div>
  <div><label for="nmt">Longueur MAX (tokens)</label>
       <input id="nmt" type="number" min="20" max="1000" step="10"></div>
</div>
<label for="nk">Mots-clés déclencheurs (séparés par des virgules)</label>
<textarea id="nk" style="min-height:70px" maxlength="800"></textarea>
<div class="row"><button class="pri" onclick="saveNews()">Enregistrer</button></div>
<div class="msg" id="msgN"></div>
</div>

</div>
</div>

<script>
// ---- HELPERS COMMUNS ----
let DEFAULTS = {};
function say(id,m,ok){const e=document.getElementById(id);e.textContent=m;e.className='msg '+(ok?'ok':'ko');
  if(ok&&m)setTimeout(()=>{e.textContent='';},4000);}

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
function esc(s){return String(s).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));}
async function loadConv(){
  try{
    const d=await (await fetch('/conversation')).json();
    document.getElementById('histN').textContent=Math.floor(d.history.length/2);
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

// ---- PANNEAU MÉTÉO ----
function saveWeather(){
  postConfig({weather_city:document.getElementById('wc').value,
              weather_lat:document.getElementById('wla').value,
              weather_lon:document.getElementById('wlo').value,
              weather_keywords:document.getElementById('wk').value},'msgW','Météo enregistrée.');
}

// ---- PANNEAU ACTUALITÉS ----
function saveNews(){
  postConfig({news_feeds:document.getElementById('nf').value,
              news_count:document.getElementById('nc').value,
              news_max_tokens:document.getElementById('nmt').value,
              news_keywords:document.getElementById('nk').value},'msgN','Actualités enregistrées.');
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
  document.getElementById('histN').textContent = cfg.info.history_len;
  document.getElementById('wc').value  = cfg.settings.weather_city;
  document.getElementById('wla').value = cfg.settings.weather_lat;
  document.getElementById('wlo').value = cfg.settings.weather_lon;
  document.getElementById('wk').value  = cfg.settings.weather_keywords.join(', ');
  document.getElementById('nf').value  = cfg.settings.news_feeds.join('\\n');
  document.getElementById('nc').value  = cfg.settings.news_count;
  document.getElementById('nmt').value = cfg.settings.news_max_tokens;
  document.getElementById('nk').value  = cfg.settings.news_keywords.join(', ');

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

    if "weather_keywords" in data:
        raw = data["weather_keywords"]
        # La page envoie une chaîne "a, b, c" ; une liste JSON est acceptée aussi.
        # Normalisés en minuscules : la détection compare à la question en .lower().
        if isinstance(raw, str):
            kws = [k.strip().lower() for k in raw.split(",")]
        elif isinstance(raw, list):
            kws = [str(k).strip().lower() for k in raw]
        else:
            kws = []
        kws = [k for k in kws if k]
        if 1 <= len(kws) <= 30 and all(len(k) <= 40 for k in kws):
            _settings["weather_keywords"] = kws
        else:
            errors.append("mots-clés météo : 1 à 30 entrées de 40 caractères max")

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

    if "news_max_tokens" in data:
        try:
            n = int(data["news_max_tokens"])
            if not (20 <= n <= 1000):
                raise ValueError
            _settings["news_max_tokens"] = n
        except (TypeError, ValueError):
            errors.append("longueur réponse actu : 20 à 1000")

    if "news_keywords" in data:
        raw = data["news_keywords"]
        if isinstance(raw, str):
            kws = [k.strip().lower() for k in raw.split(",")]
        elif isinstance(raw, list):
            kws = [str(k).strip().lower() for k in raw]
        else:
            kws = []
        kws = [k for k in kws if k]
        if 1 <= len(kws) <= 30 and all(len(k) <= 40 for k in kws):
            _settings["news_keywords"] = kws
        else:
            errors.append("mots-clés actu : 1 à 30 entrées de 40 caractères max")

    if errors:
        return {"errors": errors}, 400

    _settings_save()
    print(f"[Bridge] Paramètres mis à jour : {', '.join(sorted(data.keys()))}")
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
    """Historique conversationnel courant — pour le bouton « Afficher » de la page
    config. Même vue que celle donnée au LLM (purge TTL appliquée au passage)."""
    return {"history": _conversation_history()}


def _answer_and_speak(text: str):
    """Partie commune à /ask et /ask_text, une fois le texte de la
    question connu : météo/actu -> LLM -> TTS -> réponse HTTP + MQTT."""
    news_context = maybe_fetch_news(text)
    contexts = [c for c in (maybe_fetch_weather(text), news_context) if c]
    # Réponse actu plus longue (5-6 titres) ; sinon budget court habituel.
    max_tokens = _settings["news_max_tokens"] if news_context else _settings["max_tokens"]
    answer = llm_answer(text, "\n\n".join(contexts) or None, max_tokens)
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
