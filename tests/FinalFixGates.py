#!/usr/bin/env python3
"""Focused CLI gates for schema identity, JSON, and path validation."""
from __future__ import annotations

import json
import os
import shutil
import sqlite3
import subprocess
import sys
from pathlib import Path


def schema(db: sqlite3.Connection, kind: str, uuid: str = "cache-a", image: str = "") -> None:
    db.executescript("""
        CREATE TABLE Meta(Key TEXT PRIMARY KEY, Value TEXT NOT NULL) WITHOUT ROWID;
        CREATE TABLE Image(Path TEXT PRIMARY KEY, Name TEXT NOT NULL, Tower TEXT,
            Header INTEGER NOT NULL, State TEXT NOT NULL, Reason TEXT, FactsVersion TEXT,
            HexRaysVersion TEXT) WITHOUT ROWID;
        CREATE TABLE Coverage(Image TEXT NOT NULL, Layer TEXT NOT NULL, Unit TEXT NOT NULL,
            Read INTEGER NOT NULL, Total INTEGER NOT NULL, PRIMARY KEY(Image, Layer)) WITHOUT ROWID;
    """ if kind == "Catalog" else """
        CREATE TABLE Meta(Key TEXT PRIMARY KEY, Value TEXT NOT NULL) WITHOUT ROWID;
        CREATE TABLE Call(Site INTEGER PRIMARY KEY, Caller INTEGER NOT NULL,
            Target INTEGER NOT NULL, Island INTEGER, Via TEXT NOT NULL);
    """)
    values = [("Kind", kind), ("SchemaVersion", "1"), ("SherlockVersion", "0.1.0"),
              ("CacheUuid", uuid)]
    if kind == "Catalog":
        values.append(("Build", "test"))
    else:
        values.append(("ImagePath", image))
    db.executemany("INSERT INTO Meta VALUES(?, ?)", values)
    db.commit()


def run(exe: Path, *args: str, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    env.pop("SHERLOCK_STORE", None)
    env.pop("SHERLOCK_CACHE", None)
    return subprocess.run([str(exe), *args], cwd=cwd, env=env, capture_output=True, text=True)


def main() -> int:
    exe, scratch = Path(sys.argv[1]), Path(sys.argv[2])
    if scratch.exists():
        shutil.rmtree(scratch)
    scratch.mkdir(parents=True, exist_ok=True)
    store = scratch / "store"
    images = store / "Images"
    images.mkdir(parents=True, exist_ok=True)
    catalog = sqlite3.connect(store / "Catalog.db")
    schema(catalog, "Catalog")

    status = run(exe, "status", "--json", "--store", str(store))
    try:
        payload = json.loads(status.stdout)
    except json.JSONDecodeError as error:
        print("FAIL status --json:", error, repr(status.stdout))
        return 1
    if status.returncode != 0 or payload.get("verdict") != "FOUND":
        print("FAIL status --json outcome", status.returncode, payload)
        return 1
    required_status = ("schema:", "produced by Sherlock:", "running Sherlock:", "disk:", "layer 2: not built")
    output = payload.get("output", [])
    if not all(any(fragment in line for line in output) for fragment in required_status):
        print("FAIL status --json omitted text-equivalent content", payload)
        return 1
    malformed = run(exe, "status", "--workers", "junk", "--json", "--store", str(store))
    malformed_payload = json.loads(malformed.stdout)
    if malformed.returncode != 2 or malformed_payload.get("verdict") != "NOT VERIFIED":
        print("FAIL argument error was not JSON", malformed.returncode, malformed.stdout)
        return 1

    catalog.execute("UPDATE Meta SET Value = '0' WHERE Key = 'SchemaVersion'")
    catalog.commit()
    refused = run(exe, "status", "--json", "--store", str(store))
    if refused.returncode != 2 or json.loads(refused.stdout).get("verdict") != "NOT VERIFIED":
        print("FAIL incompatible catalog schema was accepted", refused.returncode, refused.stdout)
        return 1
    catalog.execute("UPDATE Meta SET Value = '1' WHERE Key = 'SchemaVersion'")
    catalog.execute("INSERT INTO Image VALUES(?, ?, NULL, 1, 'FactsDone', NULL, '0.1.0', NULL)",
                    ("/right/image", "image.db"))
    catalog.execute("INSERT INTO Coverage VALUES('/right/image', 'Facts', 'instructions', 1, 1)")
    catalog.commit()
    catalog.close()

    image = sqlite3.connect(images / "image.db")
    schema(image, "Image", image="/wrong/image")
    image.close()
    mismatch = run(exe, "callers", "0x1", "--store", str(store))
    if mismatch.returncode != 2 or "identity" not in mismatch.stdout:
        print("FAIL mismatched image identity was accepted", mismatch.returncode, mismatch.stdout)
        return 1

    partial_store = scratch / "partial"
    (partial_store / "Images").mkdir(parents=True)
    partial = sqlite3.connect(partial_store / "Catalog.db")
    schema(partial, "Catalog")
    partial.execute("INSERT INTO Image VALUES('/x', 'x.db', NULL, 1, 'FactsDone', NULL, '0.1.0', NULL)")
    partial.execute("INSERT INTO Coverage VALUES('/x', 'Facts', 'instructions', 1, 2)")
    partial.commit()
    partial.close()
    partial_image = sqlite3.connect(partial_store / "Images" / "x.db")
    schema(partial_image, "Image", image="/x")
    partial_image.close()
    incomplete = run(exe, "callers", "0x1", "--json", "--store", str(partial_store))
    incomplete_payload = json.loads(incomplete.stdout)
    if incomplete.returncode != 3 or incomplete_payload.get("verdict") != "PARTIAL":
        print("FAIL query JSON normalization", incomplete.returncode, incomplete.stdout)
        return 1

    no_store = run(exe, "status", cwd=scratch)
    if no_store.returncode != 2 or (scratch / "Catalog.db").exists():
        print("FAIL omitted store path touched the working directory", no_store.returncode, no_store.stdout)
        return 1

    print("PASS final schema, JSON, identity, and path gates")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
