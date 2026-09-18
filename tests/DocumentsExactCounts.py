#!/usr/bin/env python3
"""Exact-count gate: Documents.db's Section/Seal totals must equal the numbers
measure_document_counts.py just measured -- see that script's header for how they are
re-baselined. Also checks two positive controls end to end: the DesignLibrary 13-layer
table's citations, and one clean [BIN] seal with two addresses in its own segment.

exit 0 = PASS  1 = FAIL  2 = NOT VERIFIED (Documents.db missing)
python tests/Sherlock/DocumentsExactCounts.py <Documents.db> <re-sections> <concept-sections> <bin-seals>
"""
import sqlite3
import sys
from pathlib import Path


def main():
    db_path, re_sections, concept_sections, bin_seals = (
        sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]))
    if not Path(db_path).is_file():
        print("NOT VERIFIED: %s is missing" % db_path)
        return 2
    conn = sqlite3.connect(db_path)
    ok = True

    for label, sql, expected in (
        ("docs/re sections", "SELECT COUNT(*) FROM Section WHERE File LIKE 'docs/re/%'", re_sections),
        ("docs/concepts sections", "SELECT COUNT(*) FROM Section WHERE File LIKE 'docs/concepts/%'",
         concept_sections),
        ("[BIN] seals", "SELECT COUNT(*) FROM Seal WHERE Tag = 'BIN'", bin_seals),
    ):
        actual = conn.execute(sql).fetchone()[0]
        print(("PASS" if actual == expected else "FAIL") +
              ": %s (expected %d, got %d)" % (label, expected, actual))
        ok = ok and actual == expected

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
