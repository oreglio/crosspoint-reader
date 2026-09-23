#!/usr/bin/env python3
"""Capture chaque ecran CrossTasks dans le simulateur, un passage par theme.

Prepare une carte simulee isolee (index de taches + une note), lance le binaire
sans fenetre, et laisse le simulateur sauvegarder une capture par etape du
parcours src/simulator/SimulatorTasksTour.cpp. Convertit les BMP en PNG.

  scripts/run_simulator_tasks_tour.py [--out DIR] [--themes classic,lyra,minimal] [--no-build]

La liste STEPS et les constantes de temps DOIVENT rester identiques a
kSteps / kStartMs / kPeriodMs dans SimulatorTasksTour.cpp.
"""
from __future__ import annotations

import argparse
import base64
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROGRAM = ROOT / ".pio" / "build" / "simulator" / "program"

START_MS, PERIOD_MS, SHOT_OFFSET_MS = 3500, 1300, 1000
STEPS = [
    "00-pair-cold-boot", "01-list", "move-down", "02-ticked-sinks", "move-up", "03-done-row-selected",
    "04-done-expanded", "reopen", "05-detail-with-note", "05b-detail-ticked", "back-to-list",
    "13-task-menu", "menu-down-1", "menu-down-2", "menu-down-3", "menu-down-4", "14-menu-delete-selected",
    "15-delete-confirm", "confirm-down", "16-deleted", "menu-again", "menu-edit-1", "17-menu-edit-selected",
    "18-edit-keyboard", "leave-keyboard",
    "06-all-done", "07-empty", "08-add-keyboard", "09-pair", "10-pair-confirm",
    "11-pair-cancelled", "ask-again", "pick-confirm", "12-pair-renewed", "end",
]
THEMES = {"classic": 0, "lyra": 1, "minimal": 5}
LANGS = {"en": 0, "fr": 2}

NOTE_ID = "d00000001"
NOTE = (
    "Appeler avant midi, le cabinet ferme a 12h30.\n"
    "Demander la copie de l'acte et le releve des frais.\n\n"
    "Si le notaire est absent, laisser un message a sa collaboratrice "
    "en precisant la reference du dossier et un numero de rappel."
)
TASKS = [
    {"id": NOTE_ID, "t": "Rappeler le notaire", "p": 0, "d": False, "n": len(NOTE.encode())},
    {"id": "d00000002", "t": "Relire le contrat de location avant la signature de jeudi", "p": 1, "d": False, "n": 0},
    {"id": "d00000003", "t": "Commander du papier", "p": 2, "d": False, "n": 0},
    {"id": "d00000004", "t": "Arroser les plantes", "p": 1, "d": False, "n": 0},
    {"id": "d00000005", "t": "Preparer la reunion", "p": 0, "d": False, "n": 0},
    {"id": "d00000006", "t": "Payer la facture EDF", "p": 1, "d": True, "n": 0},
    {"id": "d00000007", "t": "Reserver le train", "p": 2, "d": True, "n": 0},
    {"id": "d00000008", "t": "Envoyer les photos", "p": 1, "d": True, "n": 0},
]


# Secret d'appairage deja stocke : l'etape 00 ouvre l'ecran d'appairage AVANT
# toute visite de la liste, donc sur un store jamais charge, comme apres un
# redemarrage. Le code affiche doit etre celui-ci ; tout autre code veut dire
# que l'ecran a ecrase l'appairage existant. Forme canonique, reconnaissable.
SEEDED_SECRET = "5EEDC0DE5EEDC0DE5EEDC0DE5E"
# MAC factice du stub esp_mac.h du simulateur : la cle d'ObfuscationUtils.
SIM_HW_KEY = bytes([0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01])


def obfuscate(plaintext: str) -> str:
    """Meme format que obfuscation::obfuscateToBase64 (lib/Serialization/ObfuscationUtils.cpp)."""
    h = 2166136261
    for b in SIM_HW_KEY + plaintext.encode():
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    payload = b"CPV1" + h.to_bytes(4, "little") + plaintext.encode()
    xored = bytes(c ^ SIM_HW_KEY[i % len(SIM_HW_KEY)] for i, c in enumerate(payload))
    return base64.b64encode(xored).decode()


def seed(fs: Path) -> None:
    tasks = fs / ".crosspoint" / "tasks"
    (tasks / "n").mkdir(parents=True, exist_ok=True)
    (tasks / "index.json").write_text(json.dumps({"schema": 1, "tasks": TASKS}))
    # Le secret a son propre fichier (TaskStore::secretPath), jamais l'index.
    (tasks / "secret").write_text(obfuscate(SEEDED_SECRET))
    (tasks / "n" / f"{NOTE_ID}.txt").write_text(NOTE)


def run_theme(name: str, lang: str, out: Path, spacing: int = 0, font: int = 0) -> list[Path]:
    work = Path(tempfile.mkdtemp(prefix=f"tour-{name}-{lang}-"))
    seed(work / "fs_")
    shots = [(i, s) for i, s in enumerate(STEPS) if s[0].isdigit()]
    schedule = ";".join(
        f"{START_MS + i * PERIOD_MS + SHOT_OFFSET_MS}:{work / f'{s}.bmp'}" for i, s in shots
    )
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", CROSSINK_SIMULATOR_TASKS_TOUR="1",
               CROSSINK_SIMULATOR_TASKS_THEME=str(THEMES[name]), CROSSINK_SIMULATOR_TASKS_LANG=str(LANGS[lang]),
               CROSSINK_SIMULATOR_TASKS_SPACING=str(spacing), CROSSINK_SIMULATOR_TASKS_FONT=str(font),
               CROSSPOINT_SIM_SCREENSHOTS=schedule)
    timeout = (START_MS + len(STEPS) * PERIOD_MS) / 1000 + 15
    log = work / "run.log"
    with log.open("w") as fh:
        proc = subprocess.run([str(PROGRAM)], cwd=work, env=env, stdout=fh, stderr=subprocess.STDOUT,
                              timeout=timeout)
    text = log.read_text(errors="replace")
    seen = [line.split(" step ", 1)[1].split(" ", 1)[1].strip()
            for line in text.splitlines() if "[TOUR]" in line and " step " in line]
    if seen[: len(STEPS)] != STEPS[: len(seen)]:
        print(f"  !! le parcours C++ et STEPS ont diverge : {seen}", file=sys.stderr)
    if proc.returncode != 0:
        print(f"  !! sortie {proc.returncode}, journal : {log}", file=sys.stderr)
    produced = []
    dest = out / (f"{lang}-{name}" + (f"-spacing{spacing}" if spacing else "") + (f"-font{font}" if font else ""))
    dest.mkdir(parents=True, exist_ok=True)
    for _, s in shots:
        bmp = work / f"{s}.bmp"
        if not bmp.exists():
            print(f"  !! capture manquante : {lang}-{name}/{s}", file=sys.stderr)
            continue
        png = dest / f"{s}.png"
        subprocess.run(["sips", "-s", "format", "png", str(bmp), "--out", str(png)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        produced.append(png)
    shutil.copy2(log, dest / "run.log")
    return produced


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / ".superpowers" / "tour"))
    ap.add_argument("--themes", default="classic,lyra,minimal")
    ap.add_argument("--langs", default="fr,en")
    ap.add_argument("--no-build", dest="build", action="store_false")
    ap.add_argument("--spacing", type=int, choices=(0, 1, 2), default=0, help="reglage Espacement des taches")
    ap.add_argument("--font", type=int, choices=(0, 1, 2), default=0, help="reglage Taille du texte des taches")
    args = ap.parse_args()
    if args.build and subprocess.run(["pio", "run", "-e", "simulator"], cwd=ROOT).returncode != 0:
        return 1
    if not PROGRAM.exists():
        print(f"binaire introuvable : {PROGRAM}", file=sys.stderr)
        return 1
    out = Path(args.out)
    total = 0
    for lang in args.langs.split(","):
        for name in args.themes.split(","):
            print(f"{lang} / {name}...", flush=True)
            total += len(run_theme(name.strip(), lang.strip(), out, args.spacing, args.font))
    print(f"{total} captures dans {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
