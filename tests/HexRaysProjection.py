#!/usr/bin/env python3
# Sherlock -- tests/Sherlock/HexRaysProjection.py
# The phase-3 gate: what layer 2 costs per byte of binary and per function, and what those two
# rates project over the whole cache. Phase 4 starts only after a person reads this.
#
# It measures, it does not decide. A projection that refuses phase 4 -- because the pseudocode
# does not fit the disk, or the hours do not fit the week -- is a successful run of this gate.
#
# Two traps, both already paid for once:
#   - Never sample by address order. The low addresses of an image are stubs, and a rate measured
#     on them reads three to four times too fast. This reads whole images only.
#   - An earlier session measured DesignLibrary before any of this existed, at 0.013 s per
#     function on a random sample. That is the order of magnitude to land near; a disagreement by
#     an order of magnitude is a bug in the measurement, not a discovery.
import os
import sqlite3
import sys
from pathlib import Path

SAMPLED_RATE = 0.013  # s/function, the earlier session's random sample of DesignLibrary
GIB = 1024.0 ** 3

# The spec names five tower images. Running on fewer is a deliberate, recorded reduction, never a
# default: SHERLOCK_MIN_IMAGES has to be lowered by hand, and the report says on its face how many
# images its rates come from and which of the five are absent.
SPEC_IMAGES = ["DesignLibrary", "ContactsUICore", "QuartzCore", "SwiftUICore", "AppKit"]

store = Path(os.environ["SHERLOCK_STORE"]) / "Images"
cache_dir = Path(os.environ["SHERLOCK_CACHE"])
dylibs = Path(os.environ["SHERLOCK_CORPUS_DYLIBS"])
report_path = Path(os.environ.get("SHERLOCK_PROJECTION", str(store.parent / "hexrays-projection.txt")))
workers = int(os.environ.get("SHERLOCK_WORKERS", "2"))

rows = []
for db_path in sorted(store.glob("*.HexRays.db")):
    name = db_path.name[: -len(".HexRays.db")]
    db = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    total, ok, seconds, lines = db.execute(
        "SELECT count(*), sum(Status = 'Ok'), sum(Seconds), sum(Lines) FROM Decompilation").fetchone()
    ida = db.execute("SELECT Value FROM Meta WHERE Key = 'IdaVersion'").fetchone()
    db.close()
    image = dylibs / name
    if not image.is_file():
        print(f"note: {name} has a store but no extracted image, so its bytes-per-byte is unknown")
        continue
    rows.append({
        "name": name,
        "image_bytes": image.stat().st_size,
        "store_bytes": db_path.stat().st_size,
        "functions": total or 0,
        "decompiled": ok or 0,
        "seconds": seconds or 0.0,
        "lines": lines or 0,
        "ida": ida[0] if ida else "unknown",
    })

minimum = int(os.environ.get("SHERLOCK_MIN_IMAGES", "5"))
if len(rows) < minimum:
    print(f"NOT VERIFIED: {len(rows)} image(s) exported, and this gate reads {minimum}. Export the "
          f"rest with: Sherlock build hexrays --towers --resume --store {store.parent}")
    sys.exit(2)

absent = [name for name in SPEC_IMAGES if name not in {r["name"] for r in rows}]

# Every byte of the cache is a byte layer 2 would have to read, so the cache's own size is what
# the two rates scale by.
cache_bytes = sum(f.stat().st_size for f in cache_dir.iterdir() if f.is_file())

image_bytes = sum(r["image_bytes"] for r in rows)
store_bytes = sum(r["store_bytes"] for r in rows)
functions = sum(r["functions"] for r in rows)
decompiled = sum(r["decompiled"] for r in rows)
seconds = sum(r["seconds"] for r in rows)

bytes_per_byte = store_bytes / image_bytes
seconds_per_function = seconds / functions if functions else 0.0
functions_per_byte = functions / image_bytes

projected_functions = functions_per_byte * cache_bytes
projected_hours = projected_functions * seconds_per_function / 3600.0
projected_store_gib = cache_bytes * bytes_per_byte / GIB

free_bytes = 0
try:
    import shutil
    free_bytes = shutil.disk_usage(store).free
except OSError:
    pass

out = []
out.append(f"Sherlock layer 2 -- the phase-3 gate, from {len(rows)} image(s)")
if absent:
    out.append(f"REDUCED: the spec names five, and {', '.join(absent)} are absent. Every rate below "
               f"comes from the images that are here, and the projection is only as good as they "
               f"represent the cache.")
out.append("")
out.append(f"{'image':<18}{'image MB':>10}{'store MB':>10}{'B/B':>7}{'functions':>11}"
           f"{'decompiled':>12}{'s/function':>12}")
for r in rows:
    out.append(f"{r['name']:<18}{r['image_bytes'] / 1e6:>10.2f}{r['store_bytes'] / 1e6:>10.2f}"
               f"{r['store_bytes'] / r['image_bytes']:>7.2f}{r['functions']:>11}{r['decompiled']:>12}"
               f"{r['seconds'] / r['functions'] if r['functions'] else 0:>12.4f}")
out.append("")
out.append(f"totals: {image_bytes / 1e6:.2f} MB of image, {store_bytes / 1e6:.2f} MB of store, "
           f"{functions} functions, {decompiled} decompiled, {seconds / 3600:.2f} h of decompilation")
out.append(f"rates: {bytes_per_byte:.2f} stored bytes per binary byte, "
           f"{seconds_per_function:.4f} s per function, {functions_per_byte * 1e6:.0f} functions per MB")
out.append(f"ida: {rows[0]['ida']}")
out.append("")
out.append(f"cache: {cache_bytes / GIB:.2f} GiB in {cache_dir}")
out.append(f"projection: {projected_functions / 1e6:.2f} M functions, "
           f"{projected_hours:.1f} h in series, {projected_hours / workers:.1f} h at {workers} workers, "
           f"{projected_store_gib:.1f} GiB of pseudocode")
if free_bytes:
    out.append(f"free on the store's disk: {free_bytes / GIB:.1f} GiB "
               f"({'fits' if projected_store_gib < free_bytes / GIB else 'DOES NOT FIT'})")
out.append("")
out.append(f"against the earlier random sample of {SAMPLED_RATE} s/function: this whole-image rate "
           f"is {seconds_per_function / SAMPLED_RATE:.1f}x it")
if absent:
    out.append(f"absent from this projection: {', '.join(absent)} -- the largest images of the five, "
               f"so the analysis time and the memory a large image needs are NOT in these numbers.")
out.append("Read this before starting phase 4. The gate measures; the decision is a person's.")

text = "\n".join(out)
print(text)
report_path.parent.mkdir(parents=True, exist_ok=True)
report_path.write_text(text + "\n", encoding="utf-8")
print(f"\nwritten to {report_path}")

# The gate fails only on a measurement that cannot be believed, never on a number someone will
# not like: a rate an order of magnitude off the earlier one means the run measured the wrong
# thing, and a store smaller than its own images means nothing was stored.
if not (0.1 <= seconds_per_function / SAMPLED_RATE <= 10.0):
    sys.exit(f"FAIL: {seconds_per_function:.4f} s/function is off the earlier {SAMPLED_RATE} by more "
             f"than an order of magnitude, so one of the two measured the wrong thing")
if decompiled == 0:
    sys.exit("FAIL: no function decompiled across the images read")
