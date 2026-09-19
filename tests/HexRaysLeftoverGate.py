#!/usr/bin/env python3
# Sherlock -- tests/Sherlock/HexRaysLeftoverGate.py
# An unpacked IDA database beside an image has two causes that need opposite answers, and the
# files alone cannot tell them apart:
#
#   - a live session holds it, and the answer is to wait for whoever has it open;
#   - an interrupted run left it, and the answer is to delete it and run again.
#
# Reading "another session holds it" off debris is how a killed export blocks itself forever, so
# this gate drives both branches: the same file, once held open and once not.
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

    def run():
        return subprocess.run([worker, "--image", str(image), "--store", str(root / "store"),
                               "--name", "Pretend", "--build", "gate"],
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

    # And with no unpacked database at all, the refusal must come from the image, not the guard.
    unpacked.unlink()
    clean = run()
    print(clean.stdout, clean.stderr)
    if "interrupted run left" in clean.stdout or "a live process holds" in clean.stdout:
        failures.append(f"clean: the guard fired with no unpacked database present: {clean.stdout!r}")

if failures:
    for failure in failures:
        print(f"FAIL: {failure}")
    sys.exit(1)
print("OK: debris and a held database are told apart, and neither is reported as the other")
