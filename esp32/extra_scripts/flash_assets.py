# Flashe, EN MÊME TEMPS que le firmware, les deux images que PlatformIO n'écrit
# pas de lui-même :
#   - model/srmodels_jarvis.bin -> partition `model`    (wake word ESP-SR)
#   - $BUILD_DIR/littlefs.bin   -> partition `littlefs` (sprites Companion)
#
# PlatformIO n'écrit RIEN dans la partition `model`, et le système de fichiers
# demande une cible `uploadfs` séparée. Les deux images sont donc ajoutées à
# FLASH_EXTRA_IMAGES, la liste que le builder passe à esptool avec le bootloader
# et la table de partitions : une seule commande de flash.
#
# ⚠️ FLASH USB UNIQUEMENT : en OTA, FLASH_EXTRA_IMAGES est ignoré (espota n'écrit
# que la partition applicative). Le script se désactive alors entièrement.
#
# ⚠️ Chaque flash USB ÉCRASE le système de fichiers avec le contenu de data/.
#
# Source de vérité des offsets : la colonne Offset de partitions.csv. Aucun
# recalcul d'alignement ici, il dupliquerait les règles de gen_esp32part.py.

Import("env")
from SCons.Script import COMMAND_LINE_TARGETS
import os

# Cibles qui gèrent DÉJÀ le système de fichiers : ce script n'a rien à y faire.
# C'est aussi ce qui rend la récursion impossible — l'appel imbriqué plus bas
# passe justement par `buildfs`.
FS_TARGETS = {"buildfs", "uploadfs", "uploadfsota"}

CSV  = os.path.join(env.subst("$PROJECT_DIR"), "partitions.csv")
BLOB = os.path.join(env.subst("$PROJECT_DIR"), "model", "srmodels_jarvis.bin")


def partition_offset(label):
    """Offset (chaîne '0x...') de la partition `label`, None si introuvable ou
    déclarée en auto (colonne vide)."""
    if not os.path.isfile(CSV):
        return None
    with open(CSV, "r", encoding="utf-8") as f:
        for line in f:
            line = line.split("#")[0].strip()
            if not line:
                continue
            cols = [c.strip() for c in line.split(",")]
            if len(cols) >= 4 and cols[0] == label:
                return cols[3] or None
    return None


if FS_TARGETS & set(COMMAND_LINE_TARGETS):
    pass   # cible dédiée au système de fichiers, ou sous-build lancé d'ici : rien à faire,
           # et surtout rien à afficher (ce sous-build ne flashe pas).
elif env.subst("$UPLOAD_PROTOCOL") != "esptool":
    print("[assets] upload OTA — modèle et système de fichiers non embarqués "
          "(normal : l'OTA n'écrit que la partition applicative)")
else:
    # --- Modèle ESP-SR ---
    offset = partition_offset("model")
    if not os.path.isfile(BLOB):
        print("[assets] model/srmodels_jarvis.bin introuvable — modèle NON "
              "flashé, le wake word ne démarrera pas")
    elif offset is None:
        print("[assets] partition `model` sans offset explicite dans "
              "partitions.csv — modèle NON flashé")
    else:
        env.Append(FLASH_EXTRA_IMAGES=[(offset, BLOB)])
        print("[assets] %s -> %s (%d o)"
              % (os.path.basename(BLOB), offset, os.path.getsize(BLOB)))

    # --- Système de fichiers LittleFS ---
    # ⚠️ Nom de l'image pris sur board_build.filesystem et NON sur
    # $ESP32_FS_IMAGE_NAME : cette variable est posée par le builder de la
    # plateforme, qui tourne APRÈS les scripts `pre:` — elle serait vide ici.
    fs_offset = partition_offset("littlefs")
    fs_name   = env.BoardConfig().get("build.filesystem", "spiffs")
    fs_image  = os.path.join(env.subst("$BUILD_DIR"), fs_name + ".bin")
    data_dir  = env.subst("$PROJECT_DATA_DIR")

    if fs_offset is None:
        print("[assets] partition `littlefs` sans offset explicite dans "
              "partitions.csv — système de fichiers NON flashé")
    elif not os.path.isdir(data_dir):
        print("[assets] data/ introuvable — système de fichiers NON flashé")
    else:
        # L'image n'est produite que par buildfs/uploadfs : sur un `upload`
        # normal elle n'existe pas, d'où l'appel imbriqué à `buildfs`.
        #
        # ⚠️ Accroché à firmware.bin, PAS à `upload` : firmware.factory.bin est
        # généré en post-action de firmware.bin en fusionnant FLASH_EXTRA_IMAGES,
        # donc une pre-action sur `upload` arriverait trop tard.
        env.AddPreAction(
            "$BUILD_DIR/${PROGNAME}.bin",
            env.VerboseAction(
                '"$PYTHONEXE" -m platformio run -e $PIOENV -t buildfs',
                "[assets] construction de l'image LittleFS depuis data/"),
        )
        env.Append(FLASH_EXTRA_IMAGES=[(fs_offset, fs_image)])
        print("[assets] %s -> %s (depuis %s)"
              % (os.path.basename(fs_image), fs_offset, data_dir))
