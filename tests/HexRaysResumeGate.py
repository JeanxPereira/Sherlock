#!/usr/bin/env python3
# Sherlock -- tests/Sherlock/HexRaysResumeGate.py
# An export of a large image runs for hours and is interrupted by things that have nothing to do
# with it: a machine short of memory, a session that ends, a reboot. What that interruption costs
# is a property of the worker, not of the accident, and it is the property this gate measures.
#
# The worker is started on a real image, killed once a batch has landed, and started again with
# --resume. Two things have to hold, and neither is visible from a run that finishes:
#
#   - the store the killed run left carries rows, so the work before the kill survived it;
#   - the second run continues from them instead of decompiling the image again.
#
# The kill is TerminateProcess, which is what the machine does under memory pressure -- no unwind,
# no destructor, nothing flushed on the way out.
import os
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
from pathlib import Path

BATCH = 25          # small enough that a flush lands in the first seconds of a short image
DEADLINE = 300.0    # the image below decompiles in well under this, even on a loaded machine

worker = os.environ["SHERLOCK_HEXRAYS_WORKER"]
image_source = Path(os.environ["SHERLOCK_RESUME_IMAGE"])
if not image_source.is_file():
    sys.exit(f"NOT VERIFIED: {image_source} is not extracted -- carve it out of the cache first")

env = dict(os.environ)
env["PATH"] = os.environ["SHERLOCK_IDA_DIR"] + os.pathsep + env["PATH"]

failures = []


def rows(store):
    # The writer holds the database while it runs; under WAL a reader is still allowed, and a
    # lock that is momentarily contended reads as "not yet", not as a failure.
    if not store.exists():
        return 0
    try:
        db = sqlite3.connect(f"file:{store}?mode=ro", uri=True, timeout=1.0)
        try:
            return db.execute("SELECT count(*) FROM Decompilation").fetchone()[0]
        finally:
            db.close()
    except sqlite3.Error:
        return 0


def timings(store):
    # Each row records the seconds its own decompilation took. A resume that skips what is
    # already stored leaves those numbers alone; one that decompiles the image again overwrites
    # them with fresh measurements. It is the difference between continuing and restarting, and
    # the row count cannot see it -- INSERT OR REPLACE lands on the same 2000 rows either way.
    db = sqlite3.connect(f"file:{store}?mode=ro", uri=True)
    try:
        return dict(db.execute("SELECT Function, Seconds FROM Decompilation").fetchall())
    finally:
        db.close()


def meta(store, key):
    db = sqlite3.connect(f"file:{store}?mode=ro", uri=True)
    try:
        row = db.execute("SELECT Value FROM Meta WHERE Key = ?", (key,)).fetchone()
        return row[0] if row else None
    finally:
        db.close()


with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    # Copied out of the corpus: the kill leaves an unpacked database beside the image, and the
    # corpus is not the place to leave one.
    image = root / image_source.name
    shutil.copy2(image_source, image)
    store_dir = root / "Images"
    store = store_dir / f"{image_source.name}.HexRays.db"

    command = [worker, "--image", str(image), "--store", str(store_dir),
               "--name", image_source.name, "--build", "gate", "--batch", str(BATCH)]

    killed = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, env=env)
    banked = 0
    reached_its_end = False
    started = time.monotonic()
    while time.monotonic() - started < DEADLINE:
        if killed.poll() is not None:
            reached_its_end = True
            break
        banked = rows(store)
        if banked >= BATCH:
            break
        time.sleep(0.2)

    if not reached_its_end:
        killed.kill()
    killed.wait(timeout=60)
    print(f"{'ran to its end' if reached_its_end else 'killed'} after {banked} rows, "
          f"{time.monotonic() - started:.1f}s")

    # A run that reached its own end wrote everything at once, and the store it left is whole.
    # That has to be told apart from a killed one before anything else is read off it: a whole
    # store passes every check below while proving nothing, because no interruption was survived.
    # The Complete flag closes the narrow race where the last flush lands between two polls.
    if reached_its_end or meta(store, "Complete") == "1":
        failures.append("the export reached its end before any batch could be caught on disk: "
                        "either nothing is written until the last function -- so an interruption "
                        f"costs the whole image -- or this image is faster than {BATCH} functions "
                        "of head start and cannot gate a resume")
    elif banked < BATCH:
        failures.append(f"no batch reached disk within {DEADLINE:.0f}s while the run was alive")
    else:
        if meta(store, "Complete") != "0":
            failures.append("the killed run's store does not say it is unfinished, so the next "
                            "run cannot tell a partial store from a whole one")
        if not (root / f"{image_source.name}.sherlock-run").exists():
            failures.append("the killed run left no marker, and without one its own debris "
                            "blocks every later resume")

        before = timings(store)
        resumed = subprocess.run(command + ["--resume"], capture_output=True, text=True,
                                 env=env, timeout=3600)
        print(resumed.stdout, resumed.stderr)
        if resumed.returncode != 0:
            failures.append(f"the resumed run exited {resumed.returncode}")
        else:
            fields = dict(part.split("=", 1) for part in resumed.stdout.split()
                          if "=" in part and not part.startswith("--"))
            carried = int(fields.get("carried", 0))
            functions = int(fields.get("functions", 0))
            if carried < banked:
                failures.append(f"the resumed run carried {carried} rows where the kill had "
                                f"banked {banked}: it started the image again")
            if meta(store, "Complete") != "1":
                failures.append("the resumed run did not mark the store complete")

            attempted = rows(store)
            if attempted != functions:
                failures.append(f"the store holds {attempted} rows for an image of {functions} "
                                f"functions: the resume lost or repeated some")

            after = timings(store)
            redone = [address for address, seconds in before.items()
                      if after.get(address) != seconds]
            if redone:
                failures.append(f"{len(redone)} of the {len(before)} rows the kill had banked were "
                                f"decompiled again: the resume restarted the image and only the "
                                f"writes were idempotent")
            print(f"carried {carried} of {attempted} rows across the kill, "
                  f"{len(before) - len(redone)} of them untouched by the second run")

if failures:
    for failure in failures:
        print(f"FAIL: {failure}")
    sys.exit(1)
print("OK: a killed export keeps what it banked, and --resume continues it instead of restarting")
