#!/usr/bin/env python3
"""
png_to_bin.py — Convertit un ou plusieurs PNG en blobs binaires bruts
RGB565A8, prêts à être copiés dans data/<dossier>/ et uploadés en
LittleFS via `pio run -t uploadfs`.

Format produit (PAS de header LVGL) :
    [plan couleur RGB565, w*h*2 octets, little-endian]
  + [plan alpha A8,        w*h*1 octet ]   (--format rgb565a8, défaut)
Layout confirmé par la doc LVGL v9 pour LV_COLOR_FORMAT_RGB565A8.
`--format rgb565` s'arrête au plan couleur, pour une image opaque.
Le lv_image_dsc_t correspondant est reconstruit à la main côté
firmware (voir ai_companion.cpp) — aucun risque de désalignement
avec un format de header qui changerait d'une version LVGL à l'autre.

USAGE
-----
Un seul fichier :
    python3 png_to_bin.py mon_sprite.png
    -> génère mon_sprite.bin dans le même dossier

Un dossier entier (tous les .png dedans) :
    python3 png_to_bin.py chemin/vers/dossier_pngs/ -o chemin/vers/dossier_bins/

Taille différente de 120x120 (ex: une icône 64x64) :
    python3 png_to_bin.py mon_icone.png --size 64

PRÉREQUIS
---------
- PNG en mode RGBA (transparence gérée nativement — si ton PNG n'a
  pas de canal alpha, il est traité comme opaque, alpha=255 partout)
- Dimensions exactes attendues (120x120 par défaut) — le script
  refuse un PNG qui ne correspond pas, plutôt que de le redimensionner
  silencieusement (une mauvaise surprise moins visible qu'une erreur
  claire à la conversion)

    pip3 install pillow --break-system-packages
"""

# ----------------------------------------------------------------
#  BIBLIOTHÈQUES
# ----------------------------------------------------------------
import argparse
import struct
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow manquant — installe avec : pip3 install pillow --break-system-packages")

# ----------------------------------------------------------------
# HELPERS
# ----------------------------------------------------------------
def rgb888_to_rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

# ----------------------------------------------------------------
# API LOCALES
# ----------------------------------------------------------------
def convert_one(png_path: Path, out_path: Path, size: int, fmt: str):
    img = Image.open(png_path).convert("RGBA")
    if img.size != (size, size):
        raise ValueError(
            f"{png_path.name} : taille {img.size}, attendu ({size}, {size}). "
            f"Redimensionne la source ou passe --size {img.size[0]} si c'est voulu."
        )

    color_plane = bytearray()
    alpha_plane = bytearray()

    px = img.load()
    for y in range(size):
        for x in range(size):
            r, g, b, a = px[x, y]
            color_plane += struct.pack("<H", rgb888_to_rgb565(r, g, b))
            alpha_plane.append(a)

    blob = bytes(color_plane)
    if fmt == "rgb565a8":
        blob += bytes(alpha_plane)
    out_path.write_bytes(blob)
    return len(blob)

# ----------------------------------------------------------------
# API PUBLIQUES
# ----------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="PNG -> .bin RGB565A8 (LVGL v9, chargement LittleFS)")
    ap.add_argument("input", type=Path, help="Fichier .png ou dossier contenant des .png")
    ap.add_argument("-o", "--output", type=Path, default=None,
                     help="Dossier de sortie (défaut : même dossier que l'entrée)")
    ap.add_argument("--size", type=int, default=120,
                     help="Largeur/hauteur attendue en pixels (défaut : 120)")
    ap.add_argument("--format", choices=("rgb565a8", "rgb565"), default="rgb565a8",
                     help="rgb565 omet le plan alpha (2 o/px au lieu de 3) — "
                          "réservé aux images opaques")
    args = ap.parse_args()

    if args.input.is_dir():
        pngs = sorted(args.input.glob("*.png"))
        if not pngs:
            sys.exit(f"Aucun .png trouvé dans {args.input}")
        out_dir = args.output or args.input
        out_dir.mkdir(parents=True, exist_ok=True)
    else:
        pngs = [args.input]
        out_dir = args.output or args.input.parent
        out_dir.mkdir(parents=True, exist_ok=True)

    total = 0
    errors = 0
    for png in pngs:
        out_path = out_dir / (png.stem + ".bin")
        try:
            n = convert_one(png, out_path, args.size, args.format)
            total += n
            print(f"  {png.name:30s} -> {out_path.name:30s} ({n} octets)")
        except Exception as e:
            errors += 1
            print(f"  ERREUR {png.name} : {e}", file=sys.stderr)

    print(f"\n{len(pngs) - errors}/{len(pngs)} fichier(s) converti(s), "
          f"{total} octets ({total / 1024:.1f} Ko) au total dans {out_dir}")
    
    if errors:
        sys.exit(1)

if __name__ == "__main__":
    main()
