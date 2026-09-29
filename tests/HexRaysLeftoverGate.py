#!/usr/bin/env python3
# Sherlock -- tests/HexRaysLeftoverGate.py
# An unpacked IDA database beside an image has two causes that need opposite answers, and the
# files alone cannot tell them apart:
#
#   - a live session holds it, and the answer is to wait for whoever has it open;
#   - an interrupted run left it, and the answer is to delete it and run again.
#
# Reading "another session holds it" off debris is how a killed export blocks itself forever, so
# this gate drives both branches: the same file, once held open and once not.
#
# A third branch splits the second one. Debris this worker left is its own to delete, and --resume
# has to clear it or no interrupted export ever resumes -- but a crashed interactive session leaves
# the identical files, and those are analysis IDA can still recover. The run marker is what
# separates them, so the same debris is driven twice: once without the marker and once with it.
import os
import subprocess
import sys
import tempfile
from pathlib import Path

worker = os.environ["SHERLOCK_HEXRAYS_WORKER"]
env = dict(os.environ)
env["PATH"] = os.environ["SHERLOCK_IDA_DIR"] + os.pathsep + env["PATH"]

failures = []

with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    image = root / "Pretend"
    image.write_bytes(b"not a Mach-O; the worker must refuse before it ever opens one")
    unpacked = root / "Pretend.id0"
    unpacked.write_bytes(b"an unpacked IDA database")

    def run(*extra):
        return subprocess.run([worker, "--image", str(image), "--store", str(root / "store"),
                               "--name", "Pretend", "--build", "gate", *extra],
                              capture_output=True, text=True, env=env, timeout=600)

    # Nothing holds the file: this is debris, and the worker says which files to remove.
    out = run()
    print(out.stdout, out.stderr)
    if out.returncode != 2:
        failures.append(f"debris: exit {out.returncode}, and an instrument that could not look is 2")
    if "interrupted run left" not in out.stdout:
        failures.append(f"debris: read as something other than a dead run's leftovers: {out.stdout!r}")
    if ".id0" not in out.stdout:
        failures.append("debris: the worker did not name the files to delete")

    # The same file, held open for exclusive write: now it is a live session.
    handle = open(unpacked, "r+b")
    try:
        held = run()
        print(held.stdout, held.stderr)
        if held.returncode != 2:
            failures.append(f"held: exit {held.returncode}, and an instrument that could not look is 2")
        if "a live process holds" not in held.stdout:
            failures.append(f"held: a held database was not read as held: {held.stdout!r}")
    finally:
        handle.close()

    # --resume alone does not license deleting a database. Debris and a crashed interactive
    # session look the same on disk, and the second is analysis somebody can still recover, so
    # what licenses the delete is our own marker -- not the caller's intent.
    unasked = run("--resume")
    print(unasked.stdout, unasked.stderr)
    if "interrupted run left" not in unasked.stdout:
        failures.append(f"resume without marker: the refusal was dropped: {unasked.stdout!r}")
    if not unpacked.exists():
        failures.append("resume without marker: a database nothing claims was deleted anyway")

    # With the marker beside it, the same debris is this worker's own interrupted run: it is
    # cleared and the run proceeds to fail on the image, which is the next honest answer.
    marker = root / "Pretend.sherlock-run"
    marker.write_text("Pretend")
    mine = run("--resume")
    print(mine.stdout, mine.stderr)
    if "interrupted run left" in mine.stdout:
        failures.append(f"resume with marker: own debris still blocked the run: {mine.stdout!r}")
    if unpacked.exists():
        failures.append("resume with marker: the worker's own debris survived the run")

    # And with no unpacked database at all, the refusal must come from the image, not the guard.
    unpacked.unlink(missing_ok=True)
    clean = run()
    print(clean.stdout, clean.stderr)
    if "interrupted run left" in clean.stdout or "a live process holds" in clean.stdout:
        failures.append(f"clean: the guard fired with no unpacked database present: {clean.stdout!r}")

if failures:
    for failure in failures:
        print(f"FAIL: {failure}")
    sys.exit(1)
print("OK: debris and a held database are told apart, and neither is reported as the other")
