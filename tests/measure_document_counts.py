#!/usr/bin/env python3
"""Re-measures the three numbers tests/Sherlock/CMakeLists.txt pins for Sherlock.DocumentsExactCounts:
docs/re's heading count (one Section per heading, fence-aware AND indentation-aware, matching
Heading.cpp's two CommonMark rules exactly -- a fence closes only on a same-character run at
least as long as its opening one, and a 4-column-indented line is code, never a heading), docs/concepts'
file count (one Section per file, decision 3), and Source/'s [BIN] pairing count (lint_seals.py's
own blocks()+BIN, imported directly -- not re-implemented a second time).

Run this after a change to docs/re, docs/concepts or Source/'s seals grows or shrinks the
corpus, and copy its three printed numbers into tests/Sherlock/CMakeLists.txt's
SHERLOCK_DOCUMENTS_* set() lines BY HAND -- a deliberate re-baseline, never a build step.

python tests/Sherlock/measure_document_counts.py <repo root>
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


def main():
    root = sys.argv[1]
    sys.path.insert(0, os.path.join(root, "References", "scripts"))
    import lint_seals  # noqa: E402

    re_files = [f for f in glob.glob(os.path.join(root, "docs", "re", "*.md"))
               if os.path.basename(f) != "README.md"]
    re_sections = sum(count_headings(open(f, encoding="utf-8", errors="replace").read()) for f in re_files)

    concept_files = [f for f in glob.glob(os.path.join(root, "docs", "concepts", "*.md"))
                     if os.path.basename(f) != "index.md"]
    concept_sections = len(concept_files)  # one Section per file, decision 3

    # One row PER ADDRESS in a BIN-tagged segment (SealExtractor.cpp's own count, Seal.Tag = 'BIN'),
    # not one row per tag occurrence: a segment can carry more than one cache-shaped address (the
    # SnippetSizeConstants.h:6 clean control seals two), and a segment with none still counts as
    # one row, Address null.
    #
    # Extensions match DocumentIndex::HasSealExtension (Builder.h/.cpp) and SealDump.cpp's own
    # walk exactly -- .h/.cpp/.hpp plus the shader extensions (.frag/.vert/.glsl) that carry real
    # BIN-tagged seals outside Source/Platform/shaders too (QuartzCore, DesignLibrary, SwiftUICore).
    bin_seals = 0
    for dirpath, dirs, files in os.walk(os.path.join(root, "Source")):
        dirs[:] = [d for d in dirs if d not in ("build", "lab", ".git")]
        for fn in files:
            if not fn.endswith((".h", ".cpp", ".hpp", ".frag", ".vert", ".glsl")):
                continue
            lines = open(os.path.join(dirpath, fn), encoding="utf-8", errors="replace").read().split("\n")
            for _, blk in lint_seals.blocks(lines):
                text = "\n".join(blk)
                bin_matches = list(lint_seals.BIN.finditer(text))
                for idx, m in enumerate(bin_matches):
                    seg_end = bin_matches[idx + 1].start() if idx + 1 < len(bin_matches) else len(text)
                    addrs = list(lint_seals.ADDR.finditer(text, m.end(), seg_end))
                    bin_seals += max(1, len(addrs))

    print("SHERLOCK_DOCUMENTS_RE_SECTIONS", re_sections)
    print("SHERLOCK_DOCUMENTS_CONCEPT_SECTIONS", concept_sections)
    print("SHERLOCK_DOCUMENTS_BIN_SEALS", bin_seals)


if __name__ == "__main__":
    main()
