#!/usr/bin/env python3
# Sherlock -- tests/HexRaysQueryGate.py
# `fn` over the real stores: the enclosing function, layer 2's pseudocode, layer 1's disassembly,
# and what an address layer 2 does not cover reports.
#
# The addresses are read out of the stores rather than written here, so the gate holds the command
# rather than a particular function surviving in a particular build.
import json
import os
import re
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

# 3b. Layer 2 is decompiled from an image carved out of the cache, so a call leaving the image
# reads as MEMORY[<island>] with no name. Layer 1 read the WHOLE cache and stored that island
# beside the target it jumps to, so `fn` performs the join and prints the target's symbol in the
# pseudocode itself. What this case holds is the accounting around it, because a substitution
# that silently covers part of the set is worse than none: the count of islands named, and, for
# every one it could not name, WHICH of the two reasons applies -- a target outside the indexed
# images, or an indexed image with no symbol at that address. The two have different answers, so
# reporting them as one number would send a reader at the wrong instrument.
crossing = None
db = sqlite3.connect(f"file:{store / 'Images' / (basename + '.db')}?mode=ro", uri=True)
row = db.execute(
    "SELECT c.Site, count(*) FROM Call c WHERE NOT EXISTS "
    "(SELECT 1 FROM Segment s WHERE c.Target >= s.Address AND c.Target < s.Address + s.Size) "
    "GROUP BY c.Site LIMIT 1").fetchone()
db.close()
if row:
    # The function containing that call site, from the same resolution `fn` performs.
    db = sqlite3.connect(f"file:{store / 'Images' / (basename + '.db')}?mode=ro", uri=True)
    owner = db.execute("SELECT Address FROM Function WHERE Address <= ? ORDER BY Address DESC LIMIT 1",
                       (row[0],)).fetchone()
    db.close()
    crossing = owner[0] if owner else None

if crossing is None:
    print("note: no call leaves this image, so the cross-image line had nothing to state")
else:
    out = run([hex(crossing)])
    if "call island" not in out.stdout:
        failures.append(f"3b: fn {crossing:#x} contains a call leaving the image and said nothing "
                        f"about it: {out.stdout[:400]!r}")

    # How many islands layer 1 recorded for this function, and how many of their targets fall
    # inside an indexed image. Measured here from the store rather than read off fn's own line,
    # so the gate is not grading the tool against its own claim.
    db = sqlite3.connect(f"file:{store / 'Images' / (basename + '.db')}?mode=ro", uri=True)
    targets = [t for (t,) in db.execute(
        "SELECT DISTINCT Target FROM Call WHERE Caller = ? AND Island IS NOT NULL", (crossing,))]
    db.close()
    inside = 0
    for target in targets:
        for image in (store / "Images").glob("*.db"):
            if image.name.endswith(".HexRays.db"):
                continue
            other = sqlite3.connect(f"file:{image}?mode=ro", uri=True)
            try:
                hit = other.execute("SELECT 1 FROM Segment WHERE ? >= Address "
                                    "AND ? < Address + Size LIMIT 1", (target, target)).fetchone()
            except sqlite3.Error:
                hit = None
            other.close()
            if hit:
                inside += 1
                break

    named = re.search(r"layer 2: (\d+) of (\d+) call island", out.stdout)
    if not named:
        failures.append(f"3b: fn printed no island accounting: {out.stdout[:400]!r}")
    else:
        if int(named.group(2)) != len(targets):
            failures.append(f"3b: fn counted {named.group(2)} islands where the store holds "
                            f"{len(targets)}")
        if int(named.group(1)) > inside:
            failures.append(f"3b: fn claims {named.group(1)} named, but only {inside} of the "
                            f"targets fall inside an indexed image")
    # Whatever it could not name has to say WHY, and the reason has to be the true one.
    outside = len(targets) - inside
    if outside and "outside the" not in out.stdout:
        failures.append(f"3b: {outside} target(s) are outside the indexed images and fn did not "
                        f"say so: {out.stdout[:400]!r}")
    if outside == 0 and inside and "outside the" in out.stdout:
        failures.append("3b: fn blamed the corpus for targets that are inside it")

    # The substitution itself, on an image whose targets ARE indexed: a resolved call must print
    # its symbol instead of the placeholder. DesignLibrary calls SwiftUICore, both indexed.
    design = run(["0x24053f798"])
    if "_$s" not in design.stdout:
        failures.append("3b: a call into an indexed image was not replaced by its target's "
                        f"symbol: {design.stdout[:400]!r}")
    for line in design.stdout.splitlines():
        if line.startswith("layer 2:"):
            continue
        if "MEMORY[0x248" in line:
            failures.append(f"3b: an island layer 1 resolved is still printed raw: {line!r}")
            break

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
