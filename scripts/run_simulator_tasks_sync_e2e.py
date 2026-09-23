#!/usr/bin/env python3
"""Sync CrossTasks de bout en bout : le simulateur contre un crossdrop local.

Le simulateur demarre directement sur la cible reseau TASK_SYNC (jeton de
redemarrage silencieux, cible 9), avec un reseau Wi-Fi ouvert deja enregistre
pour que l'auto-connexion passe sans clavier. Son client HTTP est adosse a curl
et fait de VRAIES requetes. Chaque scenario seme une carte isolee, lance une
sync, puis compare la carte octet pour octet et interroge le serveur.

Deux serveurs :
  - le vrai crossdrop (branche feat/crosstasks), que ce script lance lui-meme
    depuis --crossdrop avec `npx tsx src/index.ts` et ces variables :
        DEVICE_TOKEN, RAINDROP_TOKEN, WEB_PASSWORD, SESSION_SECRET  (valeurs factices)
        DOMAIN=localhost  DATA_DIR=<dossier jetable>  PORT=<port>
        POLL_MINUTES=1440  (empeche l'appel a Raindrop pendant le test)
    Il faut `npm install` fait une fois dans le depot crossdrop.
  - un faux serveur Python integre, pour les echecs qu'un vrai serveur ne
    produit pas sur commande : 500, corps coupe dans une note ou une ligne,
    schema inconnu, connexion fermee avant la fin, 400.

La propriete verifiee sur chaque echec : RIEN de persiste ne change (index,
notes, curseur, file d'ops, compteur d'acquittement, secret), a la seule
exception documentee d'un 400, qui efface le curseur.

  scripts/run_simulator_tasks_sync_e2e.py --crossdrop ../../crossdrop [--no-build] [--keep DIR]

Sortie : PASS/FAIL par scenario ; code de sortie non nul si un seul echoue.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import http.cookiejar
import json
import os
import secrets
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROGRAM = ROOT / ".pio" / "build" / "simulator" / "program"
# MAC factice du stub esp_mac.h du simulateur : la cle d'ObfuscationUtils.
SIM_HW_KEY = bytes([0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01])
CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
SILENT_REBOOT_MAGIC = 0xC1EAB007  # src/main.cpp
TASK_SYNC_TARGET = 9  # NetworkBootTarget::TASK_SYNC
WEB_PASSWORD = "motdepasse-local-test"


def obfuscate(plaintext: str) -> str:
    """Meme format que obfuscation::obfuscateToBase64 (lib/Serialization/ObfuscationUtils.cpp)."""
    h = 2166136261
    for b in SIM_HW_KEY + plaintext.encode():
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    payload = b"CPV1" + struct.pack("<I", h) + plaintext.encode()
    return base64.b64encode(bytes(c ^ SIM_HW_KEY[i % 6] for i, c in enumerate(payload))).decode()


def new_secret() -> str:
    return "".join(secrets.choice(CROCKFORD) for _ in range(26))


# --------------------------------------------------------------------------- serveurs

class Crossdrop:
    def __init__(self, repo: Path, port: int, datadir: Path, log: Path):
        self.repo, self.port, self.datadir, self.log = repo, port, datadir, log
        self.base = f"http://127.0.0.1:{port}"
        self.jar = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(self.jar))
        self.proc: subprocess.Popen | None = None

    def start(self) -> None:
        env = dict(os.environ, DEVICE_TOKEN="devtoken-0123456789abcdef0123456789abcdef",
                   RAINDROP_TOKEN="raindrop-factice-pour-test-local", WEB_PASSWORD=WEB_PASSWORD,
                   SESSION_SECRET="sessionsecret-0123456789abcdef0123456789", DOMAIN="localhost",
                   POLL_MINUTES="1440", DATA_DIR=str(self.datadir), PORT=str(self.port))
        self.datadir.mkdir(parents=True, exist_ok=True)
        self.proc = subprocess.Popen(["npx", "tsx", "src/index.ts"], cwd=self.repo, env=env,
                                     stdout=self.log.open("a"), stderr=subprocess.STDOUT, start_new_session=True)
        for _ in range(150):
            try:
                urllib.request.urlopen(self.base + "/web/api/status", timeout=1)
                return
            except urllib.error.HTTPError:
                return  # 401 : le serveur repond
            except Exception:
                time.sleep(0.2)
        raise SystemExit(f"crossdrop did not start; see {self.log}")

    def stop(self) -> None:
        if self.proc:
            os.killpg(self.proc.pid, signal.SIGTERM)
            self.proc.wait(timeout=10)
            self.proc = None

    def req(self, method: str, path: str, body=None):
        data = None if body is None else json.dumps(body).encode()
        r = urllib.request.Request(self.base + path, data=data, method=method,
                                   headers={"content-type": "application/json"} if data else {})
        # Le cookie de session est Secure : on le renvoie a la main en http local.
        cookie = "; ".join(f"{c.name}={c.value}" for c in self.jar)
        if cookie:
            r.add_header("cookie", cookie)
        with self.opener.open(r) as resp:
            return json.loads(resp.read() or b"{}")

    def login(self) -> None:
        self.req("POST", "/web/login", {"password": WEB_PASSWORD})

    def pair(self, secret: str) -> None:
        self.req("POST", "/web/pair", {"secret": secret, "label": "simulateur"})

    def tasks(self) -> dict:
        return {t["id"]: t for t in self.req("GET", "/web/api/tasks")["tasks"]}


class FakeServer:
    """Repond a POST /api/v1/tasks/sync selon `mode` ; verifie le Bearer."""

    NOTE = "nouvelle note\nsur deux lignes"

    def __init__(self, port: int, secret: str):
        self.secret, self.mode = secret, None
        outer = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_POST(self):
                self.rfile.read(int(self.headers.get("content-length", 0)))
                m = outer.mode
                if m == "401" or self.headers.get("authorization") != f"Bearer {outer.secret}":
                    return self._send(401, b'{"error":"unauthorized"}')
                if m in ("500", "400"):
                    return self._send(int(m), b"{}")
                full = outer.body()
                if m == "trunc-note":
                    return self._send(200, full[: full.index(b"nouvelle note") + 5])
                if m == "trunc-line":
                    return self._send(200, full[: full.index(b"w00000002")])
                if m == "bad-schema":
                    return self._send(200, full.replace(b'"schema": 1', b'"schema": 2'))
                if m == "short-close":
                    self.send_response(200)
                    self.send_header("content-length", str(len(full)))
                    self.end_headers()
                    self.wfile.write(full[: len(full) // 2])
                    self.close_connection = True
                    return None
                return self._send(200, full)

            def _send(self, code, body):
                self.send_response(code)
                self.send_header("content-type", "application/x-ndjson")
                self.send_header("content-length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        self.httpd = ThreadingHTTPServer(("127.0.0.1", port), Handler)
        self.base = f"http://127.0.0.1:{port}"
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def body(self) -> bytes:
        hdr = {"schema": 1, "cursor": "Nw", "more": False, "reset": True, "count": 2,
               "rejected": [{"id": "d0000abcd", "reason": "full"}]}
        return (json.dumps(hdr) + "\n"
                + json.dumps({"id": "w00000001", "title": "Tâche avec note", "priority": 0, "done": True,
                              "noteBytes": len(self.NOTE.encode())}, ensure_ascii=False) + "\n" + self.NOTE + "\n"
                + json.dumps({"id": "w00000002", "title": "Nouvelle du web", "priority": 1, "done": False,
                              "noteBytes": 0}) + "\n").encode()

    def stop(self) -> None:
        self.httpd.shutdown()
        self.httpd.server_close()


# --------------------------------------------------------------------------- carte et simulateur

def tasks_dir(fs: Path) -> Path:
    return fs / ".crosspoint" / "tasks"


def seed_card(fs: Path, url: str, secret: str, tasks, ops_lines=(), cursor=None, notes=None, lang=None) -> None:
    t = tasks_dir(fs)
    (t / "n").mkdir(parents=True, exist_ok=True)
    (t / "index.json").write_text(json.dumps({"schema": 1, "tasks": tasks}, ensure_ascii=False))
    (t / "secret").write_text(obfuscate(secret))
    if ops_lines:
        (t / "ops.ndjson").write_text("".join(line + "\n" for line in ops_lines))
    if cursor:
        (t / "cursor.txt").write_text(cursor)
    for tid, text in (notes or {}).items():
        (t / "n" / f"{tid}.txt").write_text(text)
    cp = fs / ".crosspoint"
    (cp / "wifi.json").write_text(json.dumps(
        {"lastConnectedSsid": "SimOpen", "credentials": [{"ssid": "SimOpen", "password_obf": obfuscate("sim")}]}))
    settings = {"taskServerUrl": url, "taskEnabled": 1}
    if lang:
        settings["language"] = lang
    (cp / "crossink-settings.json").write_text(json.dumps(settings))


def run_sim(work: Path, tag: str, shot_ms=3000, quit_ms=5000) -> list[str]:
    env = dict(os.environ, SDL_VIDEODRIVER="dummy",
               CROSSPOINT_SIM_SILENT_REBOOT_MAGIC=str(SILENT_REBOOT_MAGIC),
               CROSSPOINT_SIM_SILENT_REBOOT_TARGET=str(TASK_SYNC_TARGET),
               CROSSPOINT_SIM_SILENT_REBOOT_PAYLOAD="0",
               CROSSPOINT_SIM_WIFI_NETWORKS="SimOpen:-40:open",
               CROSSPOINT_SIM_SCREENSHOTS=f"{shot_ms}:{work / (tag + '.bmp')}",
               CROSSPOINT_SIM_INPUT_SCRIPT=f"{quit_ms}:QUIT")
    log = work / f"{tag}.log"
    with log.open("w") as fh:
        subprocess.run([str(PROGRAM)], cwd=work, env=env, stdout=fh, stderr=subprocess.STDOUT,
                       timeout=quit_ms / 1000 + 30)
    return [line for line in log.read_text(errors="replace").splitlines() if "[TSYNC]" in line]


def card_hashes(fs: Path) -> dict:
    return {str(p.relative_to(fs)): hashlib.sha256(p.read_bytes()).hexdigest()[:12]
            for p in sorted(tasks_dir(fs).rglob("*")) if p.is_file()}


def card(fs: Path) -> dict:
    t = tasks_dir(fs)
    read = lambda name: (t / name).read_text() if (t / name).exists() else None  # noqa: E731
    return {"tasks": {x["id"]: x for x in json.loads((t / "index.json").read_text())["tasks"]},
            "ops": read("ops.ndjson"), "cursor": read("cursor.txt"), "acked": read("acked.txt"),
            "secret": read("secret"),
            "notes": {p.name: p.read_bytes() for p in (t / "n").iterdir()},
            "staged": sorted(p.name for p in (t / "ns").iterdir()) if (t / "ns").exists() else []}


# --------------------------------------------------------------------------- scenarios

class Runner:
    def __init__(self, out: Path):
        self.out, self.results = out, []

    def fresh(self, name: str) -> Path:
        work = self.out / name
        shutil.rmtree(work, ignore_errors=True)
        (work / "fs_").mkdir(parents=True)
        return work

    def check(self, name: str, ok: bool, lines: list[str], detail: str = "") -> None:
        self.results.append((name, ok))
        print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}")
        if not ok:
            for line in lines:
                print("      ", line[:170])


def real_server_scenarios(r: Runner, srv: Crossdrop) -> None:
    srv.login()
    secret = new_secret()
    srv.pair(secret)

    # 1. Premiere sync : un ajout de la liseuse part, deux taches web arrivent.
    work = r.fresh("real-first-sync")
    fs = work / "fs_"
    a = srv.req("POST", "/web/api/tasks", {"title": "Tâche web avec note", "priority": 0})["id"]
    note = "Première ligne\nseconde ligne — avec « guillemets »\n\nfin"
    srv.req("PATCH", f"/web/api/tasks/{a}", {"note": note})
    b = srv.req("POST", "/web/api/tasks", {"title": "Tâche web simple", "priority": 2})["id"]
    dev = {"id": "d0000abcd", "t": "Tâche créée sur la liseuse", "p": 1, "d": False, "n": 0}
    seed_card(fs, srv.base, secret, [dev],
              ['{"op":"add","id":"d0000abcd","title":"Tâche créée sur la liseuse","priority":1}'])
    lines = run_sim(work, "sync")
    c, web = card(fs), srv.tasks()
    r.check("real: first sync, both directions", c["ops"] is None and c["acked"] is None and bool(c["cursor"])
            and set(c["tasks"]) == {a, b, "d0000abcd"} and c["notes"].get(f"{a}.txt") == note.encode()
            and "d0000abcd" in web and c["staged"] == [], lines)

    # 2. Coche sur la liseuse ; suppression et note videe sur le web.
    (tasks_dir(fs) / "ops.ndjson").write_text(f'{{"op":"done","id":"{a}","done":true}}\n')
    srv.req("DELETE", f"/web/api/tasks/{b}")
    srv.req("PATCH", f"/web/api/tasks/{a}", {"note": ""})
    before = c["cursor"]
    lines = run_sim(work, "tick")
    c, web = card(fs), srv.tasks()
    r.check("real: tick up, deletion and emptied note down", web[a]["done"] is True and b not in c["tasks"]
            and f"{a}.txt" not in c["notes"] and c["cursor"] != before and c["ops"] is None, lines)

    # 3. Index perdu (coupure pendant un remplacement), curseur survivant : le
    #    chargement efface le curseur, la sync reconstruit TOUT l'index, et le
    #    secret, dans son propre fichier, n'a rien perdu.
    (tasks_dir(fs) / "index.json").unlink()
    lines = run_sim(work, "index-lost")
    c = card(fs)
    r.check("real: lost index rebuilds from a full snapshot, still paired",
            {a, "d0000abcd"} <= set(c["tasks"]) and c["tasks"][a]["d"] is True and bool(c["cursor"]), lines)

    # 4. Index.tmp orphelin (coupure entre remove et rename) : promu au boot.
    t = tasks_dir(fs)
    (t / "index.json").rename(t / "index.tmp")
    lines = run_sim(work, "tmp-promoted")
    c = card(fs)
    r.check("real: orphaned index.tmp is promoted", not (t / "index.tmp").exists() and a in c["tasks"], lines)

    # 5. Tache fantome : l'index porte une tache que le serveur ignore et le
    #    curseur manque. L'instantane complet la retire (plus de zombie).
    idx = json.loads((t / "index.json").read_text())
    idx["tasks"].append({"id": "w0000dead", "t": "Supprimée ailleurs", "p": 1, "d": False, "n": 0})
    (t / "index.json").write_text(json.dumps(idx))
    (t / "cursor.txt").unlink()
    lines = run_sim(work, "zombie")
    r.check("real: cursorless snapshot drops a task the server no longer has",
            "w0000dead" not in card(fs)["tasks"], lines)

    # 6. Curseur corrompu (prefixe d'une ecriture coupee) : 400, curseur efface,
    #    file intacte ; la sync suivante repart et reussit.
    (t / "cursor.txt").write_text("M")
    (t / "ops.ndjson").write_text(f'{{"op":"prio","id":"{a}","priority":2}}\n')
    lines = run_sim(work, "cursor-400")
    c = card(fs)
    ok400 = any("Response 400" in line for line in lines) and c["cursor"] is None and c["ops"] is not None
    lines2 = run_sim(work, "after-400")
    c = card(fs)
    r.check("real: corrupt cursor -> 400 drops it, next sync heals",
            ok400 and bool(c["cursor"]) and c["ops"] is None and srv.tasks()[a]["priority"] == 2, lines + lines2)

    # 7. Ops deja acquittees : acked.txt = 1 ; la premiere op (qui recocherait
    #    la tache) ne doit PAS repartir, meme si l'utilisateur l'a decochee sur
    #    le web entre-temps.
    srv.req("PATCH", f"/web/api/tasks/{a}", {"done": False})
    (t / "ops.ndjson").write_text(f'{{"op":"done","id":"{a}","done":true}}\n'
                                  f'{{"op":"prio","id":"{a}","priority":0}}\n')
    (t / "acked.txt").write_text("1")
    lines = run_sim(work, "acked")
    web = srv.tasks()
    r.check("real: an acknowledged op is never resent", web[a]["done"] is False and web[a]["priority"] == 0
            and card(fs)["acked"] is None and any("1 op(s) from offset 1" in line for line in lines), lines)

    # 8. 70 ajouts aux titres qui s'echappent en ~420 octets : trois tranches.
    work = r.fresh("real-slices")
    fs = work / "fs_"
    tasks, ops = [], []
    for i in range(70):
        tid, title = f"d{i:08x}", f"Tache {i:02d} " + '"\\' * 90
        tasks.append({"id": tid, "t": title, "p": 1, "d": False, "n": 0})
        ops.append(json.dumps({"op": "add", "id": tid, "title": title, "priority": 1}))
    seed_card(fs, srv.base, secret, tasks, ops)
    lines = run_sim(work, "slices", shot_ms=4000, quit_ms=6000)
    c, web = card(fs), srv.tasks()
    ids = {x["id"] for x in tasks}
    r.check("real: 70 ops in slices, titles round-trip", ids <= set(web) and ids <= set(c["tasks"])
            and all(web[x["id"]]["title"] == x["t"] for x in tasks) and c["ops"] is None
            and sum("POST" in line for line in lines) >= 3, lines)

    # 9. Serveur reappaire ailleurs : 401, carte identique.
    srv.pair(new_secret())
    (tasks_dir(fs) / "ops.ndjson").write_text('{"op":"done","id":"d00000000","done":true}\n')
    before = card_hashes(fs)
    lines = run_sim(work, "401")
    r.check("real: 401 changes nothing", before == card_hashes(fs) and any("Response 401" in line for line in lines),
            lines)


def fake_server_scenarios(r: Runner, lang: str | None) -> None:
    secret = new_secret()
    fake = FakeServer(38491 if lang else 38490, secret)
    note_old = "ancienne note"
    card_tasks = [{"id": "d0000abcd", "t": "Tâche locale à ajouter", "p": 1, "d": False, "n": 0},
                  {"id": "w00000001", "t": "Tâche avec note", "p": 0, "d": False, "n": len(note_old.encode())}]
    card_ops = ['{"op":"add","id":"d0000abcd","title":"Tâche locale à ajouter","priority":1}',
                '{"op":"done","id":"w00000001","done":true}']
    suffix = f"-{lang}" if lang else ""

    def case(name, mode, expect, url=fake.base, unchanged=True):
        fake.mode = mode
        work = r.fresh(f"fake{suffix}-{name}")
        fs = work / "fs_"
        seed_card(fs, url, secret, card_tasks, card_ops, "Mg", {"w00000001": note_old}, lang)
        before = card_hashes(fs)
        lines = run_sim(work, name)
        after = card_hashes(fs)
        diff = sorted(k for k in set(before) | set(after) if before.get(k) != after.get(k))
        ok = any(expect in line for line in lines) and ((before == after) == unchanged)
        r.check(f"fake{suffix}: {name}", ok, lines, "" if ok else f"changed={diff}")
        return fs, diff

    try:
        case("unreachable", None, "Response -", url="http://127.0.0.1:38499")
        case("401", "401", "Response 401")
        case("500", "500", "Response 500")
        case("truncated-in-note", "trunc-note", "reader unfinished")
        case("truncated-in-line", "trunc-line", "reader unfinished")
        case("bad-schema", "bad-schema", "reader error")
        case("short-close", "short-close", "Response")
        _, diff = case("400", "400", "Response 400", unchanged=False)
        r.check(f"fake{suffix}: 400 changes only the cursor", diff == [".crosspoint/tasks/cursor.txt"], [])
        fs, _ = case("ok-reset", "ok", "Sync complete", unchanged=False)
        c = card(fs)
        r.check(f"fake{suffix}: reset commit", set(c["tasks"]) == {"w00000001", "w00000002"}
                and c["notes"] == {"w00000001.txt": FakeServer.NOTE.encode()} and c["cursor"] == "Nw"
                and c["ops"] is None, [])
    finally:
        fake.stop()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--crossdrop", type=Path, help="crossdrop checkout (feat/crosstasks); omit to skip real-server runs")
    ap.add_argument("--port", type=int, default=38471)
    ap.add_argument("--keep", type=Path, help="keep cards, logs and screenshots here")
    ap.add_argument("--no-build", action="store_true")
    args = ap.parse_args()

    if not args.no_build:
        subprocess.run(["pio", "run", "-e", "simulator"], cwd=ROOT, check=True)
    out = args.keep or Path(tempfile.mkdtemp(prefix="tasks-sync-e2e-"))
    out.mkdir(parents=True, exist_ok=True)
    r = Runner(out)

    if args.crossdrop:
        # Base neuve a chaque passage : les scenarios comptent les taches du serveur.
        shutil.rmtree(out / "crossdrop-data", ignore_errors=True)
        srv = Crossdrop(args.crossdrop.resolve(), args.port, out / "crossdrop-data", out / "crossdrop.log")
        srv.start()
        try:
            real_server_scenarios(r, srv)
        finally:
            srv.stop()
    fake_server_scenarios(r, None)
    fake_server_scenarios(r, "FR")

    failed = [name for name, ok in r.results if not ok]
    print(f"\n{len(r.results) - len(failed)}/{len(r.results)} passed; artifacts in {out}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
