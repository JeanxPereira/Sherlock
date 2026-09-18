#!/usr/bin/env python3
"""Process gate for laudo's stored-byte plain output and matching JSON payload."""
import json
import shutil
import subprocess
import sys


CRLF = b"## \xc2\xa77 LayerResolver\r\n\r\n100% shadow pool.\r\n"
LF = b"A concept page\n"


def run(command):
    return subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def main():
    sherlock, fixture, scratch = sys.argv[1:4]
    shutil.rmtree(scratch, ignore_errors=True)
    built = run([fixture, scratch])
    if built.returncode:
        print("FAIL: fixture failed:", built.stderr.decode(errors="replace"))
        return 1

    common = ["--repo", scratch, "--documents", f"{scratch}/Documents.db"]
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
