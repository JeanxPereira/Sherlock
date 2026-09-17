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
SYSTEM_BANNER_UI = "/System/Library/PrivateFrameworks/SystemBannerUI.framework/Versions/A/SystemBannerUI"
REQUIRED_IMAGES = (DESIGN_LIBRARY, SWIFTUICORE, SYSTEM_BANNER_UI)
RESOLVE_LAYERS  = 0x2406780f0


class StoreUnavailable(RuntimeError):
    """A required part of the Sherlock store is absent or cannot be inspected."""


def readonly_db(path: Path) -> sqlite3.Connection:
    if not path.is_file():
        raise StoreUnavailable("missing database %s" % path)
    try:
        return sqlite3.connect(path.resolve().as_uri() + "?mode=ro", uri=True)
    except sqlite3.Error as error:
        raise StoreUnavailable("cannot open database %s: %s" % (path, error)) from error


def image_db_path(store: Path, catalog: sqlite3.Connection, path: str) -> Path:
    try:
        row = catalog.execute("SELECT Name FROM Image WHERE Path = ?", (path,)).fetchone()
    except sqlite3.Error as error:
        raise StoreUnavailable("cannot resolve Image row for %s: %s" % (path, error)) from error
    if row is None:
        raise StoreUnavailable("Catalog.db has no Image row for %s" % path)
    db_path = store / "Images" / row[0]
    if not db_path.is_file():
        raise StoreUnavailable("missing image database %s for %s" % (db_path, path))
    return db_path


def image_db(store: Path, catalog: sqlite3.Connection, path: str) -> sqlite3.Connection:
    return readonly_db(image_db_path(store, catalog, path))


def check_owner(sherlock: Path, cache_dir: Path, store: Path, build: str) -> bool:
    ok = True
    for address, segment in ((0x240622d98, "__TEXT"), (0x27c198c20, "__AUTH_CONST")):
        py = subprocess.run([sys.executable, str(REPO / "References" / "scripts" / "dsc_reader.py"),
                             "--build", build, "--who", "0x%x" % address], capture_output=True, text=True)
        sh = subprocess.run([str(sherlock), "q", "0x%x" % address, "--cache", str(cache_dir), "--store", str(store)],
                            capture_output=True, text=True)
        py_hit = py.returncode == 0 and "DesignLibrary" in py.stdout
        owner_lines = [line for line in sh.stdout.splitlines() if line.startswith("owner: ")]
        sh_hit = (sh.returncode in (0, 3) and len(owner_lines) == 1 and
                  owner_lines[0].endswith("/DesignLibrary " + segment))
        got = py_hit and sh_hit and "NOT VERIFIED" not in sh.stdout
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

    python_swift = {name: addr for name, addr in python_symbols.items() if name.startswith("_$s")}
    disagree = sum(1 for name, addr in python_swift.items()
                   if addr not in sherlock_by_name.get(name, set()))
    sherlock_only = sum(1 for name in sherlock_by_name if name not in python_symbols)
    python_only = sum(1 for name in python_swift if name not in sherlock_by_name)
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

    catalog_path = args.store / "Catalog.db"
    try:
        catalog = readonly_db(catalog_path)
        for image_path in REQUIRED_IMAGES:
            image_db_path(args.store, catalog, image_path)
    except StoreUnavailable as error:
        print("NOT VERIFIED: %s -- run Sherlock build facts first" % error)
        return 2
    cache_dir = need_corpus(args.build)
    cache = SplitCache(cache_dir)

    checks = [
        check_owner(args.sherlock, cache_dir, args.store, args.build),
        check_function_starts(cache_dir, args.store, catalog),
        check_symbols(cache_dir, args.store, catalog),
        check_islands(cache, cache_dir, args.store, catalog),
        check_literals(cache, args.store, catalog),
    ]
    catalog.close()
    return 0 if all(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
