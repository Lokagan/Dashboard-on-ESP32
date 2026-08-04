#!/usr/bin/env python3
"""
bin_to_png.py — Convertit un ou plusieurs blobs .bin en PNG RGBA

Format attendu :
    [plan couleur RGB565, w*h*2 octets, little-endian]
  + [plan alpha A8,        w*h*1 octet ]   (--format rgb565a8, défaut)
`--format rgb565` s'arrête au plan couleur, l'image sort opaque.

Inverse exact du script png_to_bin.py — même option `--format`, et les frames
de data/companion/ sont en rgb565.

USAGE
-----
Un seul fichier :
    python3 bin_to_png.py mon_sprite.bin
    -> génère mon_sprite.png dans le même dossier

Un dossier entier :
    python3 bin_to_png.py chemin/vers/dossier_bins/ -o chemin/vers/dossier_pngs/

Taille différente de 120x120 :
    python3 bin_to_png.py mon_icone.bin --size 64

Frames de l'avatar (sans plan alpha) :
    python3 bin_to_png.py ../data/companion/ --format rgb565 -o /tmp/frames/

PRÉREQUIS
---------
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
    sys.exit(
        "Pillow manquant — installe avec : pip3 install pillow --break-system-packages"
    )


# ----------------------------------------------------------------
# HELPERS
# ----------------------------------------------------------------
def rgb565_to_rgb888(value):

    r5 = (value >> 11) & 0x1F
    g6 = (value >> 5) & 0x3F
    b5 = value & 0x1F

    # Extension 5/6 bits -> 8 bits
    r8 = (r5 << 3) | (r5 >> 2)
    g8 = (g6 << 2) | (g6 >> 4)
    b8 = (b5 << 3) | (b5 >> 2)

    return r8, g8, b8

# ----------------------------------------------------------------
# API LOCALES
# ----------------------------------------------------------------
def convert_one(bin_path: Path, out_path: Path, size: int, fmt: str):
    data = bin_path.read_bytes()
    pixel_count = size * size
    color_size = pixel_count * 2
    has_alpha = (fmt == "rgb565a8")
    expected_size = color_size + (pixel_count if has_alpha else 0)

    if len(data) != expected_size:
        raise ValueError(
            f"{bin_path.name} : taille {len(data)} octets, "
            f"attendu {expected_size} octets pour une image "
            f"{size}x{size} {fmt.upper()}."
        )

    color_plane = data[:color_size]
    alpha_plane = data[color_size:]

    img = Image.new("RGBA", (size, size))
    pixels = img.load()

    for y in range(size):
        for x in range(size):
            index = y * size + x
            offset = index * 2
            rgb565 = struct.unpack_from("<H", color_plane, offset)[0]
            r, g, b = rgb565_to_rgb888(rgb565)
            a = alpha_plane[index] if has_alpha else 255
            pixels[x, y] = (r, g, b, a)
    img.save(out_path, "PNG")
    return len(data)


# ----------------------------------------------------------------
# API PUBLIQUES
# ----------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="RGB565A8 / RGB565 .bin -> PNG RGBA")

    ap.add_argument("input", type=Path,
        help="Fichier .bin ou dossier contenant des .bin")
    ap.add_argument("-o", "--output", type=Path, default=None,
                    help="Dossier de sortie (défaut : même dossier que l'entrée)")
    ap.add_argument("--size", type=int, default=120,
        help="Largeur/hauteur de l'image (défaut : 120)")
    ap.add_argument("--format", choices=("rgb565a8", "rgb565"), default="rgb565a8",
                    help="rgb565 : pas de plan alpha (2 o/px), image opaque")
    args = ap.parse_args()

    if args.input.is_dir():
        bins = sorted(args.input.glob("*.bin"))
        if not bins:
            sys.exit(f"Aucun fichier .bin trouvé dans {args.input}")
        out_dir = args.output or args.input
    else:
        bins = [args.input]
        out_dir = args.output or args.input.parent

    out_dir.mkdir(parents=True, exist_ok=True)

    total = 0
    errors = 0

    for bin_file in bins:
        out_path = out_dir / (bin_file.stem + ".png")
        try:
            n = convert_one(bin_file, out_path, args.size, args.format)
            total += n

            print(f"  {bin_file.name:30s} -> "
                  f"{out_path.name:30s} "
                  f"({n} octets)")

        except Exception as e:
            errors += 1

            print(
                f"  ERREUR {bin_file.name} : {e}",
                file=sys.stderr
            )

    print(f"\n{len(bins) - errors}/{len(bins)} fichier(s) converti(s), "
        f"{total} octets ({total / 1024:.1f} Ko) traités "
        f"dans {out_dir}")

    if errors:
        sys.exit(1)

if __name__ == "__main__":
    main()