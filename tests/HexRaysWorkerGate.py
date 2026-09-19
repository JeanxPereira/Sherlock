#!/usr/bin/env python3
# Sherlock -- tests/Sherlock/HexRaysWorkerGate.py
# The worker's output over one real image: the repository's own control address is covered, every
# row carries a status, and no row claims success with nothing in it.
#
# The export itself is the Sherlock.HexRaysExport fixture, which resumes; this gate only reads the
# store it left, so the suite pays for the decompilation once.
import os
import sqlite3
import sys

CONTROL = 0x27C198C20  # the DesignLibrary layer table -- the repository's own positive control

store = os.path.join(os.environ["SHERLOCK_STORE"], "Images", "DesignLibrary.HexRays.db")
if not os.path.isfile(store):
    sys.exit(f"NOT VERIFIED: {store} does not exist -- the export fixture did not run")

db = sqlite3.connect(f"file:{store}?mode=ro", uri=True)

kind, version = db.execute(
    "SELECT (SELECT Value FROM Meta WHERE Key = 'Kind'), "
    "(SELECT Value FROM Meta WHERE Key = 'SchemaVersion')").fetchone()
if kind != "HexRays":
    sys.exit(f"FAIL: the store says it is a '{kind}' store")

ida = db.execute("SELECT Value FROM Meta WHERE Key = 'IdaVersion'").fetchone()
if not ida or not ida[0]:
    sys.exit("FAIL: the store does not record which IDA produced its rows, and two versions "
             "decompile the same function differently")

total, = db.execute("SELECT count(*) FROM Decompilation").fetchone()
ok, = db.execute("SELECT count(*) FROM Decompilation WHERE Status = 'Ok'").fetchone()
blank, = db.execute("SELECT count(*) FROM Decompilation WHERE Status = 'Ok' AND Lines = 0").fetchone()
empty, = db.execute("SELECT count(*) FROM Decompilation WHERE Status = 'Ok' AND Pseudocode IS NULL").fetchone()
nostatus, = db.execute("SELECT count(*) FROM Decompilation WHERE Status NOT IN "
                       "('Ok', 'Timeout', 'Failed')").fetchone()

# The image reads as 11 214 functions. A store an order of magnitude smaller is the worker giving
# up early, which a bare "it ran" would not catch.
if total < 10000:
    sys.exit(f"FAIL: only {total} rows for an image that reads as 11 214 functions")
if ok == 0:
    sys.exit("FAIL: no function decompiled -- the handshake or the licence, not the image")
if blank:
    sys.exit(f"FAIL: {blank} rows claim Ok with zero lines")
if empty:
    sys.exit(f"FAIL: {empty} rows claim Ok with no pseudocode stored")
if nostatus:
    sys.exit(f"FAIL: {nostatus} rows carry a status outside Ok/Timeout/Failed")

owner = db.execute(
    "SELECT Function, Lines FROM Decompilation WHERE Function <= ? ORDER BY Function DESC LIMIT 1",
    (CONTROL,)).fetchone()
if owner is None:
    sys.exit(f"FAIL: no function at or below {CONTROL:#x} -- the store does not reach the control")

# IdaName is what layer 2 adds over layer 1's symbol table; an empty table means the pass that
# fills it was skipped, which nothing else here would notice.
names, = db.execute("SELECT count(*) FROM IdaName").fetchone()
if names == 0:
    sys.exit("FAIL: IdaName is empty, so no name reached the store")

# The worker prints store-bytes, and a store measured while its own connection is still open
# reads 4096 -- one empty page -- for anything under SQLite's WAL autocheckpoint. The number is
# what a projection of the whole fill is built from, so a size that only happens to be right for
# large images is a number that lies exactly where it is cheapest to believe.
on_disk = os.path.getsize(store)
if on_disk < 64 * 1024:
    sys.exit(f"FAIL: the store is {on_disk} bytes on disk, which is an empty page, not "
             f"{ok} decompiled functions")

print(f"OK: {ok}/{total} decompiled, {names} names, control {CONTROL:#x} under {owner[0]:#x}, "
      f"ida {ida[0]}, {on_disk / 1e6:.2f} MB on disk")
