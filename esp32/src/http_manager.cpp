// ============================================================
// HTTP_MANAGER.CPP — serveur de fichiers LittleFS + commandes, sur une
// tâche FreeRTOS dédiée (cœur 0) : WebServer est synchrone, un
// téléchargement bloquerait loop() pendant tout le transfert.
//
// ⚠️ Les commandes reçues (POST /cmd, /ai_ask) peuvent toucher LVGL, ce
// qui est interdit hors loopTask. Les handlers ne posent donc qu'un
// drapeau ; l'exécution a lieu dans http_loop(), appelée depuis loop().
// /say fait exception : ai_say() ne fait qu'empiler dans une file.
// ============================================================

// ---- BIBLIOTHÈQUES ----
#include <ArduinoJson.h>
#include <WebServer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ---- RESSOURCES LOCALES ----
#include "http_manager.h"
#include "config.h"

// ---- OBJETS GLOBAUX ----
static WebServer server(HTTP_SERVER_PORT);

// Commande et question en attente d'exécution sur loopTask (cf. http_loop)
static volatile bool _cmd_pending = false;
static char          _pending_cmd[32] = "";
static volatile bool _ai_ask_pending = false;
static char          _pending_ai_question[200] = "";

// --- INTERFACE HTML/JS ---
// Les valeurs initiales des sliders sont INJECTÉES depuis config.h : codées en
// dur, elles avaient fini par diverger de la réalité du firmware.
const char HTML_CONTENT[] PROGMEM = R"=====(
<!DOCTYPE html>
<html lang="fr">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32 Dashboard Manager</title>
    <style>
        /* ---- BASE / LAYOUT ---- */
        body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background-color: #121212; color: #e0e0e0; margin: 0; padding: 20px; display: flex; flex-direction: column; align-items: center; }
        .layout { display: flex; gap: 20px; width: 100%; max-width: 1300px; flex-wrap: wrap; }
        .left-col { flex: 2; min-width: 320px; display: flex; flex-direction: column; gap: 20px; }
        .container, .cmd-panel, .log-panel, .ai-panel {
            background: #1e1e1e; padding: 20px; border-radius: 12px;
            box-shadow: 0 4px 20px rgba(0,0,0,0.5);
        }
        .cmd-panel h2, .log-panel h2, .ai-panel h2, .container h2 { color: #03dac6; font-size: 18px; margin: 0; }

        /* ---- PANNEAUX ASSISTANT IA / JARVIS ---- */
        .ai-panel { display: flex; flex-direction: column; align-items: center; gap: 12px; }
        .ai-row { display: flex; align-items: center; gap: 12px; width: 100%; max-width: 600px; }
        .ai-light { width: 18px; height: 18px; border-radius: 50%; background: #666; flex-shrink: 0; transition: background-color 0.3s; }
        .ai-light.idle      { background: #4caf50; }
        .ai-light.listening { background: #ffb300; }
        .ai-light.thinking  { background: #9c27b0; }
        .ai-light.speaking  { background: #2196f3; }
        .ai-light.error     { background: #f44336; }
        .ai-input { flex: 1; padding: 8px 10px; border-radius: 4px; border: none; background: #2a2a2a; color: #e0e0e0; font-size: 14px; }
        .ai-ask-btn { background: #3700B3; color: white; border: none; padding: 8px 16px; border-radius: 4px; cursor: pointer; }
        .ai-ask-btn:hover { background: #6200EE; }

        /* ---- PANNEAU LOGS ---- */
        .log-head { display: flex; align-items: center; margin-bottom: 12px; gap: 12px; }
        .log-head h2 { margin-right: auto; }
        .log-paused { color: #ffab40; font-size: 12px; visibility: hidden; }
        .copy-btn { background: #2a2a2a; color: #03dac6; border: 1px solid #03dac6; border-radius: 6px; padding: 5px 12px; font-size: 12px; cursor: pointer; white-space: nowrap; }
        .copy-btn:hover { background: #03dac6; color: #121212; }
        .copy-btn.ok { background: #00c853; border-color: #00c853; color: #121212; }
        .copy-btn.ko { background: #ff4081; border-color: #ff4081; color: white; }
        .log-box { background: #000; color: #0f0; font-family: 'Courier New', monospace; font-size: 12px; padding: 10px; border-radius: 6px; height: 260px; overflow-y: auto; white-space: pre-wrap; word-break: break-all; }

        /* ---- PANNEAU LITTLEFS ---- */
        .controls { display: flex; align-items: center; gap: 12px; margin-bottom: 12px; }
        .controls h2 { flex: 1; }
        .controls .status { flex: 1; text-align: right; margin: 0; }
        .status { font-style: italic; color: #888; margin-bottom: 10px; }
        .btn { display: inline-block; padding: 6px 12px; border-radius: 4px; font-size: 14px; text-decoration: none; transition: 0.3s; cursor: pointer; border: none; }
        .btn-back { background-color: #444; color: white; }
        table { width: 100%; border-collapse: collapse; }
        th, td { padding: 12px; text-align: left; border-bottom: 1px solid #333; }
        th { background-color: #252525; color: #03dac6; }
        tr:hover { background-color: #2a2a2a; }
        .btn-download { background-color: #3700B3; color: white; }
        .btn-download:hover { background-color: #6200EE; }
        .btn-delete { background-color: #cf6679; color: #121212; margin-left: 5px; }
        .btn-delete:hover { background-color: #ff4081; color: white; }

        /* ---- COLONNE COMMANDES ---- */
        .cmd-panel { flex: 1; min-width: 260px; }
        .cmd-panel h2 { margin-bottom: 16px; }
        .cmd-section { margin-bottom: 20px; }
        .cmd-section h3 { color: #03dac6; font-size: 13px; text-transform: uppercase; letter-spacing: 0.5px; margin-bottom: 8px; }
        .cmd-btn { display: block; width: 100%; margin-bottom: 8px; padding: 8px 10px; border-radius: 4px; border: none; background: #333; color: #e0e0e0; font-size: 14px; text-align: left; cursor: pointer; transition: 0.2s; }
        .cmd-btn:hover { background: #3700B3; color: white; }
        .cmd-row { display: flex; align-items: center; gap: 12px; margin-bottom: 10px; }
        .cmd-label { flex: none; width: 88px; color: #aaa; font-size: 13px; }
        .cmd-slider { width: 100%; }
        .cmd-row .cmd-slider { flex: 1; }
        a.cmd-btn { display: block; box-sizing: border-box; text-decoration: none; text-align: center; }
        .cmd-grid6 { display: grid; grid-template-columns: repeat(6, 1fr); gap: 4px; margin-bottom: 8px; }
        .cmd-grid6 .cmd-btn { width: auto; min-width: 0; margin-bottom: 0;
                              text-align: center; font-size: 12px; padding: 8px 0; }
        .cmd-pair { display: flex; gap: 8px; margin-bottom: 8px; }
        .cmd-pair .cmd-btn { flex: 1; width: auto; min-width: 0; margin-bottom: 0;
                             text-align: center; font-size: 13px; padding: 8px 4px;
                             white-space: nowrap; }
        .cmd-btn.btn-on { background: #00c853; color: #121212; }
        .cmd-btn.btn-on:hover { background: #00e676; color: #121212; }
        .cmd-btn.btn-reboot { background: #cf6679; color: #121212; }
        .cmd-btn.btn-reboot:hover { background: #ff4081; color: white; }
    </style>
</head>
<body>
    <div class="layout">
    <div class="left-col">
        <div class="ai-panel">
            <h2>Assistant IA</h2>
            <div class="ai-row">
                <div id="aiLight" class="ai-light idle"></div>
                <input id="aiQuestion" class="ai-input" type="text" placeholder="Pose ta question..."
                       onkeydown="if (event.key === 'Enter') postText('/ai_ask', 'aiQuestion')">
                <button class="ai-ask-btn" onclick="postText('/ai_ask', 'aiQuestion')">Demander</button>
            </div>
        </div>

        <div class="ai-panel">
            <h2>Faire parler Jarvis</h2>
            <div class="ai-row">
                <input id="sayText" class="ai-input" type="text" maxlength="180"
                       placeholder="Texte à prononcer (TTS seul)..."
                       onkeydown="if (event.key === 'Enter') postText('/say', 'sayText')">
                <button class="ai-ask-btn" onclick="postText('/say', 'sayText')">Dire</button>
            </div>
        </div>

        <div class="log-panel">
            <div class="log-head">
                <h2>Logs (Serial)</h2>
                <span id="logPaused" class="log-paused">⏸ figé pour copie (Effacer pour reprendre)</span>
                <button id="copyBtn" class="copy-btn" onclick="copyLogs()">Copier</button>
                <button id="clearBtn" class="copy-btn" onclick="clearLogs()">Effacer</button>
            </div>
            <div id="logBox" class="log-box">Chargement...</div>
        </div>

        <div class="container">
            <div class="controls">
                <h2>LittleFS</h2>
                <button class="btn btn-back" onclick="loadFiles('/')">⬅ Back</button>
                <div id="status" class="status">Loading...</div>
            </div>
            <table id="fileTable">
                <thead><tr><th>Name</th><th>Size</th><th>Action</th></tr></thead>
                <tbody id="fileList"></tbody>
            </table>
        </div>
    </div>

    <div class="cmd-panel">
        <div class="cmd-section">
            <h3>Navigation</h3>
            <button class="cmd-btn" onclick="sendCmd('page:home')">Accueil</button>
            <button class="cmd-btn" onclick="sendCmd('page:ai')">Assistant IA</button>
            <button class="cmd-btn" onclick="sendCmd('page:nas')">NAS</button>
            <button class="cmd-btn" onclick="sendCmd('page:disks')">NAS - Disques</button>
            <button class="cmd-btn" onclick="sendCmd('page:downloads')">NAS - Téléchargements</button>
            <button class="cmd-btn" onclick="sendCmd('page:connections')">NAS - Connexions</button>
            <button class="cmd-btn" onclick="sendCmd('page:freebox')">Freebox</button>
            <button class="cmd-btn" onclick="sendCmd('page:devices')">Freebox - Appareils</button>
            <button class="cmd-btn" onclick="sendCmd('page:activity')">Freebox - Activité Réseau</button>
        </div>

        <div class="cmd-section">
            <h3>Paramètres</h3>
            <div class="cmd-row">
                <span class="cmd-label">Luminosité</span>
                <input class="cmd-slider" type="range" min="1" max="100" value=")=====" CFG_STR(DISPLAY_BRIGHTNESS_DEFAULT) R"=====("
                       onchange="sendCmd('brightness:' + this.value)">
            </div>
            <div class="cmd-row">
                <span class="cmd-label">Volume</span>
                <input class="cmd-slider" type="range" min="0" max="100" value=")=====" CFG_STR(AUDIO_VOLUME_DEFAULT) R"=====("
                       onchange="sendCmd('volume:' + this.value)">
            </div>
            <a class="cmd-btn" target="_blank" rel="noopener"
               href=")=====" AI_BRIDGE_UI_URL R"=====(">⚙ Config IA côté NAS (voix, contexte, LLM)</a>
            <a class="cmd-btn" target="_blank" rel="noopener"
               href=")=====" ACTIVITY_UI_URL R"=====(">⚙ Config Services côté NAS</a>
            <a class="cmd-btn" target="_blank" rel="noopener"
               href=")=====" ACTIVITY_ADGUARD_URL R"=====(">⚙ Config ADGUARD côté NAS</a>
        </div>

        <div class="cmd-section">
            <h3>Diagnostic</h3>
            <div class="cmd-grid6">
                <button class="cmd-btn" onclick="sendCmd('page:sysinfo1')" title="SysInfo — Identité : modèle, silicium, CPU, charge par cœur, flash, température, dernier reset">ID</button>
                <button class="cmd-btn" onclick="sendCmd('page:sysinfo2')" title="SysInfo — Mémoire : heap interne/DMA/PSRAM, firmware, postes d'allocation connus">MEM</button>
                <button class="cmd-btn" onclick="sendCmd('page:sysinfo3')" title="SysInfo — Réseau : WiFi, IP, MQTT">NET</button>
                <button class="cmd-btn" onclick="sendCmd('page:sysinfo4')" title="SysInfo — Tâches : état, priorité, %CPU et pile de chaque tâche FreeRTOS">TSK</button>
                <button class="cmd-btn" onclick="sendCmd('page:sysinfo5')" title="SysInfo — Partitions : table de partitionnement de la flash">PRT</button>
                <button class="cmd-btn" onclick="sendCmd('page:sysinfo6')" title="SysInfo — Fichiers : occupation LittleFS">FS</button>
            </div>
            <div class="cmd-pair">
                <button class="cmd-btn" onclick="sendMem()">État mémoire</button>
                <button id="loopBtn" class="cmd-btn" onclick="toggleLoop()"
                        title="Journalise la charge de loopTask : une synthèse toutes les 10 s (%, tours, ms par manager) plus les dépassements de 20 ms. Coupé au démarrage — ça remplit vite les 40 lignes du journal.">Mesure boucle : —</button>
            </div>
            <div class="cmd-pair">
                <button class="cmd-btn" onclick="sendDiag('tree', 600)" title="Arbre des widgets de l'écran actif">Arbre LVGL</button>
                <button class="cmd-btn" onclick="sendDiag('spy', 1200)" title="Trace les 24 prochaines invalidations LVGL">Espion LVGL</button>
            </div>
            <div class="cmd-pair">
                <button class="cmd-btn" onclick="sendCmd('saverec')" title="Envoie la dernière capture IA au NAS (WAV horodaté dans scripts/captures/)">Capture IA → NAS</button>
                <button class="cmd-btn btn-reboot" onclick="sendReboot()">Redémarrer</button>
            </div>
        </div>
    </div>
    </div>

    <script>
        // ---- PANNEAUX ASSISTANT IA / JARVIS ----

        async function postText(path, inputId) {
            const input = document.getElementById(inputId);
            const text = input.value.trim();
            if (!text) return;
            try {
                const response = await fetch(`${path}?text=${encodeURIComponent(text)}`, { method: 'POST' });
                if (!response.ok) throw new Error('HTTP ' + response.status);
                input.value = '';
            } catch (error) {
                alert('Erreur : ' + error.message);
            }
        }

        async function refreshAiStatus() {
            try {
                const response = await fetch('/ai_status');
                if (!response.ok) return;
                const data = await response.json();
                document.getElementById('aiLight').className = 'ai-light ' + data.state;
                setLoopBtn(data.loop === true);
            } catch (error) {
            }
        }

        // ---- PANNEAU LOGS ----

        let refreshPausedUntil = 0;

        async function copyLogs() {
            const btn  = document.getElementById('copyBtn');
            const box  = document.getElementById('logBox');
            const text = box.textContent;
            let ok = false;

            if (window.isSecureContext && navigator.clipboard) {
                try { await navigator.clipboard.writeText(text); ok = true; } catch (e) {}
            }

            if (!ok) {
                const ta = document.createElement('textarea');
                ta.value = text;
                ta.style.position = 'fixed';
                ta.style.opacity  = '0';
                document.body.appendChild(ta);
                ta.select();
                try { ok = document.execCommand('copy'); } catch (e) {}
                document.body.removeChild(ta);
            }

            if (!ok) {
                const sel = window.getSelection();
                sel.removeAllRanges();
                const r = document.createRange();
                r.selectNodeContents(box);
                sel.addRange(r);
                refreshPausedUntil = Date.now() + 15000;
            }

            btn.textContent = ok ? 'Copié !' : 'Ctrl+C';
            btn.classList.add(ok ? 'ok' : 'ko');
            setTimeout(() => {
                btn.textContent = 'Copier';
                btn.classList.remove('ok', 'ko');
            }, 1500);
        }

        async function clearLogs() {
            const btn = document.getElementById('clearBtn');
            try {
                const response = await fetch('/serial/clear', { method: 'POST' });
                if (!response.ok) throw new Error('HTTP ' + response.status);
                refreshPausedUntil = 0;
                document.getElementById('logBox').textContent = '';
                btn.textContent = 'Effacé !';
                btn.classList.add('ok');
            } catch (error) {
                btn.textContent = 'Échec';
                btn.classList.add('ko');
            }
            setTimeout(() => {
                btn.textContent = 'Effacer';
                btn.classList.remove('ok', 'ko');
            }, 1500);
        }

        async function refreshLogs(force) {
            const box = document.getElementById('logBox');

            if (!force && Date.now() < refreshPausedUntil) {
                document.getElementById('logPaused').style.visibility = 'visible';
                return;
            }
            document.getElementById('logPaused').style.visibility = 'hidden';

            try {
                const response = await fetch('/serial?lines=40');
                if (!response.ok) return;
                const lines = await response.json();
                const wasAtBottom = box.scrollTop + box.clientHeight >= box.scrollHeight - 10;
                box.textContent = lines.join('\n');
                if (wasAtBottom) box.scrollTop = box.scrollHeight;
            } catch (error) {
            }
        }

        // ---- PANNEAU LITTLEFS ----

        let currentPath = '/';

        async function loadFiles(targetPath) {
            currentPath = targetPath;
            const statusElement = document.getElementById('status');
            const listElement = document.getElementById('fileList');
            const tableElement = document.getElementById('fileTable');

            statusElement.innerText = 'Scanning...';
            listElement.innerHTML = '';

            try {
                const response = await fetch(`/list?path=${encodeURIComponent(currentPath)}`);
                if (!response.ok) {
                    const err = await response.json();
                    throw new Error(err.error || 'Server error');
                }
                const files = await response.json();

                if (files.length === 0) {
                    statusElement.innerText = 'Directory is empty.';
                    tableElement.style.display = 'none';
                    return;
                }

                statusElement.innerText = 'Path: ' + currentPath;
                tableElement.style.display = 'table';

                files.forEach(file => {
                    const row = document.createElement('tr');
                    const isDir = file.is_dir;
                    const sizeStr = isDir ? '--' : (file.size / 1024).toFixed(2) + ' KB';

                    const actions = isDir
                        ? `<button class="btn btn-download" onclick="loadFiles('${file.path}')">Open Folder</button>`
                        : `<a href="/data?path=${encodeURIComponent(file.path)}" class="btn btn-download">Download</a>
                           <button class="btn btn-delete" onclick="deleteFile('${file.path}')">Delete</button>`;

                    row.innerHTML = `
                        <td>${isDir ? '📁' : '📄'} ${file.name}</td>
                        <td>${sizeStr}</td>
                        <td>${actions}</td>`;
                    listElement.appendChild(row);
                });
            } catch (error) {
                statusElement.innerText = 'Error: ' + error.message;
                console.error(error);
            }
        }

        async function deleteFile(filePath) {
            if (!confirm(`Are you sure you want to delete ${filePath}?`)) return;
            try {
                const response = await fetch(`/delete?path=${encodeURIComponent(filePath)}`, { method: 'POST' });
                if (response.ok) loadFiles(currentPath);
                else alert('Delete failed');
            } catch (error) {
                alert('Error deleting file');
            }
        }

        // ---- COLONNE COMMANDES ----

        async function sendCmd(cmd) {
            try {
                const response = await fetch(`/cmd?cmd=${encodeURIComponent(cmd)}`, { method: 'POST' });
                if (!response.ok) throw new Error('HTTP ' + response.status);
            } catch (error) {
                alert('Erreur commande : ' + error.message);
            }
        }

        async function sendMem() {
            await sendCmd('mem');
            setTimeout(refreshLogs, 300);
        }

        let loopMeasure = false;
        async function toggleLoop() {
            await sendCmd('loop:' + (loopMeasure ? 'off' : 'on'));
            setTimeout(refreshLogs, 300);
        }

        function setLoopBtn(on) {
            loopMeasure = on;
            const btn = document.getElementById('loopBtn');
            btn.textContent = 'Mesure boucle : ' + (on ? 'ON' : 'OFF');
            btn.className = 'cmd-btn' + (on ? ' btn-on' : '');
        }

        async function sendDiag(cmd, delayMs) {
            await sendCmd(cmd);
            setTimeout(async () => {
                await refreshLogs(true);
                refreshPausedUntil = Date.now() + 30000;
                document.getElementById('logPaused').style.visibility = 'visible';
            }, delayMs);
        }

        function sendReboot() {
            if (!confirm('Redémarrer l\'ESP32 ?')) return;
            sendCmd('reboot');
        }

        // ---- INITIALISATION ----

        setInterval(refreshLogs, 2000);
        setInterval(refreshAiStatus, 1500);

        window.onload = () => {
            loadFiles('/');
            refreshLogs();
            refreshAiStatus();
        };
    </script>
</body>
</html>
)=====";

// ---- HELPERS ----

// Rejette tout chemin contenant ".." (sortie de LittleFS)
static bool _path_is_safe(const String& p) {
    return p.indexOf("..") == -1;
}

static String _normalize_path(String p) {
    if (p.length() == 0) p = "/";
    if (p[0] != '/') p = "/" + p;
    return p;
}

static String _join_path(const String& dir, const String& name) {
    if (dir.endsWith("/")) return dir + name;
    return dir + "/" + name;
}

static String _basename(const String& path) {
    int slash = path.lastIndexOf('/');
    return (slash >= 0) ? path.substring(slash + 1) : path;
}

// ---- API LOCALES ----

// --- HANDLERS ---

static void _handle_list_files() {
    String path = _normalize_path(server.arg("path"));
    if (!_path_is_safe(path)) {
        server.send(400, "application/json", "{\"error\":\"chemin invalide\"}");
        return;
    }

    File root = littlefs_open(path, "r");
    if (!root || !root.isDirectory()) {
        server.send(404, "application/json", "{\"error\":\"repertoire introuvable\"}");
        return;
    }

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    File file = root.openNextFile();
    while (file) {
        // Selon la version du core, file.name() renvoie un nom relatif OU un
        // chemin complet : on ne garde que le dernier segment et on reconstruit
        // le chemin nous-mêmes, fiable en sous-dossier.
        String basename = _basename(String(file.name()));

        JsonObject o = arr.add<JsonObject>();
        o["name"]   = basename;
        o["path"]   = _join_path(path, basename);
        o["size"]   = file.size();
        o["is_dir"] = file.isDirectory();

        File next = root.openNextFile();
        file.close();
        file = next;
    }
    root.close();

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

static void _handle_file_request() {
    if (!server.hasArg("path")) {
        server.send(400, "text/plain", "Parametre 'path' manquant");
        return;
    }

    String filePath = _normalize_path(server.arg("path"));
    if (!_path_is_safe(filePath)) {
        server.send(400, "text/plain", "Chemin invalide");
        return;
    }
    if (!littlefs_exists(filePath)) {
        server.send(404, "text/plain", "Fichier introuvable");
        return;
    }

    File file = littlefs_open(filePath, "r");
    if (!file || file.isDirectory()) {
        server.send(404, "text/plain", "Fichier introuvable");
        return;
    }

    server.sendHeader("Content-Disposition", "attachment; filename=\"" + _basename(filePath) + "\"");
    server.streamFile(file, "application/octet-stream");
    file.close();
}

static void _handle_delete_request() {
    if (!server.hasArg("path")) {
        server.send(400, "text/plain", "Parametre 'path' manquant");
        return;
    }

    String filePath = _normalize_path(server.arg("path"));
    if (!_path_is_safe(filePath)) {
        server.send(400, "text/plain", "Chemin invalide");
        return;
    }
    if (!littlefs_exists(filePath)) {
        server.send(404, "text/plain", "Fichier introuvable");
        return;
    }

    if (littlefs_remove(filePath)) server.send(200, "text/plain", "Deleted");
    else                           server.send(500, "text/plain", "Delete failed");
}

// Dernières lignes du journal circulaire — ?lines=N (défaut 20).
static void _handle_serial_request() {
    int maxLines = 20;
    if (server.hasArg("lines")) {
        int v = server.arg("lines").toInt();
        if (v > 0) maxLines = v;
    }
    server.send(200, "application/json", log_get_json(maxLines));
}

// Ne pose qu'un drapeau — cf. en-tête de fichier.
static void _handle_cmd_request() {
    if (!server.hasArg("cmd")) {
        server.send(400, "text/plain", "Parametre 'cmd' manquant");
        return;
    }
    strncpy(_pending_cmd, server.arg("cmd").c_str(), sizeof(_pending_cmd) - 1);
    _pending_cmd[sizeof(_pending_cmd) - 1] = '\0';
    _cmd_pending = true;
    server.send(200, "text/plain", "OK");
}

// Idem : ai_on_ask_request() touche LVGL.
static void _handle_ai_ask_request() {
    if (!server.hasArg("text")) {
        server.send(400, "text/plain", "Parametre 'text' manquant");
        return;
    }
    strncpy(_pending_ai_question, server.arg("text").c_str(), sizeof(_pending_ai_question) - 1);
    _pending_ai_question[sizeof(_pending_ai_question) - 1] = '\0';
    _ai_ask_pending = true;
    server.send(200, "text/plain", "OK");
}

// Appel direct : log_clear() est protégé par la même section critique que
// log_line() et ne touche pas LVGL.
static void _handle_serial_clear_request() {
    log_clear();
    log_line("[LOG] Journal efface");   // repère : tout ce qui suit est postérieur
    server.send(200, "text/plain", "OK");
}

// PAS de drapeau différé, contrairement à /ai_ask : ai_say() ne fait que
// recopier le texte dans la file de la tâche IA, sans toucher LVGL.
static void _handle_say_request() {
    if (!server.hasArg("text")) {
        server.send(400, "text/plain", "Parametre 'text' manquant");
        return;
    }
    ai_say(server.arg("text").c_str());
    server.send(200, "text/plain", "OK");
}

static void _handle_ai_status_request() {
    const char* state_str = "idle";
    switch (ai_get_state()) {
        case AI_IDLE:      state_str = "idle";      break;
        case AI_LISTENING: state_str = "listening"; break;
        case AI_THINKING:  state_str = "thinking";  break;
        case AI_SPEAKING:  state_str = "speaking";  break;
        case AI_ERROR:     state_str = "error";     break;
    }
    // "loop" voyage avec l'état IA plutôt que sur sa propre route : le sondage
    // à 1,5 s existe déjà, et il resynchronise le bouton après un rechargement
    // de page ou un redémarrage de l'ESP32.
    char json[64];
    snprintf(json, sizeof(json), "{\"state\":\"%s\",\"loop\":%s}",
             state_str, log_loop_measure() ? "true" : "false");
    server.send(200, "application/json", json);
}

// Tâche dédiée, cœur 0 : handleClient() est bloquant pendant un streamFile.
static void _http_task(void* pvParameters) {
    for (;;) {
        server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

// ---- API PUBLIQUES ----

void http_init() {
    // send_P streame depuis la flash ; server.send() aurait construit un String
    // de ~9 Ko en heap à chaque chargement de la page.
    server.on("/", []() { server.send_P(200, "text/html", HTML_CONTENT); });

    server.on("/list",         _handle_list_files);
    server.on("/data",         HTTP_GET,  _handle_file_request);
    server.on("/delete",       HTTP_POST, _handle_delete_request);
    server.on("/cmd",          HTTP_POST, _handle_cmd_request);
    server.on("/serial",       HTTP_GET,  _handle_serial_request);
    server.on("/serial/clear", HTTP_POST, _handle_serial_clear_request);
    server.on("/ai_ask",       HTTP_POST, _handle_ai_ask_request);
    server.on("/say",          HTTP_POST, _handle_say_request);
    server.on("/ai_status",    HTTP_GET,  _handle_ai_status_request);
    server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });

    server.begin();
    log_line("[HTTP] Serveur démarré sur le port %d", HTTP_SERVER_PORT);

    // Pile : STACK_BYTES_HTTP_TASK. Les 6144 initiaux débordaient via
    // log_get_json() (~5880 o) ; sérialisé ligne par ligne désormais → pic 3080 o
    // (relevé APRÈS un téléchargement, le chemin le plus gourmand) → ~3 Ko marge.
    xTaskCreatePinnedToCore(_http_task, "http_task", STACK_BYTES_HTTP_TASK, nullptr, 1, nullptr, 0);
}

// Exécute ce qui est en attente — sur loopTask, jamais depuis _http_task.
void http_loop() {
    if (_cmd_pending) {
        _cmd_pending = false;
        mqtt_handle_esp_cmd(_pending_cmd);
    }
    if (_ai_ask_pending) {
        _ai_ask_pending = false;
        ai_on_ask_request(_pending_ai_question);
    }
}
