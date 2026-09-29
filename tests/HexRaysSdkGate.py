#!/usr/bin/env python3
# Sherlock -- tests/HexRaysSdkGate.py
# The worker's version line, and the loop between the SDK cmake chose and the one it compiled against.
#
# An SDK whose decompiler API differs from the installed decompiler's links, initialises and opens a
# database, and only then answers 0 -- which reads like a missing licence or a missing plugin.
# cmake/SherlockIdaSdk.cmake refuses that pairing by reading the API out of both sides; this gate
# proves the binary carries the API cmake matched, so the static check governs the real build.
#
# The handshake itself is NOT checked here: the decompiler is chosen by the processor module, so with
# no database open load_plugin returns null and the handshake answers 0 whatever the pairing. That
# form of the thing is invisible to this instrument, and the worker's line says so rather than
# reporting an absence. The handshake is proven where a database exists.
import os
import re
import subprocess
import sys

worker = os.environ["SHERLOCK_HEXRAYS_WORKER"]
expected_api = os.environ["SHERLOCK_IDA_API"]
env = dict(os.environ)
env["PATH"] = os.environ["SHERLOCK_IDA_DIR"] + os.pathsep + env["PATH"]

out = subprocess.run([worker, "--version"], capture_output=True, text=True, env=env, timeout=300)
print(out.stdout, out.stderr)
if out.returncode != 0:
    sys.exit(f"FAIL: worker --version exited {out.returncode}")

line = re.search(r"sdk (\d+) runtime (\d+)\.(\d+)\.(\d+) api (\d+) decompiler (\S+)", out.stdout)
if not line:
    sys.exit(f"FAIL: --version did not print sdk/runtime/api/decompiler:\n{out.stdout}")

sdk, runtime, api, decompiler = line.group(1), line.group(2, 3, 4), line.group(5), line.group(6)
if api != expected_api:
    sys.exit(f"FAIL: cmake matched decompiler API {expected_api} against the install, and the worker "
             f"was compiled against API {api}. The check and the binary disagree, so the check governs "
             f"nothing -- SHERLOCK_IDA_SDK changed after the configure, or two SDKs are in the build.")
if decompiler != "unknown-without-database":
    sys.exit(f"FAIL: --version claims the decompiler is '{decompiler}'. With no database open the "
             f"processor module is unset and the handshake cannot be read either way; a yes or a no "
             f"here is a reported absence the instrument could not see.")
print(f"OK: sdk {sdk}, runtime {'.'.join(runtime)}, decompiler API {api} as cmake matched")
