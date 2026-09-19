# Sherlock

A precomputed fact store over the dyld shared cache, queried by agents instead of explored.
Design: `docs/superpowers/specs/2026-09-17-sherlock-design.md`.
Phase plans: `docs/superpowers/plans/2026-09-1{7,8}-sherlock-phase-{1,2,3}.md`.

## Build

    cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DAQUAKIT_RE_TOOLS=ON
    cmake --build build --config Debug --target Sherlock

Standalone, without the AquaKit towers: `cmake -S tools/Sherlock -B build/sherlock-standalone`.

Layer 2 -- the Hex-Rays pseudocode -- is built only when an IDA SDK and an IDA installation whose
DECOMPILER APIs match are both named:

    cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DAQUAKIT_RE_TOOLS=ON `
      -DSHERLOCK_IDA_SDK=D:/CodingProjects/ida-sdk-9.2/src `
      -DSHERLOCK_IDA_DIR="C:/Program Files/IDA Professional 9.2"

The pairing is checked byte-wise at configure time, and the versions do not predict it: a 9.4 SDK
links against a 9.2 runtime, opens a database and then fails the decompiler handshake, which reads
like a missing licence. Without both variables the configure says `layer 2 off` and everything
else builds and runs -- the worker is a separate executable, so the SDK never reaches the
`Sherlock` binary.

## Test

    ctest --test-dir build -C Debug -R "^Sherlock\." --output-on-failure

The gates that read the corpus take its directories from the environment: `SHERLOCK_CACHE`
(the extracted cache) and `SHERLOCK_STORE` (where the databases are written). The test
CMake fills both from `References/scripts/target.py --paths`.

## Query

    $env:SHERLOCK_CACHE = "G:\AquaKit-refs\26A5416b\dsc\26A5416b__MacOS"
    $env:SHERLOCK_STORE = "G:\AquaKit-refs\26A5416b\Sherlock"
    .\build\tools\Sherlock\Debug\Sherlock.exe q 0x240622d98
    .\build\tools\Sherlock\Debug\Sherlock.exe callers 0x2230edebc
    .\build\tools\Sherlock\Debug\Sherlock.exe refs 0x29f60f388 --to 0x29f60f480
    .\build\tools\Sherlock\Debug\Sherlock.exe fn 0x240622d98
    .\build\tools\Sherlock\Debug\Sherlock.exe fn 0x240622d98 --asm
    .\build\tools\Sherlock\Debug\Sherlock.exe status

Layer 3 (`q`'s "cited by"/"sealed at", `status`'s section/citation/seal counts, `find`, `laudo`)
comes from `Documents.db`, which nothing builds automatically -- run
`Sherlock build docs --repo <repo>` once (a few seconds, no corpus needed) when `q`/`status`
report `layer 3: not built`; that line itself prints the exact command, including the
`--documents` path it resolved (`<repo>/build/Sherlock/Documents.db` by default, worktree-relative
on purpose, so a different clone or worktree gets its own).

## Layer 2

    Sherlock build hexrays --towers --resume --store <corpus>/<build>/Sherlock `
                           --images-dir <corpus>/<build>/extracted/dylibs `
                           --ida-dir "C:/Program Files/IDA Professional 9.2"

One worker process per image, towers first, two at a time by default. `--resume` skips an image
already exported and resets one a crash left mid-flight. The free space on the store's disk is
checked before each image and the run stops with exit 2 below `--min-free-bytes`.

The worker opens the EXTRACTED image, not the cache, and keeps its `.i64` for a tower. An
interrupted run leaves IDA's database unpacked beside the image; the next run says so and names
the `.id0`/`.id1`/`.nam`/`.til` to delete, which is a different answer from a live session holding
the same files.

`fn <address>` answers from layer 2 and says so; with `--asm` it disassembles layer 1 instead. An
address layer 2 does not cover prints the `build hexrays` command that would cover it.

**Pseudocode locates.** It never closes a decoded value on its own: that needs a layer-1 reading
or an instrument that prints coverage, and `lint_instruments.py` enforces it.
