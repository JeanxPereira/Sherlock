#!/usr/bin/env python3
"""The fixture consumer, end to end through Sherlock.exe: discovery, --config, a consumer nested in
another, every collection kind, and every refusal sherlock.json promises -- no file, an unknown
key, an invalid kind, another schema, a missing path, an empty collection, a requirement this
build does not meet -- plus the stale answer after the configuration's bytes change.

A refusal must be NOT VERIFIED (exit 2) and name what it refused; one that names nothing reads
exactly like a crash. The copies live in a directory whose name holds a space.

exit 0 = PASS  1 = FAIL  2 = NOT VERIFIED (the executable or the fixture is missing)
python tests/ConsumerGate.py <Sherlock.exe> <fixture consumer dir> <scratch dir>
"""
import hashlib
import json
import shutil
import sqlite3
import subprocess
import sys
import tempfile
from pathlib import Path

FAILURES = []


def check(ok, label, detail=""):
    print(("PASS: " if ok else "FAIL: ") + label + ("" if ok or not detail else "\n    " + detail[-800:]))
    if not ok:
        FAILURES.append(label)


def run(exe, args, cwd):
    done = subprocess.run([str(exe), *args], cwd=cwd, capture_output=True)
    return done.returncode, (done.stdout + done.stderr).decode("utf-8", errors="replace")


def with_head(root):
    (root / ".git").mkdir(exist_ok=True)
    (root / ".git" / "HEAD").write_text("0" * 40 + "\n", encoding="utf-8")
    return root


def consumer(fixture, scratch, name):
    root = scratch / name
    shutil.rmtree(root, ignore_errors=True)
    shutil.copytree(fixture, root)
    return with_head(root)


def edit(root, change):
    path = root / "sherlock.json"
    config = json.loads(path.read_text(encoding="utf-8"))
    change(config)
    path.write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")


def refused(exe, cwd, label, fragments):
    code, out = run(exe, ["find", "sample section"], cwd)
    missing = [f for f in fragments if f not in out]
    check(code == 2 and "verdict: NOT VERIFIED" in out and not missing, label,
          "exit %d, missing %r:\n%s" % (code, missing, out))


def main():
    exe, fixture, scratch = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    if not exe.is_file() or not (fixture / "sherlock.json").is_file():
        print("NOT VERIFIED: %s or %s is missing" % (exe, fixture / "sherlock.json"))
        return 2
    shutil.rmtree(scratch, ignore_errors=True)
    scratch.mkdir(parents=True)
    version = run(exe, ["--version"], scratch)[1].split()[-1]

    # 1. Discovery from a subdirectory, and every kind.
    root = consumer(fixture, scratch, "consumer with space")
    code, out = run(exe, ["build", "docs"], root / "Source")
    documents = root / "build" / "Sherlock" / "Documents.db"
    check(code == 0 and "built " in out and documents.is_file(),
          "build docs from a subdirectory finds sherlock.json and writes <root>/build/Sherlock/Documents.db", out)
    if documents.is_file():
        db = sqlite3.connect(documents)
        coverage = sorted(db.execute("SELECT Root, Read, Total FROM Coverage").fetchall())
        check(coverage == [("Source", 1, 1), ("docs/concepts", 1, 1), ("docs/re", 1, 1)],
              "Coverage names the three declared collections", repr(coverage))
        concept = db.execute("SELECT COUNT(*), MIN(Title) FROM Section WHERE File LIKE 'docs/concepts/%'").fetchone()
        check(concept == (1, "Sample concept"), "a concept page is one section titled by its front matter", repr(concept))
        evidence = db.execute("SELECT COUNT(*) FROM Section WHERE File = 'docs/re/sample-laudo.md'").fetchone()[0]
        check(evidence == 2, "an evidence file splits into one section per heading", repr(evidence))
        seals = db.execute("SELECT Tag, Image, Address FROM Seal ORDER BY Line").fetchall()
        check(seals == [("BIN", "Sample", 0x240622d98), ("KIT", None, None)],
              "seals follow the declared tags: BIN with its image and address, KIT without, OBS is text", repr(seals))
        stored = db.execute("SELECT Value FROM Meta WHERE Key = 'ConfigSha256'").fetchone()
        expected = hashlib.sha256((root / "sherlock.json").read_bytes()).hexdigest()
        check(stored == (expected,), "Documents.db records the SHA-256 of the sherlock.json bytes", repr(stored))
        db.close()
    code, out = run(exe, ["laudo", "sample-laudo", "§1"], root)
    check(code == 0 and "cited here" in out, "laudo serves an evidence section", out)

    # 2. --config names the consumer from anywhere.
    elsewhere = Path(tempfile.mkdtemp(prefix="sherlock-elsewhere-"))
    code, out = run(exe, ["find", "sample section", "--config", str(root / "sherlock.json")], elsewhere)
    check(code == 0 and "sample-laudo" in out, "--config reads a consumer from outside its tree", out)

    # 3. A consumer nested in another reads the nearest sherlock.json.
    inner = with_head(Path(shutil.copytree(fixture, root / "Source" / "nested")))
    code, out = run(exe, ["build", "docs"], inner / "docs")
    check(code == 0 and (inner / "build" / "Sherlock" / "Documents.db").is_file(),
          "a consumer nested in another builds its own store from the nearest sherlock.json", out)
    shutil.rmtree(inner)

    # 4. Every refusal.
    for label, change, fragments in (
        ("an unknown key is refused by name", lambda c: c.update(extra=1), ['unknown key "extra"']),
        ("an invalid kind is refused by name", lambda c: c["collections"][0].update(kind="evidance"), ['"evidance"']),
        ("another schema is refused", lambda c: c.update(schema=2), ['"schema" is 2']),
        ("a missing path is refused naming the collection",
         lambda c: c["collections"][0].update(path="docs/nope"), ["docs/nope", "no such directory"]),
        ("a newer requirement in this series is refused, stating both versions",
         lambda c: c.update(sherlock="0.2.99"), ["0.2.99", version]),
        ("another 0.x series is refused, stating both versions",
         lambda c: c.update(sherlock="0.1"), ["requires Sherlock 0.1", version]),
    ):
        case = consumer(fixture, scratch, "refusal case")
        edit(case, change)
        refused(exe, case, label, fragments)
    case = consumer(fixture, scratch, "refusal case")
    (case / "docs" / "empty").mkdir()
    (case / "docs" / "empty" / "README.md").write_text("not indexed\n", encoding="utf-8")
    edit(case, lambda c: c["collections"].append({"path": "docs/empty", "kind": "evidence"}))
    refused(exe, case, "an empty collection is refused naming it", ["docs/empty", "holds no file"])
    lonely = Path(tempfile.mkdtemp(prefix="sherlock-no-config-"))
    if any((parent / "sherlock.json").exists() for parent in [lonely, *lonely.parents]):
        print("NOT VERIFIED: a sherlock.json sits above %s, so 'no file' cannot be exercised" % lonely)
        return 2
    refused(exe, lonely, "no sherlock.json is refused, naming the walk and --config", ["no sherlock.json", "--config"])

    # 5. The store binds its configuration: changed bytes read stale until the next build.
    # The stale mark below relies on the keyed-hash assertion in check 1 to tell a mismatch from a missing key.
    with open(root / "sherlock.json", "ab") as fh:
        fh.write(b"\n")
    code, out = run(exe, ["laudo", "sample-laudo", "§1"], root)
    check(code == 2 and "sherlock.json has changed" in out, "laudo refuses a section after the configuration's bytes change", out)
    code, out = run(exe, ["find", "sample section"], root)
    check("[stale]" in out, "find marks its hits stale after the configuration's bytes change", out)
    run(exe, ["build", "docs"], root)
    code, out = run(exe, ["laudo", "sample-laudo", "§1"], root)
    check(code == 0 and "cited here" in out, "a rebuild clears the staleness", out)

    for leftover in (scratch, elsewhere, lonely):
        shutil.rmtree(leftover, ignore_errors=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
