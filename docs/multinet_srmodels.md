# srmodels.bin — construction & MultiNet

Comment est fabriqué le `srmodels.bin` d'ESP-SR (WakeNet / MultiNet / VAD), comment en régénérer un,
et pourquoi MultiNet **n'est pas retenu** sur ce projet.

---

## Ce qu'est un `srmodels.bin`

Un conteneur packé : une table d'en-tête suivie des blobs concaténés. Le packer parcourt un dossier,
et **chaque sous-dossier = un modèle**. Format produit :

```
[u32]  nombre de modèles
pour chaque modèle :
    [char[32]] nom du modèle    (= nom du sous-dossier)
    [u32]      nombre de fichiers
    pour chaque fichier :
        [char[32]] nom du fichier
        [u32]      offset (dans la zone data)
        [u32]      longueur
--- zone data : contenu binaire de tous les fichiers, bout à bout ---
```

Longueur de l'en-tête : `4 + (n_modèles × 36) + (n_fichiers × 40)` octets.

Notre `esp32/model/srmodels_jarvis.bin` actuel contient **2 modèles empilés** : `wn9_jarvis_ts`
(wake word) + `vadnet1_medium` (VAD). **Aucun MultiNet.**

---

## Régénérer le bin — méthode directe (`pack_model.py`)

L'outil ne vit ni sur la machine ni dans pioarduino ; il faut le dépôt esp-sr :

```bash
git clone --depth 1 https://github.com/espressif/esp-sr.git
```

Les dossiers-modèles bruts sont sous `esp-sr/model/` (`wakenet_model/`, `multinet_model/`,
`nsnet_model/`…). On monte un dossier de staging avec **exactement les sous-dossiers voulus**, puis on
packe :

```bash
mkdir -p mymodels
cp -R esp-sr/model/wakenet_model/wn9_jarvis_ts   mymodels/
cp -R esp-sr/model/nsnet_model/vadnet1_medium    mymodels/
# + mn7_en si l'on voulait MultiNet (cf. plus bas — non retenu)
python3 esp-sr/model/pack_model.py -m mymodels -o srmodels.bin
```

`pack_model.py` n'a que deux arguments : `-m` (dossier de modèles) et `-o` (sortie, défaut
`srmodels.bin`). **Pas de fichier de config** : il empile tout sous-dossier trouvé. Le **nom packé =
le nom du sous-dossier** (renommer si besoin).

⚠️ Les chemins exacts sous `esp-sr/model/` varient selon la version du dépôt (modèles parfois en
git-lfs ou téléchargés à part) — seul point à vérifier au moment de le faire.

⚠️ **Le modèle doit être packé par une version d'esp-sr compatible avec le runtime** (core Arduino
3.x / libs IDF 5.5.x). Un bin jarvis généré sous esp-sr 1.9.2 (2024) fait crasher le runtime récent
(`LoadProhibited`, le nom du modèle se lit puis ça plante).

---

## Régénérer le bin — via menuconfig (ESP-IDF)

Le flux officiel assemble le dossier tout seul : dans un projet ESP-IDF requérant le composant
`espressif/esp-sr`, `idf.py menuconfig → ESP Speech Recognition`, cocher les modèles voulus, puis :

```
python {esp-sr}/movemodel.py -d1 {sdkconfig} -d2 {esp-sr} -d3 {build}
```

`movemodel.py` lit le `sdkconfig`, copie les dossiers sélectionnés et appelle `pack_model.py`.
Résultat dans `build/srmodels/srmodels.bin`. C'est ce flux qui a produit le `sdkconfig` figé du core
pioarduino (`CONFIG_SR_WN_WN9_HIESP=y`, `CONFIG_SR_MN_EN_MULTINET7_QUANT=y`,
`CONFIG_SR_VADN_VADNET1_MEDIUM=y`).

---

## Flasher le bin

La partition `model` est à **`0x810000`** — offset **explicite** dans `partitions.csv`, à relire là-bas
plutôt qu'à recalculer. Indépendant du firmware, par câble USB-C :

```bash
esptool.py --chip esp32s3 --port /dev/tty.usbmodemXXX --baud 921600 write_flash 0x810000 srmodels.bin
```

Un `pio run -t upload` par OTA ne touche **pas** cette partition ; par USB (`esptool`) si, via
`extra_scripts/flash_assets.py`, qui lit l'offset dans `partitions.csv` et ajoute le modèle et l'image
LittleFS aux images flashées.

---

## MultiNet — pourquoi ce n'est pas retenu

MultiNet = reconnaissance de **commandes fixes hors-ligne**. Pour l'activer il faudrait : (1) un
`srmodels.bin` contenant un modèle MN (`mn7_en`), (2) passer une liste `sr_cmd_t` à `ESP_SR.begin()`
(aujourd'hui `NULL, 0`), (3) gérer `SR_EVENT_COMMAND` + le basculement `WAKEWORD → COMMAND` dans le
callback. La RAM **n'est plus** le blocage (~120 Ko d'interne libres avant ESP-SR).

Le vrai mur est ailleurs : **MultiNet n'existe qu'en anglais et chinois**, et le wrapper Arduino
`esp32-hal-sr.c` est **câblé en dur sur l'anglais** :

```c
char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_ENGLISH);  // "en"
char *phonemes = flite_g2p(sr_commands[i].str, 1);   // Flite = g2p anglais
```

Les commandes seraient donc à écrire **en anglais**, phonétisées à l'anglaise — bancal pour un
dashboard piloté en français, et fragile vu la limite micro connue (SNR ~1 dB au-dessus de 3 kHz, les
consonnes sont noyées).

**Décision (2026-07-29) : MultiNet abandonné.** L'architecture retenue reste : WakeNet « Jarvis »
**hors-ligne** → Whisper / LLM / TTS **en français** via le bridge NAS. MultiNet n'apporterait que des
ordres fixes, hors-ligne et en anglais — rien que le bridge ne fasse déjà mieux en français. Le flag
`-DCONFIG_SR_MN_EN_NONE=1` (`platformio.ini`) reste posé : il exclut tout le chargement MultiNet de
`esp32-hal-sr.c`.
