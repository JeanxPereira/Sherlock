#!/usr/bin/env python3
# Sherlock -- tests/Sherlock/HexRaysGrepGate.py
# `grep` is the one query whose ZERO is dangerous, and this gate is about the zero rather than
# about the hits. Four ways this search can be silent, and they are indistinguishable in the
# output unless it names them:
#
#   - an image has no layer 2, so it was never opened;
#   - a limit stopped the walk before the search reached an image;
#   - a function's decompilation failed, so there is no text inside it to match;
#   - a name defined in ANOTHER image cannot appear at all, because the carved slice reads its
#     calls as MEMORY[0x...] -- measured: ColorScheme.dark is initialized inside CampoUIInternal
#     at 0x22f4da22c and this search does not find the word.
#
# A reader who is not told reads any of these as "the pattern is not there". The positive control
# is the first case below: a term that IS in the corpus, so the instrument is shown to find what
# it can find before any of its silences are read as evidence.
import os
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

# 2. The cross-image caveat, on EVERY search and not only an empty one. It is the silence a reader
#    is least able to guess at, because the pattern may be called hundreds of times in the image.
if "another image is not findable here" not in found.lower():
    failures.append("a search does not declare that a name from another image cannot be found")

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

if failures:
    for failure in failures:
        print(f"FAIL: {failure}")
    sys.exit(1)
print("OK: grep finds what it can, and names every image and function it could not look inside")
