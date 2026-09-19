#!/usr/bin/env python3
# Sherlock -- tests/Sherlock/HexRaysBuildGate.py
# The three behaviours that belong to the parent of `build hexrays`, none of which need IDA:
#
#   1. An image left HexRaysRunning is a crash, not progress. --resume puts it back to Pending
#      and exports it again, rather than counting it as done.
#   2. A worker that exits non-zero leaves HexRaysFailed carrying a reason, and the parent moves
#      on to the next image instead of stopping the run.
#   3. Below the free-space floor the run stops with exit 2 -- NOT VERIFIED, never exit 1 -- and
#      says the floor.
#
# A stand-in worker stands for the real one, so the gate costs seconds and no licence seat. It
# writes a store the parent then reads for coverage, which is exactly the contract between them.
import os
import sqlite3
import subprocess
import sys
import tempfile
from pathlib import Path

sherlock = os.environ["SHERLOCK_CLI"]
python = sys.executable

STUB = '''import sqlite3, sys
args = dict(zip(sys.argv[1::2], sys.argv[2::2]))
mode = "''{mode}''"
open(args["--store"] + "/stub.log", "a", encoding="utf-8").write(args["--name"] + " ")
if mode == "fail":
    print("stub: refusing this image")
    sys.exit(1)
store = args["--store"] + "/" + args["--name"] + ".HexRays.db"
db = sqlite3.connect(store)
db.executescript("""
CREATE TABLE IF NOT EXISTS Meta(Key TEXT PRIMARY KEY, Value TEXT NOT NULL) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS Decompilation(Function INTEGER PRIMARY KEY, Pseudocode BLOB,
    Plain INTEGER NOT NULL, Lines INTEGER NOT NULL, Status TEXT NOT NULL, Reason TEXT,
    Seconds REAL NOT NULL) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS IdaName(Address INTEGER PRIMARY KEY, Name TEXT NOT NULL) WITHOUT ROWID;
""")
db.executemany("INSERT OR REPLACE INTO Meta VALUES(?,?)",
               [("Kind", "HexRays"), ("SchemaVersion", "1"), ("Build", args.get("--build", "")),
                ("ImagePath", args.get("--image-path", "")), ("IdaVersion", "stub")])
db.execute("INSERT OR REPLACE INTO Decompilation VALUES(1, NULL, 0, 3, 'Ok', NULL, 0.1)")
db.execute("INSERT OR REPLACE INTO Decompilation VALUES(2, NULL, 0, 0, 'Failed', 'stub', 0.0)")
db.commit()
print("stub: wrote", store)
'''


def write_stub(directory: Path, mode: str) -> Path:
    path = directory / f"stub_{mode}.py"
    path.write_text(STUB.replace("''{mode}''", mode), encoding="utf-8")
    launcher = directory / f"stub_{mode}.cmd"
    launcher.write_text(f'@echo off\r\n"{python}" "{path}" %*\r\n', encoding="utf-8")
    return launcher


def make_catalog(store: Path, images) -> None:
    store.mkdir(parents=True, exist_ok=True)
    db = sqlite3.connect(store / "Catalog.db")
    db.executescript("""
    CREATE TABLE IF NOT EXISTS Meta(Key TEXT PRIMARY KEY, Value TEXT NOT NULL) WITHOUT ROWID;
    CREATE TABLE IF NOT EXISTS Image(Path TEXT PRIMARY KEY, Name TEXT NOT NULL, Tower TEXT,
        Header INTEGER NOT NULL, State TEXT NOT NULL, Reason TEXT, FactsVersion TEXT,
        HexRaysVersion TEXT) WITHOUT ROWID;
    CREATE TABLE IF NOT EXISTS Coverage(Image TEXT NOT NULL, Layer TEXT NOT NULL, Unit TEXT NOT NULL,
        Read INTEGER NOT NULL, Total INTEGER NOT NULL, PRIMARY KEY(Image, Layer)) WITHOUT ROWID;
    """)
    db.executemany("INSERT OR REPLACE INTO Meta VALUES(?,?)",
                   [("Kind", "Catalog"), ("SchemaVersion", "1"), ("Build", "26A5416b"),
                    ("CacheUuid", "gate")])
    for name, state in images:
        db.execute("INSERT OR REPLACE INTO Image VALUES(?,?,?,?,?,NULL,'gate',NULL)",
                   (f"/usr/lib/{name}", name, "Gate", 0, state))
    db.commit()
    db.close()


def run(store: Path, images_dir: Path, worker: Path, extra):
    env = dict(os.environ)
    env["SHERLOCK_HEXRAYS_WORKER"] = str(worker)
    return subprocess.run([sherlock, "build", "hexrays", "--store", str(store),
                           "--images-dir", str(images_dir), "--workers", "1"] + extra,
                          capture_output=True, text=True, env=env, timeout=600)


def state_of(store: Path, name: str):
    db = sqlite3.connect(store / "Catalog.db")
    row = db.execute("SELECT State, Reason FROM Image WHERE Name = ?", (name,)).fetchone()
    db.close()
    return row


failures = []

with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    images = root / "dylibs"
    images.mkdir()
    for name in ("Alpha", "Beta"):
        (images / name).write_bytes(b"not a real image, and the stub never reads it")

    # 1. The resume reset.
    store = root / "resume"
    make_catalog(store, [("Alpha", "HexRaysRunning"), ("Beta", "Pending")])
    ok_worker = write_stub(root, "ok")
    out = run(store, images, ok_worker, ["--resume"])
    print(out.stdout, out.stderr)
    alpha = state_of(store, "Alpha")
    if out.returncode != 0:
        failures.append(f"1: the run exited {out.returncode}")
    if alpha[0] != "HexRaysDone":
        failures.append(f"1: Alpha was left HexRaysRunning and --resume treated it as {alpha[0]}, "
                        f"not as the crash it is")

    # The same run again must spawn nothing: --resume is what keeps a rerun cheap, and the log
    # the stand-in keeps is the only witness that no work happened.
    log = store / "Images" / "stub.log"
    before = len(log.read_text(encoding="utf-8").split()) if log.exists() else 0
    again = run(store, images, ok_worker, ["--resume"])
    after = len(log.read_text(encoding="utf-8").split()) if log.exists() else 0
    if again.returncode != 0:
        failures.append(f"1: the second --resume run exited {again.returncode}")
    if after != before:
        failures.append(f"1: a second --resume run spawned {after - before} worker(s) over images "
                        f"already HexRaysDone")
    if before == 0:
        failures.append("1: the first run spawned no worker at all, so the skip proves nothing")

    # 2. A worker that fails leaves a reason, and the run continues.
    store = root / "failure"
    make_catalog(store, [("Alpha", "Pending"), ("Beta", "Pending")])
    bad_worker = write_stub(root, "fail")
    out = run(store, images, bad_worker, [])
    print(out.stdout, out.stderr)
    if out.returncode != 1:
        failures.append(f"2: a failing worker gave exit {out.returncode}, not 1")
    for name in ("Alpha", "Beta"):
        state, reason = state_of(store, name)
        if state != "HexRaysFailed":
            failures.append(f"2: {name} is {state}, not HexRaysFailed")
        elif not reason:
            failures.append(f"2: {name} failed with no reason recorded")
    if "2 failed" not in out.stdout:
        failures.append(f"2: the run stopped at the first failure instead of carrying on: {out.stdout!r}")

    # 3. The free-space floor. No disk has this much free, so the floor always bites.
    store = root / "floor"
    make_catalog(store, [("Alpha", "Pending")])
    out = run(store, images, ok_worker, ["--min-free-bytes", str(1 << 62)])
    print(out.stdout, out.stderr)
    if out.returncode != 2:
        failures.append(f"3: the floor gave exit {out.returncode}, not 2 -- exit 2 is NOT VERIFIED "
                        f"and a full disk is not a verified failure")
    if "floor" not in out.stdout:
        failures.append(f"3: the floor did not say it was the floor: {out.stdout!r}")
    state, _ = state_of(store, "Alpha")
    if state == "HexRaysRunning":
        failures.append("3: the floor left an image HexRaysRunning, which a later --resume would "
                        "read as a crash")

    # 4. The reset is not the selection rule. An image the run never reaches -- the floor stops it
    # first -- must already read Pending, because a row left HexRaysRunning by a crash makes
    # `status` claim work is in flight that no process is doing. Without the reset, selecting
    # not-Done images still exports it, so only a run that stops early tells the two apart.
    store = root / "reset"
    make_catalog(store, [("Alpha", "HexRaysRunning"), ("Beta", "HexRaysRunning")])
    out = run(store, images, ok_worker, ["--resume", "--min-free-bytes", str(1 << 62)])
    print(out.stdout, out.stderr)
    for name in ("Alpha", "Beta"):
        state, _ = state_of(store, name)
        if state == "HexRaysRunning":
            failures.append(f"4: {name} still reads HexRaysRunning after a --resume run that "
                            f"stopped at the floor, so the catalog claims a worker is on it")

if failures:
    for failure in failures:
        print(f"FAIL: {failure}")
    sys.exit(1)
print("OK: resume resets a crashed image, a failed worker leaves a reason and the run carries on, "
      "and the free-space floor exits 2")
