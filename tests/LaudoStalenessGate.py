#!/usr/bin/env python3
"""laudo's own staleness gate (decision 8): builds Documents.db over a throwaway one-laudo
scratch repo, then edits that laudo's content and forces its mtime forward, and asserts
`laudo <slug> §n` now exits 2 (NOT VERIFIED) instead of serving text that no longer matches
the file on disk -- the mutation that proves the per-file stamp actually bites.

exit 0 = PASS  1 = FAIL  2 = NOT VERIFIED
python tests/Sherlock/LaudoStalenessGate.py <Sherlock.exe> <scratch dir>
"""
import os
import shutil
import subprocess
import sys
import time

LAUDO_TEXT = """## §1 A throwaway section

Some prose that never changes... until the mutation does.
"""


def run(sherlock, scratch, *args):
    return subprocess.run(
        [sherlock, *args, "--repo", scratch, "--documents", os.path.join(scratch, "Documents.db")],
        capture_output=True, text=True)


def write_git_head(scratch):
    # BuildDocuments resolves HEAD by reading .git files directly (GitHead.cpp), never by
    # shelling out to git -- a loose ref under refs/heads is enough, no real commit needed.
    git_dir = os.path.join(scratch, ".git")
    os.makedirs(os.path.join(git_dir, "refs", "heads"), exist_ok=True)
    with open(os.path.join(git_dir, "HEAD"), "w", encoding="utf-8") as fh:
        fh.write("ref: refs/heads/main\n")
    with open(os.path.join(git_dir, "refs", "heads", "main"), "w", encoding="utf-8") as fh:
        fh.write("0" * 40 + "\n")


def main():
    sherlock, scratch = sys.argv[1], sys.argv[2]
    shutil.rmtree(scratch, ignore_errors=True)
    os.makedirs(os.path.join(scratch, "docs", "re"), exist_ok=True)
    os.makedirs(os.path.join(scratch, "docs", "concepts"), exist_ok=True)
    os.makedirs(os.path.join(scratch, "Source"), exist_ok=True)
    write_git_head(scratch)
    laudo_path = os.path.join(scratch, "docs", "re", "staleness-sample.md")
    with open(laudo_path, "w", encoding="utf-8") as fh:
        fh.write(LAUDO_TEXT)

    build = run(sherlock, scratch, "build", "docs")
    if build.returncode != 0:
        print("NOT VERIFIED: build docs failed:", build.stdout, build.stderr)
        return 2

    fresh = run(sherlock, scratch, "laudo", "staleness-sample", "§1")
    if fresh.returncode != 0 or "throwaway section" not in fresh.stdout:
        print("FAIL: laudo on a freshly built store should succeed:", fresh.stdout)
        return 1
    print("PASS: laudo serves the fresh section")

    # Edit the file AND force its mtime forward, so the change is visible under any
    # filesystem's timestamp resolution (some are 1-2 seconds coarse).
    time.sleep(1.1)
    with open(laudo_path, "a", encoding="utf-8") as fh:
        fh.write("\nAn edit Documents.db never saw.\n")

    stale = run(sherlock, scratch, "laudo", "staleness-sample", "§1")
    if stale.returncode != 2:
        print("FAIL: laudo should exit 2 (NOT VERIFIED) once the file changed underneath the store, got",
              stale.returncode, stale.stdout)
        return 1
    print("PASS: laudo exits 2 once the backing file changed after the build")

    shutil.rmtree(scratch, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
