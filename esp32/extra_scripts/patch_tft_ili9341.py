# Applique automatiquement la séquence d'init ILI9341 patchée (correctif de
# l'inversion des couleurs — commande 0x21) par-dessus celle fournie par
# TFT_eSPI, avant chaque compilation.
#
# Pourquoi un script et pas -include : TFT_Drivers/ILI9341_Init.h n'est PAS un
# header autonome, c'est un fragment de code (writecommand/writedata) que
# TFT_eSPI #include À L'INTÉRIEUR d'une fonction (begin()). Le force-inclure via
# -include le placerait au niveau fichier -> erreurs. La seule façon propre de
# le surcharger est de remplacer le fichier dans la lib installée — ce que fait
# ce hook, à chaque build, donc ça survit à un `pio run -t clean` / suppression
# de .pio (fini la recopie manuelle).
#
# Source de vérité versionnée : include/ILI9341_Init.h.

Import("env")
import os
import shutil

src = os.path.join(env.subst("$PROJECT_DIR"), "include", "ILI9341_Init.h")
dst = os.path.join(
    env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"),
    "TFT_eSPI", "TFT_Drivers", "ILI9341_Init.h",
)

if not os.path.isfile(src):
    print("[patch ILI9341] source include/ILI9341_Init.h introuvable — ignoré")
elif not os.path.isdir(os.path.dirname(dst)):
    # TFT_eSPI pas encore extrait (build tout frais) — sera appliqué au build suivant.
    print("[patch ILI9341] TFT_eSPI pas encore extrait — appliqué au prochain build")
else:
    # Copie seulement si différent, pour ne pas forcer une recompilation de TFT_eSPI à chaque build.
    need = (not os.path.isfile(dst)) or (open(src, "rb").read() != open(dst, "rb").read())
    if need:
        shutil.copyfile(src, dst)
        print("[patch ILI9341] correctif couleur (0x21) appliqué à TFT_eSPI")
