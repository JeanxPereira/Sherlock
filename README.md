# Sherlock

A precomputed fact store over the dyld shared cache, queried by agents instead of explored.
Design: `docs/superpowers/specs/2026-09-17-sherlock-design.md`.
Phase 1 plan: `docs/superpowers/plans/2026-09-17-sherlock-phase-1.md`.

## Build

    cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DAQUAKIT_RE_TOOLS=ON
    cmake --build build --config Debug --target Sherlock

Standalone, without the AquaKit towers: `cmake -S tools/Sherlock -B build/sherlock-standalone`.

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
    .\build\tools\Sherlock\Debug\Sherlock.exe status

Layer 3 (`q`'s "cited by"/"sealed at", `status`'s section/citation/seal counts, `find`, `laudo`)
comes from `Documents.db`, which nothing builds automatically -- run
`Sherlock build docs --repo <repo>` once (a few seconds, no corpus needed) when `q`/`status`
report `layer 3: not built`; that line itself prints the exact command, including the
`--documents` path it resolved (`<repo>/build/Sherlock/Documents.db` by default, worktree-relative
on purpose, so a different clone or worktree gets its own).
