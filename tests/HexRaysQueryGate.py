#!/usr/bin/env python3
# Sherlock -- tests/Sherlock/HexRaysQueryGate.py
# `fn` over the real stores: the enclosing function, layer 2's pseudocode, layer 1's disassembly,
# and what an address layer 2 does not cover reports.
#
# The addresses are read out of the stores rather than written here, so the gate holds the command
# rather than a particular function surviving in a particular build.
import json
import os
import sqlite3
import subprocess
import sys
from pathlib import Path

sherlock = os.environ["SHERLOCK_CLI"]
store = Path(os.environ["SHERLOCK_STORE"])
cache = os.environ["SHERLOCK_CACHE"]

failures = []


def run(args):
    return subprocess.run([sherlock, "fn"] + args + ["--store", str(store)],
                          capture_output=True, text=True, timeout=900)


def images():
    db = sqlite3.connect(f"file:{store / 'Catalog.db'}?mode=ro", uri=True)
    rows = db.execute("SELECT Path, Name FROM Image WHERE State = 'FactsDone'").fetchall()
    db.close()
    return [(path, name, Path(path).name) for path, name in rows]


def first_function(layer_one: Path):
    db = sqlite3.connect(f"file:{layer_one}?mode=ro", uri=True)
    row = db.execute("SELECT Address FROM Function WHERE Size > 64 ORDER BY Address LIMIT 1").fetchone()
    db.close()
    return row[0] if row else None


with_layer_two = []
without_layer_two = []
for path, name, basename in images():
    layer_one = store / "Images" / name
    if not layer_one.is_file():
        continue
    address = first_function(layer_one)
    if address is None:
        continue
    target = with_layer_two if (store / "Images" / f"{basename}.HexRays.db").is_file() else without_layer_two
    target.append((basename, address))

if not with_layer_two:
    print("NOT VERIFIED: no image has a layer-2 store, so `fn` cannot be read either way")
    sys.exit(2)

# 1. The pseudocode path, and the line saying what pseudocode is worth as evidence.
basename, address = with_layer_two[0]
out = run([hex(address)])
if out.returncode != 0:
    failures.append(f"1: fn {address:#x} exited {out.returncode}: {out.stdout}{out.stderr}")
if f"is in {address:#x}" not in out.stdout:
    failures.append(f"1: fn did not name the enclosing function: {out.stdout[:400]!r}")
if "pseudocode locates and does not close a value" not in out.stdout:
    failures.append("1: fn printed pseudocode without saying it locates rather than closes a value")
if len(out.stdout.splitlines()) < 6:
    failures.append(f"1: fn printed {len(out.stdout.splitlines())} lines, which is no pseudocode")

# 2. --asm answers from layer 1 and says how much of the function it decoded.
asm = run([hex(address), "--asm", "--cache", cache])
if asm.returncode != 0:
    failures.append(f"2: fn --asm exited {asm.returncode}: {asm.stdout}{asm.stderr}")
if "instruction words decoded" not in asm.stdout:
    failures.append(f"2: fn --asm did not report its own coverage: {asm.stdout[:400]!r}")
if "pseudocode locates" in asm.stdout:
    failures.append("2: fn --asm consulted layer 2, which is not what --asm asks for")

# 3. An address layer 2 does not cover names the command that would cover it -- absence is never
# bare, and an agent hunting for that command is what the phase-2 measurement caught.
if without_layer_two:
    missing_name, missing_address = without_layer_two[0]
    out = run([hex(missing_address)])
    if "layer 2: not built" not in out.stdout:
        failures.append(f"3: {missing_name} has no layer-2 store and fn did not say so: "
                        f"{out.stdout[:400]!r}")
    if "build hexrays" not in out.stdout:
        failures.append(f"3: fn reported layer 2 missing without naming the command that builds it")
else:
    print("note: every image carries a layer-2 store, so case 3 had nothing to read")

# 4. --json carries the same answer, coverage included.
raw = run([hex(address), "--json"])
try:
    payload = json.loads(raw.stdout)
except json.JSONDecodeError:
    payload = None
    failures.append(f"4: --json did not print JSON: {raw.stdout[:400]!r}")
if payload is not None:
    if payload.get("verdict") != "FOUND":
        failures.append(f"4: --json says {payload.get('verdict')} where the text path found the function")
    if not payload.get("output"):
        failures.append("4: --json carried no output lines")
    if payload.get("coverage", {}).get("total", 0) <= 0:
        failures.append("4: --json carried no coverage")

if failures:
    for failure in failures:
        print(f"FAIL: {failure}")
    sys.exit(1)
print(f"OK: fn read {basename} at {address:#x} from layer 2, disassembled it from layer 1, "
      f"and reports what it cannot cover")
