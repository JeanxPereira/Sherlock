#!/usr/bin/env python3
"""Documents.db must agree with an independent walk of the corpus, FILE BY FILE.

What this gate proves is that two implementations of the same counting rules agree:
Sherlock's own C++ walk (Heading.cpp, SealExtractor.cpp, Builder.cpp) and the Python one
in measure_document_counts.py, which this script imports rather than duplicates. A
divergence names the FILE it happened in.

It used to compare three totals against three numbers pinned in CMakeLists.txt, and that
had to be re-baselined by hand -- and re-configured, and the whole suite re-run -- every
time a laudo landed, several times a day. A total is also the weaker invariant: two
errors that cancel pass it, and the file that changed is never named. Nothing here is
pinned now, and a new laudo needs no edit anywhere.

Also checks four positive controls end to end: the DesignLibrary 13-layer table's two
citations, and one clean [BIN] seal whose two addresses must each produce a row.

exit 0 = PASS  1 = FAIL  2 = NOT VERIFIED (Documents.db missing)
python tests/Sherlock/DocumentsExactCounts.py <Documents.db> <repo root>
"""
import os
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import measure_document_counts as walk  # noqa: E402

# How many disagreeing files to name before the list is cut. A divergence is either one
# file or a rule change that hits hundreds; the first few name it either way.
MAX_NAMED = 8


def compare(label, walked, stored):
    """True when the two agree. Prints the totals, and every file that differs."""
    files = sorted(set(walked) | set(stored))
    differ = [(f, walked.get(f, 0), stored.get(f, 0)) for f in files
              if walked.get(f, 0) != stored.get(f, 0)]
    total_walked, total_stored = sum(walked.values()), sum(stored.values())
    print(("PASS" if not differ else "FAIL") +
          ": %s (walk %d in %d file(s), store %d in %d file(s))"
          % (label, total_walked, len(walked), total_stored, len(stored)))
    for f, w, s in differ[:MAX_NAMED]:
        print("    %s: walk %d, store %d" % (f, w, s))
    if len(differ) > MAX_NAMED:
        print("    ... and %d more file(s)" % (len(differ) - MAX_NAMED))
    return not differ


def main():
    db_path, root = sys.argv[1], sys.argv[2]
    if not Path(db_path).is_file():
        print("NOT VERIFIED: %s is missing" % db_path)
        return 2
    conn = sqlite3.connect(db_path)
    ok = True

    def stored(sql):
        return {f: n for f, n in conn.execute(sql).fetchall()}

    ok = compare("docs/re sections", walk.re_sections_by_file(root),
                 stored("SELECT File, COUNT(*) FROM Section WHERE File LIKE 'docs/re/%' "
                        "GROUP BY File")) and ok
    ok = compare("docs/concepts sections", walk.concept_sections_by_file(root),
                 stored("SELECT File, COUNT(*) FROM Section WHERE File LIKE 'docs/concepts/%' "
                        "GROUP BY File")) and ok
    ok = compare("[BIN] seals", walk.bin_seals_by_file(root),
                 stored("SELECT File, COUNT(*) FROM Seal WHERE Tag = 'BIN' GROUP BY File")) and ok

    def has_row(sql, params, label):
        nonlocal ok
        found = conn.execute(sql, params).fetchone() is not None
        print(("PASS" if found else "FAIL") + ": " + label)
        ok = ok and found

    has_row(
        "SELECT 1 FROM Citation JOIN Section ON Section.Id = Citation.Section "
        "WHERE Citation.Address = ? AND Section.File = ? AND Section.Number = ? LIMIT 1",
        (0x27c198c20, "docs/re/2026-08-22-resolvelayers-recon.md", "7"),
        "0x27c198c20 cited by 2026-08-22-resolvelayers-recon.md §7")
    has_row(
        "SELECT 1 FROM Citation JOIN Section ON Section.Id = Citation.Section "
        "WHERE Citation.Address = ? AND Section.File = ? AND Section.Number = ? LIMIT 1",
        (0x27c198c20, "docs/re/2026-08-24-o-vidro-do-banner-provado.md", "7"),
        "0x27c198c20 cited by 2026-08-24-o-vidro-do-banner-provado.md §7")
    has_row(
        "SELECT 1 FROM Seal WHERE File = ? AND Line = ? AND Tag = 'BIN' AND Image = ? AND Address = ?",
        ("Source/AgentCanvasKit/include/AgentCanvasKit/SnippetSizeConstants.h", 6, "AgentCanvasKit", 0x22695fe48),
        "the clean SnippetSizeConstants.h:6 [BIN] control resolves its getter Address")
    has_row(
        "SELECT 1 FROM Seal WHERE File = ? AND Line = ? AND Tag = 'BIN' AND Image = ? AND Address = ?",
        ("Source/AgentCanvasKit/include/AgentCanvasKit/SnippetSizeConstants.h", 6, "AgentCanvasKit", 0x22695fcf4),
        "the SAME clean control ALSO resolves its initializer Address -- a first-address-only "
        "extractor drops this row entirely, so this is the two-address control's second half")

    row_count = conn.execute(
        "SELECT COUNT(*) FROM Seal WHERE File = ? AND Line = ? AND Tag = 'BIN'",
        ("Source/AgentCanvasKit/include/AgentCanvasKit/SnippetSizeConstants.h", 6)).fetchone()[0]
    print(("PASS" if row_count == 2 else "FAIL") +
          ": SnippetSizeConstants.h:6 seals exactly 2 rows, one per address (got %d)" % row_count)
    ok = ok and row_count == 2

    conn.close()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
