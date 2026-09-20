#!/usr/bin/env python3
# Sherlock -- tests/Sherlock/HexRaysGrepGate.py
# `grep` is the one query whose ZERO is dangerous, and this gate is about the zero rather than
# about the hits. Four ways this search can be silent, and they are indistinguishable in the
# output unless it names them:
#
#   - an image has no layer 2, so it was never opened;
#   - a limit stopped the walk before the search reached an image;
#   - a function's decompilation failed, so there is no text inside it to match;
#   - a call leaves the image into something the store does not index, so it carries no name at
#     all -- the Swift and ObjC runtimes above all, which are not among the towers.
#
# A name defined in another INDEXED image used to belong on that list and no longer does: the
# search resolves it through the island layer 1 stored beside the target, which case 6 holds.
# A reader who is not told reads any of these as "the pattern is not there". The positive control
# is the first case below: a term that IS in the corpus, so the instrument is shown to find what
# it can find before any of its silences are read as evidence.
import os
import re
import subprocess
import sys

cli = os.environ["SHERLOCK_CLI"]
store = os.environ["SHERLOCK_STORE"]

PRESENT = "PocketLayerDelegate"  # +[_DLPocketLayerDelegate sharedLayerDelegate], DesignLibrary
ABSENT = "ZzQqNotInAnyPseudocode"

failures = []


def run(*args):
    out = subprocess.run([cli, "grep", *args, "--store", store],
                         capture_output=True, text=True, timeout=1800)
    return out.returncode, out.stdout + out.stderr


# 1. The positive control. Without it every silence below is unreadable: an instrument that finds
#    nothing anywhere is broken, not informative.
code, found = run(PRESENT, "--images", "DesignLibrary")
print(found)
if code != 0 or "verdict: FOUND" not in found:
    failures.append(f"positive control: {PRESENT} was not found in DesignLibrary (exit {code})")
if "DesignLibrary 0x" not in found:
    failures.append("positive control: a hit does not carry its image and address")

# 2. The caveat that survives resolution, on EVERY search and not only an empty one: a call into
#    an image the store does not index has no symbol to match, however many times it is made.
if "outside the" not in found:
    failures.append("a search does not declare that a call into an unindexed image has no name "
                    "to match")

# 3. An honest zero. The pattern is absent, and the verdict must be EMPTY rather than an error --
#    but the output has to carry what was not looked at alongside it.
code, empty = run(ABSENT, "--images", "DesignLibrary")
print(empty)
if "verdict: EMPTY" not in empty:
    failures.append(f"an absent pattern did not read as EMPTY: {empty.strip()[-200:]}")
if "searched" not in empty:
    failures.append("an empty result does not say how much was searched")

# 4. Images with no layer 2 are named, with the command that would cover them. A zero over the
#    whole corpus while half of it was never decompiled is the failure this line prevents.
code, whole = run(ABSENT, "--limit", "1")
print(whole)
if "not searched:" not in whole:
    failures.append("a corpus-wide search does not name the images that have no layer 2")
if "build hexrays" not in whole:
    failures.append("the images with no layer 2 are named without the command that builds them")

# 5. A limit stops the walk, and the images it never reached are a DIFFERENT gap from the images
#    with no layer 2. Reporting only the second makes the first invisible, and the coverage pair
#    must count every selected image rather than only the visited ones.
code, capped = run(PRESENT, "--limit", "1")
print(capped)
if "stopped at 1 hit" not in capped:
    failures.append("a truncated search does not say it was truncated")
if "not reached:" not in capped:
    failures.append("a truncated search does not name the images the limit stopped it before")
# Not a fixed pair: the corpus differs per machine. The invariant is that the denominator counts
# what the search SELECTED, so a walk cut short still reports everything it owed an answer for.
verdict = next((line for line in capped.splitlines() if line.startswith("verdict:")), "")
numbers = verdict.rsplit(" ", 1)[-1].split("/") if "/" in verdict else []
if len(numbers) != 2 or not numbers[0].isdigit() or not numbers[1].isdigit():
    failures.append(f"the verdict carries no image coverage pair: {verdict!r}")
elif int(numbers[1]) <= int(numbers[0]):
    failures.append(f"a truncated search counts only the images it visited ({verdict.strip()}): "
                    f"the denominator has to carry the ones it never reached")

# 6. A name defined in ANOTHER image. The carved slice records such a call as MEMORY[<island>]
#    with no symbol on it, so the stored text cannot contain the name -- this case proves the
#    search reaches it anyway, through the island layer 1 stored beside the target. The pattern
#    is a fragment of a SwiftUICore mangled symbol; what proves it was NOT simply present in the
#    stored text is the cross-image counter in 6b, not an assumption about the corpus.
CROSS = "ControlSizeOMa"

code, found = run(CROSS, "--images", "DesignLibrary", "--limit", "5")
print(found)
if "verdict: FOUND" not in found:
    failures.append(f"6: {CROSS} is another image's symbol reached through an island and the "
                    f"search did not find it: {found.strip()[-300:]!r}")
if "_$s7SwiftUI11ControlSizeOMa" not in found:
    failures.append("6: the hit printed the island's address instead of the target's symbol")

# 6b. The one number that separates "the pattern was already in the stored text" from "the
#     pattern was reached through layer 1". Without it a reader cannot tell which happened, and
#     case 6 above would pass on a corpus where the name happened to appear natively.
crossing = re.search(r"(\d+) hit\(s\) matched a name the carved slice does not carry", found)
if not crossing:
    failures.append(f"6b: the search does not say how many hits came from a resolved island: "
                    f"{found.strip()[-300:]!r}")
elif int(crossing.group(1)) == 0:
    failures.append("6b: every hit came from the stored text, so this case proved nothing about "
                    "cross-image resolution")

# 6c. What resolution does NOT reach, stated on every run: the store indexes the towers, not the
#     cache's thousands of images, so a call into the Swift or ObjC runtime still has no name.
if "outside the" not in found:
    failures.append("6c: the search does not declare that a call into an unindexed image still "
                    "has no name to match")

if failures:
    for failure in failures:
        print(f"FAIL: {failure}")
    sys.exit(1)
print("OK: grep finds what it can, and names every image and function it could not look inside")
