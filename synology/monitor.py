#!/usr/bin/env python3
"""
monitor.py — Point d'entrée du container monitor
Lance tous les scripts de collecte en parallèle via threads.
Pour ajouter un script : importer et ajouter à la liste MONITORS.
"""

# ----------------------------------------------------------------
# RESSOURCES BIBLIOTHÈQUES
# ----------------------------------------------------------------
import threading
import sys
import os

# ----------------------------------------------------------------
# RESSOURCES LOCALES
# ----------------------------------------------------------------
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "scripts"))

import nas_monitor
import freebox_monitor
import bridge_monitor

# ----------------------------------------------------------------
# OBJETS GLOBAUX
# ----------------------------------------------------------------
MONITORS = [
    nas_monitor.main,
    freebox_monitor.main,
    bridge_monitor.main
]

# ----------------------------------------------------------------
# API PUBLIQUES
# ----------------------------------------------------------------
def main():
    print("=" * 50)
    print("  Monitor — Démarrage de tous les scripts")
    print(f"  {len(MONITORS)} thread(s) actif(s)")
    print("=" * 50)

    # daemon=True : un Ctrl-C / docker stop tue le processus sans attendre les
    # boucles infinies des collecteurs, qui ne savent pas s'arrêter proprement.
    threads = [threading.Thread(target=m, daemon=True) for m in MONITORS]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

if __name__ == "__main__":
    main()
