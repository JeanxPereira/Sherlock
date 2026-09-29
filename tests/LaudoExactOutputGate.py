#!/usr/bin/env python3
"""Process gate for laudo's stored-byte plain output and matching JSON payload."""
import hashlib
import json
import shutil
import sqlite3
import subprocess
import sys


CRLF = b"## \xc2\xa77 LayerResolver\r\n\r\n100% shadow pool.\r\n"
LF = b"A concept page\n"
CONFIG = b"""{
  "schema": 1,
  "sherlock": "0.2",
  "collections": [
    { "path": "docs/re", "kind": "evidence" },
    { "path": "docs/concepts", "kind": "concept" }
  ]
}
"""


def run(command):
    return subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def main():
    sherlock, fixture, scratch = sys.argv[1:4]
    shutil.rmtree(scratch, ignore_errors=True)
    built = run([fixture, scratch])
    if built.returncode:
        print("FAIL: fixture failed:", built.stderr.decode(errors="replace"))
        return 1

    # The fixture store is written by hand, so it records the configuration's hash the way a
    # build would; without it every file reads as stale, which is a different gate's subject.
    with open(f"{scratch}/sherlock.json", "wb") as fh:
        fh.write(CONFIG)
    db = sqlite3.connect(f"{scratch}/Documents.db")
    db.execute("INSERT OR REPLACE INTO Meta(Key, Value) VALUES('ConfigSha256', ?)", (hashlib.sha256(CONFIG).hexdigest(),))
    db.commit()
    db.close()
    common = ["--config", f"{scratch}/sherlock.json", "--documents", f"{scratch}/Documents.db"]
    for slug, section, expected in (("sample", "§7", CRLF), ("0x27c198c20", "concept title", LF)):
        plain = run([sherlock, "laudo", slug, section, *common])
        if plain.returncode != 0 or plain.stdout != expected:
            print("FAIL: plain laudo was not byte-exact", repr(plain.stdout), repr(expected), plain.returncode)
            return 1
        encoded = run([sherlock, "laudo", slug, section, "--json", *common])
        try:
            result = json.loads(encoded.stdout)
        except json.JSONDecodeError:
            print("FAIL: laudo JSON was not valid", repr(encoded.stdout))
            return 1
        if encoded.returncode != 0 or result.get("verdict") != "FOUND" or result.get("output") != [expected.decode()]:
            print("FAIL: laudo JSON did not retain the stored payload", result, encoded.returncode)
            return 1
    shutil.rmtree(scratch, ignore_errors=True)
    print("PASS: plain laudo preserves CRLF and LF without a verdict suffix")
    return 0


if __name__ == "__main__":
    sys.exit(main())
