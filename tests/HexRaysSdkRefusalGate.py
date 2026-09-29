#!/usr/bin/env python3
# Sherlock -- tests/HexRaysSdkRefusalGate.py
# The pairing check refuses an SDK the installed decompiler cannot answer, and accepts one it can.
#
# Both directions are the gate. A refusal that fires on everything protects nothing, and the check
# guards a pairing whose failure mode is silent -- the worker links, runs, opens a database and
# answers 0 at the handshake, which reads like a missing licence.
#
# The SDKs here are two lines of text, not installations: the check reads the API out of
# hexrays.hpp, so a directory carrying that one header exercises it exactly. That keeps the gate
# independent of which SDKs happen to sit on the machine.
import os
import subprocess
import sys
import tempfile
from pathlib import Path

cmake = os.environ.get("CMAKE_COMMAND", "cmake")
module = Path(os.environ["SHERLOCK_IDA_SDK_MODULE"])
ida_dir = os.environ["SHERLOCK_IDA_DIR"]
real_api = int(os.environ["SHERLOCK_IDA_API"])

# An API no shipped decompiler carries. The check reads one hex digit, so this stays a digit.
absent_api = 0xF
if absent_api == real_api:
    sys.exit(f"FAIL: this gate's stand-in API {absent_api:X} is the installed one -- pick another")


def run(api: int, root: Path) -> subprocess.CompletedProcess:
    include = root / "include"
    include.mkdir(parents=True, exist_ok=True)
    (include / "hexrays.hpp").write_text(
        f"const int64 HEXRAYS_API_MAGIC = 0x00DEC0DE0000000{api:X}LL;\n", encoding="utf-8")
    return subprocess.run(
        [cmake, f"-DSHERLOCK_IDA_SDK={root.as_posix()}", f"-DSHERLOCK_IDA_DIR={ida_dir}", "-P", str(module)],
        capture_output=True, text=True, timeout=600)


with tempfile.TemporaryDirectory() as tmp:
    refused = run(absent_api, Path(tmp) / "absent")
    print(refused.stdout, refused.stderr)
    if refused.returncode == 0:
        sys.exit(f"FAIL: an SDK speaking decompiler API {absent_api:X} was accepted against {ida_dir}. "
                 f"The check passes everything, so it predicts nothing about the handshake.")
    if f"API {absent_api:X}" not in refused.stderr and f"API {absent_api:X}" not in refused.stdout:
        sys.exit(f"FAIL: the refusal does not name the API it refused:\n{refused.stderr}")

    accepted = run(real_api, Path(tmp) / "present")
    print(accepted.stdout, accepted.stderr)
    if accepted.returncode != 0:
        sys.exit(f"FAIL: an SDK speaking the installed decompiler's API {real_api} was refused. "
                 f"The check refuses everything, which is the same as having none.\n{accepted.stderr}")

print(f"OK: API {absent_api:X} refused, API {real_api} accepted, both against {ida_dir}")
