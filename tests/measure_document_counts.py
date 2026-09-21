#!/usr/bin/env python3
"""Walks the corpus and counts what Sherlock's Documents.db must contain: docs/re's
headings (one Section each, fence-aware AND indentation-aware, matching Heading.cpp's two
CommonMark rules exactly -- a fence closes only on a same-character run at least as long as
its opening one, and a 4-column-indented line is code, never a heading), docs/concepts' files
(one Section each, decision 3), and Source's [BIN] pairing count (lint_seals.py's own
blocks()+BIN, imported directly -- not re-implemented a second time).

This is the SECOND implementation of those rules on purpose. DocumentsExactCounts.py imports
the three functions below and compares them against the store file by file, so what the gate
proves is that two independent parsers agree -- not that a total matches a number somebody
typed. Nothing here is baselined and nothing needs updating when a laudo lands.

python tests/Sherlock/measure_document_counts.py <repo root>   # prints the three totals
"""
import glob
import os
import sys


def leading_indent_width(line):
    """CommonMark: a tab expands to the next multiple of four columns."""
    column = 0
    for ch in line:
        if ch == ' ':
            column += 1
        elif ch == '\t':
            column = (column // 4 + 1) * 4
        else:
            break
    return column


def count_headings(text):
    lines = text.split("\n")
    in_fence, fence_char, fence_len, count = False, None, 0, 0
    for ln in lines:
        stripped = ln.strip(" \t")
        if not in_fence:
            if stripped[:1] in ("`", "~"):
                ch = stripped[0]
                length = 0
                while length < len(stripped) and stripped[length] == ch:
                    length += 1
                if length >= 3:
                    in_fence, fence_char, fence_len = True, ch, length
                    continue
        else:
            if stripped[:1] == fence_char:
                length = 0
                while length < len(stripped) and stripped[length] == fence_char:
                    length += 1
                # closes only on the SAME character, a run at least as long as the
                # opening one, and nothing but whitespace after it (CommonMark) --
                # a shorter or differently charactered run inside is content.
                if length >= fence_len and stripped[length:].strip(" \t") == "":
                    in_fence = False
            continue
        if leading_indent_width(ln) >= 4:
            continue  # an indented code line (CommonMark), never a heading
        trimmed = ln.lstrip(" \t")
        level = 0
        while level < len(trimmed) and trimmed[level] == '#':
            level += 1
        if level == 0 or level > 6 or level >= len(trimmed) or trimmed[level] != ' ':
            continue
        count += 1
    return count


def re_sections_by_file(root):
    """{repo-relative path with forward slashes: heading count}."""
    out = {}
    for f in glob.glob(os.path.join(root, "docs", "re", "*.md")):
        if os.path.basename(f) == "README.md":
            continue
        rel = os.path.relpath(f, root).replace(os.sep, "/")
        out[rel] = count_headings(open(f, encoding="utf-8", errors="replace").read())
    return out


def concept_sections_by_file(root):
    """{repo-relative path: 1} -- one Section per file, decision 3."""
    out = {}
    for f in glob.glob(os.path.join(root, "docs", "concepts", "*.md")):
        if os.path.basename(f) == "index.md":
            continue
        out[os.path.relpath(f, root).replace(os.sep, "/")] = 1
    return out


def bin_seals_by_file(root):
    """{repo-relative path: row count}. One row PER ADDRESS in a BIN-tagged segment
    (SealExtractor.cpp's own count), not one per tag occurrence: a segment can carry more
    than one cache-shaped address (the SnippetSizeConstants.h:6 clean control seals two),
    and a segment with none still counts as one row, Address null.

    Extensions match DocumentIndex::HasSealExtension (Builder.h/.cpp) and SealDump.cpp's
    own walk exactly -- .h/.cpp/.hpp plus the shader extensions (.frag/.vert/.glsl) that
    carry real BIN-tagged seals outside Source/Platform/shaders too."""
    sys.path.insert(0, os.path.join(root, "References", "scripts"))
    import lint_seals
    out = {}
    for dirpath, dirs, files in os.walk(os.path.join(root, "Source")):
        dirs[:] = [d for d in dirs if d not in ("build", "lab", ".git")]
        for fn in files:
            if not fn.endswith((".h", ".cpp", ".hpp", ".frag", ".vert", ".glsl")):
                continue
            path = os.path.join(dirpath, fn)
            lines = open(path, encoding="utf-8", errors="replace").read().split("\n")
            rows = 0
            for _, blk in lint_seals.blocks(lines):
                text = "\n".join(blk)
                bin_matches = list(lint_seals.BIN.finditer(text))
                for idx, m in enumerate(bin_matches):
                    seg_end = bin_matches[idx + 1].start() if idx + 1 < len(bin_matches) else len(text)
                    addrs = list(lint_seals.ADDR.finditer(text, m.end(), seg_end))
                    rows += max(1, len(addrs))
            if rows:
                out[os.path.relpath(path, root).replace(os.sep, "/")] = rows
    return out


def main():
    root = sys.argv[1]
    print("SHERLOCK_DOCUMENTS_RE_SECTIONS", sum(re_sections_by_file(root).values()))
    print("SHERLOCK_DOCUMENTS_CONCEPT_SECTIONS", sum(concept_sections_by_file(root).values()))
    print("SHERLOCK_DOCUMENTS_BIN_SEALS", sum(bin_seals_by_file(root).values()))


if __name__ == "__main__":
    main()
