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

One worker process per image, towers first, two at a time by default. The free space on the
store's disk is checked before each image and the run stops with exit 2 below `--min-free-bytes`.

**The store is written as the run goes, not at its end.** A worker holds `--batch` functions
(1 000 by default) and writes them, so an interruption costs at most that batch rather than the
whole image. A 44 MB tower decompiles for hours, and hours is what a single write at the end puts
at risk of anything that can stop a process.

`--resume` therefore has three answers, not two. A store marked complete is left alone; a partial
one is continued at the function it did not reach, with `carried=N` in the report saying how many
rows it inherited; a store that is empty, unreadable or of another schema is built again. The mark
is a `Complete` key in the store's `Meta`, and a store written before layer 2 wrote in batches has
no such key and counts as complete -- existence and completeness were the same fact then.

The worker opens the EXTRACTED image, not the cache, and keeps its `.i64` for a tower. An
interrupted run leaves IDA's database unpacked beside the image; the next run says so and names
the `.id0`/`.id1`/`.nam`/`.til` to delete, which is a different answer from a live session holding
the same files.

**Under `--resume` the worker clears that database itself, but only its own.** A crashed
interactive session leaves the identical files, and those are analysis IDA can still recover, so
what licenses the delete is a `.sherlock-run` marker the worker writes beside the image while it
holds a database open. Marker plus nothing holding the files plus `--resume` means the debris is
this worker's; without the marker the refusal stands. A resume that cannot clear its own debris is
a resume that never runs twice.

`fn <address>` answers from layer 2 and says so; with `--asm` it disassembles layer 1 instead. An
address layer 2 does not cover prints the `build hexrays` command that would cover it.

    Sherlock grep <pattern> [--images A B] [--ignore-case] [--limit N]

`grep` searches layer 2's pseudocode across the images, which is the question `fn` cannot answer
because `fn` needs the address the search is looking for. It answers in seconds -- 170 000
functions in four -- and prints `<image> <address>:<line>: <text>`.

**Its zero is the dangerous one, and it is answered case by case.** Four different silences read
identically in a text search, so every run names them: images with no layer 2 (with the command
that builds them), images a `--limit` stopped the walk before, functions the decompiler refused,
and the one a reader cannot guess -- **a name defined in another image cannot be found here at
all**, because the carved slice reads its calls as `MEMORY[0x...]` with no symbol on them. That
last one is measured rather than assumed: `ColorScheme.dark` is initialized inside
CampoUIInternal at `0x22f4da22c`, and `grep ColorScheme --images CampoUIInternal` finds nothing.
For a cross-image name the route is `fn --asm` or `References/scripts/fn.py --disasm`.

The verdict's coverage pair counts images SELECTED over images READ, so a walk cut short still
reports everything it owed an answer for.

**Pseudocode locates.** It never closes a decoded value on its own: that needs a layer-1 reading
or an instrument that prints coverage, and `lint_instruments.py` enforces it.

**A call that leaves the image has no name in layer 2.** The worker decompiles an image carved out
of the cache, and a carved slice does not contain what lies between the images -- the point
`References/scripts/dsc_reader.py` exists to make. So a cross-image call reads as `MEMORY[0x...]`,
and the pseudocode of `-[_DLPocketLayerDelegate actionForLayer:forKey:]` shows five of them where
layer 1 records five calls leaving DesignLibrary. Layer 1 read the WHOLE cache and has those
targets: `fn` states how many a function has and names `Sherlock calls <address>`, which resolves
them. The gap is in the slice, not in the store, and nothing in layer 2 can close it -- reading
the two layers together is the answer, and `fn` says so on its face.
