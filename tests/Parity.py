#!/usr/bin/env python3
"""Layer-1 parity: every Sherlock fact checked against the Python instrument that produces it.

Five checks, PASS/FAIL on their own line. Exit 1 on any FAIL, 2 when the store or the corpus
is missing -- a check that could not run is never silently a pass, the same discipline as
every other instrument in this repository.
"""
from __future__ import annotations

import argparse
import re
import sqlite3
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "References" / "scripts"))

import target  # noqa: E402
from dsc_reader import SplitCache  # noqa: E402
from verdict import is_function_start, need_corpus  # noqa: E402
import symbols  # noqa: E402
import xisland  # noqa: E402
import litref  # noqa: E402

DESIGN_LIBRARY  = "/System/Library/PrivateFrameworks/DesignLibrary.framework/Versions/A/DesignLibrary"
SWIFTUICORE     = "/System/Library/Frameworks/SwiftUICore.framework/Versions/A/SwiftUICore"
RESOLVE_LAYERS  = 0x2406780f0


def image_db(store: Path, catalog: sqlite3.Connection, path: str) -> sqlite3.Connection:
    row = catalog.execute("SELECT Name FROM Image WHERE Path = ?", (path,)).fetchone()
    if row is None:
        raise RuntimeError("no Image row for %s -- build it first" % path)
    return sqlite3.connect(str(store / "Images" / row[0]))


def check_owner(sherlock: Path, cache_dir: Path, store: Path, build: str) -> bool:
    ok = True
    for address in (0x240622d98, 0x27c198c20):
        py = subprocess.run([sys.executable, str(REPO / "References" / "scripts" / "dsc_reader.py"),
                             "--build", build, "--who", "0x%x" % address], capture_output=True, text=True)
        sh = subprocess.run([str(sherlock), "q", "0x%x" % address, "--cache", str(cache_dir), "--store", str(store)],
                            capture_output=True, text=True)
        py_hit = "DesignLibrary" in py.stdout
        sh_hit = "DesignLibrary" in sh.stdout
        got = py_hit and sh_hit
        print("%-4s owner 0x%x -- dsc_reader %s, Sherlock %s"
              % ("PASS" if got else "FAIL", address, "DesignLibrary" if py_hit else "?",
                 "DesignLibrary" if sh_hit else "?"))
        ok = ok and got
    return ok


def check_function_starts(cache_dir: Path, store: Path, catalog: sqlite3.Connection) -> bool:
    python_starts = set(symbols.Functions(cache_dir)._load(DESIGN_LIBRARY))
    db = image_db(store, catalog, DESIGN_LIBRARY)
    sherlock_starts = {row[0] for row in db.execute("SELECT Address FROM Function")}
    ok = python_starts == sherlock_starts
    print("%-4s function starts  python %d  sherlock %d"
          % ("PASS" if ok else "FAIL", len(python_starts), len(sherlock_starts)))
    return ok


def check_symbols(cache_dir: Path, store: Path, catalog: sqlite3.Connection) -> bool:
    python_symbols = symbols.Symbols(cache_dir)._load(SWIFTUICORE)  # {name: address}
    db = image_db(store, catalog, SWIFTUICORE)
    sherlock_rows = list(db.execute("SELECT N.Text, S.Address FROM Symbol S JOIN Name N ON N.Id = S.Name"))
    sherlock_by_name: dict[str, set[int]] = {}
    for name, addr in sherlock_rows:
        if name.startswith("_$s"):
            sherlock_by_name.setdefault(name, set()).add(addr)

    disagree = sum(1 for name, addr in sherlock_by_name.items()
                   if name in python_symbols and python_symbols[name] not in addr)
    sherlock_only = sum(1 for name in sherlock_by_name if name not in python_symbols)
    python_only = sum(1 for name in python_symbols if name.startswith("_$s") and name not in sherlock_by_name)
    ok = disagree == 0
    print("%-4s symbols  disagree %d  sherlock-only %d  ipsw-only %d"
          % ("PASS" if ok else "FAIL", disagree, sherlock_only, python_only))
    return ok


def check_islands(cache: SplitCache, cache_dir: Path, store: Path, catalog: sqlite3.Connection) -> bool:
    rng = is_function_start(cache_dir, DESIGN_LIBRARY, RESOLVE_LAYERS)
    if rng is None:
        print("FAIL islands  0x%x is not a function start" % RESOLVE_LAYERS)
        return False
    hits, _n, _expected = xisland.scan(cache, "DesignLibrary", {}, fn=rng)
    python_edges = {(site, isl, r) for site, isl, r, _name in hits if r is not None}
    db = image_db(store, catalog, DESIGN_LIBRARY)
    sherlock_edges = {(site, island, tgt) for site, island, tgt, via in
                      db.execute("SELECT Site, Island, Target, Via FROM Call WHERE Caller = ? AND Via != 'Direct'",
                                (RESOLVE_LAYERS,))}
    ok = python_edges == sherlock_edges
    print("%-4s islands  python %d  sherlock %d" % ("PASS" if ok else "FAIL", len(python_edges), len(sherlock_edges)))
    return ok


def check_literals(cache: SplitCache, store: Path, catalog: sqlite3.Connection) -> bool:
    path, hdr = litref.resolve_image(cache, "SystemBannerUI")
    tva, tsz = cache.sections(hdr)["__TEXT.__text"]
    insns, _coverage = litref.decode(cache.read(tva, tsz), tva)
    python_hits = {(pc, addr, kind) for pc, addr, kind in litref.readers(insns)}
    db = image_db(store, catalog, path)
    sherlock_hits = {(site, tgt, kind) for site, tgt, kind in db.execute("SELECT Site, Target, Kind FROM LiteralRef")}
    ok = python_hits == sherlock_hits
    print("%-4s literals  python %d  sherlock %d" % ("PASS" if ok else "FAIL", len(python_hits), len(sherlock_hits)))
    return ok


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--sherlock", required=True, type=Path)
    ap.add_argument("--store", required=True, type=Path)
    ap.add_argument("--build", choices=target.BUILDS, default=target.TARGET)
    args = ap.parse_args()

    if not (args.store / "Catalog.db").is_file():
        print("NOT VERIFIED: %s carries no Catalog.db -- run Sherlock build facts first" % args.store)
        return 2
    cache_dir = need_corpus(args.build)
    cache = SplitCache(cache_dir)
    catalog = sqlite3.connect(str(args.store / "Catalog.db"))

    checks = [
        check_owner(args.sherlock, cache_dir, args.store, args.build),
        check_function_starts(cache_dir, args.store, catalog),
        check_symbols(cache_dir, args.store, catalog),
        check_islands(cache, cache_dir, args.store, catalog),
        check_literals(cache, args.store, catalog),
    ]
    return 0 if all(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
