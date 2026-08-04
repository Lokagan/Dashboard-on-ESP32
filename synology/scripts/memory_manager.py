#!/usr/bin/env python3
"""
memory_manager.py — Mémoire persistante de l'assistant vocal (SQLite)
Complète l'historique conversationnel de bridge_monitor.py, qui est court et
purgé après inactivité : ici on ne garde que des FAITS durables, un par clé
canonique, écrasés à chaque mise à jour.

  clé canonique  ->  valeur         ex. "animal.chat.nom" -> "Mochi"

Deux usages côté bridge :
  1. injection — memory_profile_block() rend un bloc texte à coller dans le
     prompt système (aucun appel LLM, aucune latence ajoutée) ;
  2. recherche — memory_search() sert un outil function-calling quand la base
     dépasse ce qu'on peut injecter.

⚠️ Aucune dépendance hors stdlib : FTS5 est fourni par le sqlite3 de Python.
⚠️ La base vit sous ./scripts (bind-mount NAS) — elle survit à un rebuild de
   l'image, et n'est PAS versionnée (.gitignore).

Ce module ne journalise pas les conversations : un journal d'épisodes noie la
recherche pour une valeur quasi nulle. À ajouter dans une table séparée le jour
où le besoin est prouvé, jamais dans `facts`.
"""

# ----------------------------------------------------------------
# BIBLIOTHÈQUES
# ----------------------------------------------------------------
import os
import re
import time
import sqlite3
import threading

# ----------------------------------------------------------------
# OBJETS GLOBAUX
# ----------------------------------------------------------------
DB_FILE = os.getenv("MEMORY_DB_FILE", "/app/scripts/memory.db")

# Plafond dur : au-delà, éviction du fait le moins récemment SERVI. Sans ça la
# base grossit indéfiniment et le bloc injecté finit par noyer le prompt.
MAX_FACTS = int(os.getenv("MEMORY_MAX_FACTS", "200"))

# Nombre de faits injectés dans le prompt système à chaque requête.
PROFILE_LIMIT = int(os.getenv("MEMORY_PROFILE_LIMIT", "40"))

KEY_MAX_LEN   = 64
VALUE_MAX_LEN = 500
SCOPES = ("user", "maison", "preference")

# ⚠️ Flask tourne en threaded=True et monitor.py lance 4 threads : une connexion
# sqlite3 ne se partage pas entre threads, chacun ouvre la sienne.
_local = threading.local()
_ready = False      # base ouverte et schéma en place
_fts   = False      # FTS5 disponible ; sinon repli LIKE dans memory_search()

_SCHEMA = """
CREATE TABLE IF NOT EXISTS facts (
  key          TEXT PRIMARY KEY,
  value        TEXT NOT NULL,
  scope        TEXT NOT NULL DEFAULT 'user',
  source       TEXT,
  confidence   REAL NOT NULL DEFAULT 0.8,
  created_at   INTEGER NOT NULL,
  updated_at   INTEGER NOT NULL,
  expires_at   INTEGER,
  hits         INTEGER NOT NULL DEFAULT 0,
  last_used_at INTEGER
);
CREATE INDEX IF NOT EXISTS idx_facts_lru ON facts(last_used_at, updated_at);
"""

# Index plein texte adossé à `facts` (external content : pas de duplication des
# valeurs). remove_diacritics 2 — sans lui « préfère » ne répond pas à « prefere ».
_SCHEMA_FTS = """
CREATE VIRTUAL TABLE IF NOT EXISTS facts_fts USING fts5(
  key, value, content='facts', content_rowid='rowid',
  tokenize="unicode61 remove_diacritics 2"
);
CREATE TRIGGER IF NOT EXISTS facts_ai AFTER INSERT ON facts BEGIN
  INSERT INTO facts_fts(rowid, key, value) VALUES (new.rowid, new.key, new.value);
END;
CREATE TRIGGER IF NOT EXISTS facts_ad AFTER DELETE ON facts BEGIN
  INSERT INTO facts_fts(facts_fts, rowid, key, value) VALUES('delete', old.rowid, old.key, old.value);
END;
CREATE TRIGGER IF NOT EXISTS facts_au AFTER UPDATE ON facts BEGIN
  INSERT INTO facts_fts(facts_fts, rowid, key, value) VALUES('delete', old.rowid, old.key, old.value);
  INSERT INTO facts_fts(rowid, key, value) VALUES (new.rowid, new.key, new.value);
END;
"""

# ----------------------------------------------------------------
# API LOCALES
# ----------------------------------------------------------------
def _now() -> int:
    return int(time.time())

def _conn() -> sqlite3.Connection:
    c = getattr(_local, "conn", None)
    if c is None:
        c = sqlite3.connect(DB_FILE, timeout=5.0)
        c.row_factory = sqlite3.Row
        c.execute("PRAGMA journal_mode=WAL")
        c.execute("PRAGMA synchronous=NORMAL")
        _local.conn = c
    return c

def _norm_key(key: str) -> str:
    """Clé canonique : c'est elle qui fait l'UPSERT, donc l'écrasement d'un
    fait périmé plutôt que son empilement à côté."""
    k = (key or "").strip().lower()
    k = re.sub(r"[^a-z0-9._]+", "_", k)
    k = re.sub(r"[._]{2,}", ".", k).strip("._")
    return k[:KEY_MAX_LEN]

def _row(r: sqlite3.Row) -> dict:
    return dict(r)

def _evict(conn: sqlite3.Connection):
    n = conn.execute("SELECT COUNT(*) FROM facts").fetchone()[0]
    if n <= MAX_FACTS:
        return
    conn.execute(
        "DELETE FROM facts WHERE key IN ("
        "  SELECT key FROM facts"
        "  ORDER BY COALESCE(last_used_at, updated_at) ASC, hits ASC LIMIT ?)",
        (n - MAX_FACTS,))

# Requête FTS5 depuis du texte libre : chaque mot devient un préfixe cité. Sans
# citation, une apostrophe ou un « - » est lu comme un opérateur et lève.
def _fts_query(text: str) -> str:
    mots = [m for m in re.split(r"\W+", (text or "").lower()) if len(m) > 1]
    return " OR ".join(f'"{m}"*' for m in mots[:8])

# ----------------------------------------------------------------
# API PUBLIQUES
# ----------------------------------------------------------------
def memory_init(path: str | None = None) -> bool:
    """À appeler depuis main(), PAS à l'import. Un échec laisse le module inerte
    (toutes les API rendent vide) sans jamais lever."""
    global DB_FILE, _ready, _fts
    if path:
        DB_FILE = path
    try:
        os.makedirs(os.path.dirname(DB_FILE) or ".", exist_ok=True)
        conn = _conn()
        conn.executescript(_SCHEMA)
        try:
            conn.executescript(_SCHEMA_FTS)
            _fts = True
        except sqlite3.OperationalError as e:
            print(f"[Memoire] FTS5 absent ({e}) — recherche en repli LIKE")
        conn.commit()
        _ready = True
        n = conn.execute("SELECT COUNT(*) FROM facts").fetchone()[0]
        print(f"[Memoire] {DB_FILE} — {n} fait(s), plafond {MAX_FACTS}")
    except Exception as e:
        print(f"[Memoire] FATAL {DB_FILE} : {e} — mémoire désactivée")
        _ready = False
    return _ready

def memory_set_limits(max_facts: int | None = None, profile_limit: int | None = None):
    """Bornes pilotées par les paramètres IA du bridge (page de config)."""
    global MAX_FACTS, PROFILE_LIMIT
    if max_facts:
        MAX_FACTS = max(10, min(2000, int(max_facts)))
    if profile_limit:
        PROFILE_LIMIT = max(1, min(200, int(profile_limit)))
    if _ready:
        conn = _conn()
        _evict(conn)
        conn.commit()

def memory_clear() -> int:
    if not _ready:
        return 0
    conn = _conn()
    n = conn.execute("SELECT COUNT(*) FROM facts").fetchone()[0]
    conn.execute("DELETE FROM facts")
    conn.commit()
    print(f"[Memoire] Vidée ({n} fait(s))")
    return n

def memory_purge_expired() -> int:
    if not _ready:
        return 0
    conn = _conn()
    cur = conn.execute("DELETE FROM facts WHERE expires_at IS NOT NULL AND expires_at <= ?", (_now(),))
    conn.commit()
    return cur.rowcount or 0

def memory_remember(key: str, value: str, scope: str = "user", source: str = "voice",
                    confidence: float = 0.8, ttl_s: int | None = None) -> dict | None:
    """UPSERT sur la clé canonique. ttl_s borne un fait circonstanciel
    (« est enrhumé ») ; None = permanent."""
    if not _ready:
        return None
    k = _norm_key(key)
    v = (value or "").strip()[:VALUE_MAX_LEN]
    if not k or not v:
        return None
    if scope not in SCOPES:
        scope = "user"
    now = _now()
    # ⚠️ ttl_s vient parfois d'un tool-call LLM : tout ce qui n'est pas un entier
    # exploitable donne un fait permanent, jamais une exception.
    try:
        exp = now + int(ttl_s) if ttl_s else None
    except (TypeError, ValueError):
        exp = None
    conn = _conn()
    conn.execute(
        "INSERT INTO facts (key, value, scope, source, confidence, created_at, updated_at, expires_at)"
        " VALUES (?,?,?,?,?,?,?,?)"
        " ON CONFLICT(key) DO UPDATE SET"
        "   value=excluded.value, scope=excluded.scope, source=excluded.source,"
        "   confidence=excluded.confidence, updated_at=excluded.updated_at,"
        "   expires_at=excluded.expires_at",
        (k, v, scope, source, float(confidence), now, now, exp))
    _evict(conn)
    conn.commit()
    print(f"[Memoire] {k} = {v[:60]}" + (f" (ttl {ttl_s}s)" if ttl_s else ""))
    return memory_get(k)

def memory_forget(key: str) -> bool:
    if not _ready:
        return False
    conn = _conn()
    cur = conn.execute("DELETE FROM facts WHERE key = ?", (_norm_key(key),))
    conn.commit()
    return bool(cur.rowcount)

def memory_get(key: str) -> dict | None:
    if not _ready:
        return None
    r = _conn().execute("SELECT * FROM facts WHERE key = ?", (_norm_key(key),)).fetchone()
    return _row(r) if r else None

def memory_all(limit: int = 500) -> list[dict]:
    """Liste complète pour l'onglet Mémoire de la page de config — c'est le seul
    moyen de retirer un fait mémorisé à tort (transcription hallucinée)."""
    if not _ready:
        return []
    memory_purge_expired()
    rows = _conn().execute(
        "SELECT * FROM facts ORDER BY scope, key LIMIT ?", (limit,)).fetchall()
    return [_row(r) for r in rows]

def memory_search(query: str, limit: int = 5) -> list[dict]:
    if not _ready:
        return []
    memory_purge_expired()
    conn = _conn()
    if _fts:
        q = _fts_query(query)
        if not q:
            return []
        rows = conn.execute(
            "SELECT f.* FROM facts_fts JOIN facts f ON f.rowid = facts_fts.rowid"
            " WHERE facts_fts MATCH ? ORDER BY bm25(facts_fts) LIMIT ?", (q, limit)).fetchall()
    else:
        like = f"%{(query or '').strip()}%"
        rows = conn.execute(
            "SELECT * FROM facts WHERE key LIKE ? OR value LIKE ? LIMIT ?",
            (like, like, limit)).fetchall()
    res = [_row(r) for r in rows]
    memory_touch([r["key"] for r in res])
    return res

def memory_touch(keys: list[str]):
    """Alimente le compteur d'usage : c'est lui qui décide de l'éviction."""
    if not _ready or not keys:
        return
    conn = _conn()
    conn.executemany("UPDATE facts SET hits = hits + 1, last_used_at = ? WHERE key = ?",
                     [(_now(), k) for k in keys])
    conn.commit()

def memory_profile_block(limit: int | None = None) -> str | None:
    """Bloc prêt à concaténer au prompt système. Rend None si la base est vide —
    l'appelant ne doit alors rien ajouter au prompt."""
    if not _ready:
        return None
    memory_purge_expired()
    n = limit or PROFILE_LIMIT
    rows = _conn().execute(
        "SELECT key, value, scope FROM facts"
        " ORDER BY confidence DESC, updated_at DESC LIMIT ?", (n,)).fetchall()
    if not rows:
        return None
    memory_touch([r["key"] for r in rows])
    lignes = "\n".join(f"- {r['key']} : {r['value']}" for r in rows)
    return ("Ce que tu sais déjà de ton interlocuteur (à utiliser naturellement "
            "quand c'est pertinent, sans jamais en réciter la liste) :\n" + lignes)

def memory_stats() -> dict:
    if not _ready:
        return {"ready": False, "count": 0, "max": MAX_FACTS, "fts": False}
    n = _conn().execute("SELECT COUNT(*) FROM facts").fetchone()[0]
    return {"ready": True, "count": n, "max": MAX_FACTS, "fts": _fts, "db": DB_FILE}
