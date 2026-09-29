#!/usr/bin/env python3
"""cmake --install lays Sherlock out where a consumer finds it: <prefix>/bin/Sherlock.exe, answering
--version with this build's version.

exit 0 = PASS  1 = FAIL
python tests/InstallGate.py <cmake> <build dir> <config> <scratch prefix> <version>
"""
import shutil
import subprocess
import sys
from pathlib import Path


def main():
    cmake, build, config, prefix, version = sys.argv[1:6]
    prefix = Path(prefix)
    shutil.rmtree(prefix, ignore_errors=True)
    done = subprocess.run([cmake, "--install", build, "--config", config, "--prefix", str(prefix)],
                          capture_output=True, text=True)
    if done.returncode != 0:
        print("FAIL: cmake --install exited %d\n%s%s" % (done.returncode, done.stdout, done.stderr))
        return 1
    exe = prefix / "bin" / "Sherlock.exe"
    if not exe.is_file():
        print("FAIL: %s was not installed" % exe)
        return 1
    answered = subprocess.run([str(exe), "--version"], capture_output=True, text=True).stdout.strip()
    if answered != "Sherlock " + version:
        print("FAIL: the installed Sherlock answers %r, expected 'Sherlock %s'" % (answered, version))
        return 1
    print("PASS: %s answers %s" % (exe, answered))
    shutil.rmtree(prefix, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
