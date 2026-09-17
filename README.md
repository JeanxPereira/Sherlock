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
