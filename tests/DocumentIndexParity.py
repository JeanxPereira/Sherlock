#!/usr/bin/env python3
"""BIN seal parity: every BIN-tagged seal Sherlock's SealExtractor finds under Source/ is checked
against lint_seals.py's own blocks()/BIN/ADDR objects, imported directly -- not re-implemented in
Python a second time, which would only prove two Python copies agree with each other. Only the BIN
tag is compared: lint_seals.py's own BIN regex is the only one that ever pairs an image and an
address with a tag (Measured facts, phase-2 plan) -- KIT/OBS/API address extraction is Sherlock's
own widening (decision 6) with no Python ground truth to compare against.

Deviation from the phase-2 plan: Task 6 (the Builder and the "build docs" CLI command) does not
exist yet, so this script cannot invoke Sherlock through Documents.db as the plan's own Step 6/7
assumed. Instead it drives SherlockSealDump, a small executable owned by this same task, which
runs ExtractSeals over every Source/**/*.h|.cpp|.hpp file and prints one TSV row per seal.

exit 0 = PASS  1 = FAIL (a divergence)  2 = NOT VERIFIED (Source/, the dump exe, or the comparison
itself is empty -- a parity check that compared nothing is never a silent pass)
python tests/Sherlock/DocumentIndexParity.py --dump <SherlockSealDump exe> --repo <repo root>
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path
from typing import Optional

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "References" / "scripts"))
import lint_seals  # noqa: E402

EXT = (".h", ".cpp", ".hpp", ".frag", ".vert", ".glsl")
SKIP_DIRS = {"build", "lab", ".git"}

Seal = tuple[str, int, str, Optional[int]]  # (relpath, line, image, address)


def files_scanned(source_dir: Path) -> set[str]:
    """Every file EXT would hand to the scanner -- the coverage denominator. Distinct from "files
    with at least one BIN hit": a directory can carry plenty of .h/.cpp/.frag/... that seal
    nothing at all, and printing only the files WITH a hit as "coverage" hides that this check
    never looked at the rest."""
    found: set[str] = set()
    for root, dirs, files in os.walk(source_dir):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for fn in files:
            if fn.endswith(EXT):
                found.add(os.path.join(root, fn))
    return found


def python_bin_seals(source_dir: Path, repo: Path) -> set[Seal]:
    """(relpath, line, image, address) for every BIN tag lint_seals.py's own blocks()/BIN/ADDR
    find, walking the exact same per-tag segmentation scan() uses (`bin_matches[idx + 1]` closes
    a seal's own segment) -- everything except the dsc.owner address-resolution step, which
    DocumentIndex has no cache dependency to reproduce and does not claim to (decision 6).

    One row PER ADDRESS in a segment, not just its first: a segment can carry more than one
    cache-shaped address (SnippetSizeConstants.h:6's own clean control seals two), and a seal row
    for only the first would leave every other address in that segment unrepresented on both sides
    of the comparison -- silently agreeing with a Sherlock that also only kept the first, which is
    exactly the bug this parity check exists to catch. A segment with no address still contributes
    one row, address None, matching the no-address negative control."""
    found: set[Seal] = set()
    for path in files_scanned(source_dir):
        rel = os.path.relpath(path, repo).replace("\\", "/")
        lines = open(path, encoding="utf-8", errors="replace").read().split("\n")
        for start, blk in lint_seals.blocks(lines):
            text = "\n".join(blk)
            bin_matches = list(lint_seals.BIN.finditer(text))
            for idx, m in enumerate(bin_matches):
                img = m.group(1) or ""
                seg_end = bin_matches[idx + 1].start() if idx + 1 < len(bin_matches) else len(text)
                addrs = [int(a.group(0), 16) for a in lint_seals.ADDR.finditer(text, m.end(), seg_end)]
                within = text.count("\n", 0, m.start())
                line_no = start + within
                if addrs:
                    for addr in addrs:
                        found.add((rel, line_no, img, addr))
                else:
                    found.add((rel, line_no, img, None))
    return found


def cpp_bin_seals(dump_exe: Path, repo: Path, source_dir: Path) -> set[Seal]:
    result = subprocess.run([str(dump_exe), str(repo), str(source_dir)], capture_output=True, text=True)
    if result.returncode not in (0,):
        print("NOT VERIFIED: SealDump exited %d\nstderr: %s" % (result.returncode, result.stderr))
        raise SystemExit(2)
    found: set[Seal] = set()
    for line in result.stdout.splitlines():
        parts = line.split("\t")
        if len(parts) != 5:
            continue
        rel, line_no, tag, image, address = parts
        if tag != "BIN":
            continue
        found.add((rel, int(line_no), image, int(address) if address else None))
    return found


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", required=True, help="path to the SherlockSealDump executable")
    ap.add_argument("--repo", required=True)
    args = ap.parse_args()

    repo = Path(args.repo)
    source_dir = repo / "Source"
    dump_exe = Path(args.dump)
    if not source_dir.is_dir():
        print("NOT VERIFIED: %s is missing" % source_dir)
        return 2
    if not dump_exe.is_file():
        print("NOT VERIFIED: %s (SealDump) was not built" % dump_exe)
        return 2

    py = python_bin_seals(source_dir, repo)
    try:
        cpp = cpp_bin_seals(dump_exe, repo, source_dir)
    except SystemExit as exit_code:
        return int(exit_code.code)

    total_scanned = files_scanned(source_dir)
    files_with_hits = {row[0] for row in py}
    print("coverage: %d BIN seals compared across %d files scanned (%d carry at least one)"
          % (len(py), len(total_scanned), len(files_with_hits)))
    if not py:
        print("NOT VERIFIED: 0 BIN seals compared -- Source/ has no BIN seals, or lint_seals.py "
              "could not read it; either is a reason to distrust a green run, never to print one")
        return 2

    missing = py - cpp  # lint_seals.py finds it, Sherlock does not
    extra = cpp - py    # Sherlock finds it, lint_seals.py does not
    if missing or extra:
        print("FAIL: %d seal(s) lint_seals.py finds that Sherlock does not, %d Sherlock finds that it does not"
              % (len(missing), len(extra)))
        for row in sorted(missing)[:10]:
            print("  missing:", row)
        for row in sorted(extra)[:10]:
            print("  extra:  ", row)
        return 1
    print("PASS: %d BIN seals agree with lint_seals.py" % len(py))
    return 0


if __name__ == "__main__":
    sys.exit(main())
