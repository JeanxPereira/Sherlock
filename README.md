# Sherlock

A precomputed fact store over the dyld shared cache, queried by agents instead of explored. Layer 1
holds the facts (functions, calls, references, literal reads) of every image a consumer's tower map
names; layer 2 holds Hex-Rays pseudocode per function; layer 3 indexes the consumer's own documents
and source seals. Design history lives in AquaKit, its first consumer:
`docs/superpowers/specs/2026-09-17-sherlock-design.md` and
`docs/superpowers/specs/2026-09-29-sherlock-extraction-design.md`.

## Build, test, install

    cmake --preset debug
    cmake --build --preset debug
    ctest --preset debug

    cmake --preset release
    cmake --build --preset release
    cmake --install build --config Release --prefix "$env:LOCALAPPDATA\Programs\Sherlock"

The install yields `bin\Sherlock.exe`, and `bin\SherlockHexRays.exe` when layer 2 was built. That
`bin\` goes on `PATH` once. Every target uses the release CRT in every configuration: IDA's import
libraries are built against it and two CRTs in one binary do not link, so the Debug build has no
debug heap.

Layer 2 enters the build only when an IDA SDK and an IDA installation whose DECOMPILER APIs match
are both named:

    cmake --preset release -DSHERLOCK_IDA_SDK=D:/CodingProjects/ida-sdk-9.2/src `
                           -DSHERLOCK_IDA_DIR="C:/Program Files/IDA Professional 9.2"

The pairing is checked byte-wise at configure time, and the versions do not predict it: a 9.4 SDK
links against a 9.2 runtime, opens a database and then fails the decompiler handshake, which reads
like a missing licence. Without both variables the configure says `layer 2 off`; the worker is a
separate executable, so everything else builds and runs.

Fixture gates always run. Among them is the fixture consumer under `tests/fixtures/consumer/`,
which proves discovery, validation and every collection kind through `Sherlock.exe`. Corpus gates
run when the configure names a corpus:

    cmake --preset debug -DSHERLOCK_CACHE=<corpus>/<build>/dsc/<build>__MacOS `
                         -DSHERLOCK_STORE=<corpus>/<build>/Sherlock `
                         -DSHERLOCK_CONFIG=<consumer>/sherlock.json `
                         -DSHERLOCK_DYLIBS=<corpus>/<build>/extracted/dylibs

`SHERLOCK_CONFIG` is a consumer whose `towers` names the corpus images, and `Sherlock.BuildTowers`
reads it. Only the layer-2 export gates need `SHERLOCK_DYLIBS`.

## The consumer: `sherlock.json`

One file at the consumer's root declares everything Sherlock reads that is the consumer's:

```json
{
  "schema": 1,
  "sherlock": "0.2",
  "towers": "References/scripts/towers.json",
  "collections": [
    { "path": "docs/re",       "kind": "evidence" },
    { "path": "docs/concepts", "kind": "concept"  },
    { "path": "Source",        "kind": "code", "extensions": [".h", ".cpp"] }
  ],
  "seals": { "tags": ["BIN", "KIT"], "imageTag": "BIN" }
}
```

- `schema` must be `1`. `sherlock` is the version requirement and `collections` is required; `towers` and `seals` are optional.
- Paths are relative to the directory holding `sherlock.json`, which is the root. A trailing slash or a backslash names the same collection; an absolute path or one leaving the root is refused. `towers` follows the same rule.
- `kind` says how a collection is read, never where it is:
  - `evidence`: markdown, one section per heading;
  - `concept`: markdown with front matter, one section per page;
  - `code`: `//` comment blocks, from which seals are extracted, in files whose extension is listed exactly in `extensions`.

  A markdown collection skips `README.md` and `index.md`; a code walk skips `build/`, `lab/` and `.git/`.
- `seals.tags` are the tags that exist, and `seals.imageTag` is the one that carries an image. Without `seals`, a code collection records its files and extracts no seal.
- `towers` names the tower map `build facts` reads: `build`, `image` and `indexed_non_tower_image`.
- **Discovery:** `--config <file>`, else the first `sherlock.json` walking up from the current directory. A consumer nested in another reads the nearest one.
- **Validation:** every failure is NOT VERIFIED, exit 2, and names what it refused:
  - no file found (naming where the walk started, and `--config`);
  - an unknown key;
  - an invalid kind;
  - a schema other than 1;
  - a declared path that does not exist;
  - a collection holding no file Sherlock reads — an empty directory is almost always a wrong path;
  - a requirement this executable does not meet.
- **The requirement:** `"0.2"` accepts 0.2.0 and every later 0.2.x. An older executable, or one from another 0.x series, refuses and states both versions.
- **Which commands need it:** `build facts`, `build docs`, `find` and `laudo` refuse without a configuration. `q` and `status` answer layers 1 and 2 and print `layer 3: NOT VERIFIED -- <why>`. `callers`, `calls`, `refs`, `vcall`, `fn`, `grep` and `build hexrays` never read it.
- **The store binds its configuration:** `Documents.db` records the SHA-256 of the `sherlock.json` bytes it was built from. A store built from other bytes is stale everywhere the staleness path runs: `find` marks its hits, `laudo … §n` refuses, `q` prints `layer 3: stale`, and `status` prints `configuration: … changed`. `Documents.db` defaults to `<root>/build/Sherlock/Documents.db`; `--documents` or `SHERLOCK_DOCUMENTS` overrides it.

## The public contract

These are what a consumer reads, and a change to any of them bumps the minor version while in 0.x:
- the CLI syntax;
- the verdict lines and exit codes;
- the `sherlock.json` schema;
- the store schemas (`kSchemaVersion` for the catalog and image stores, `kDocumentsSchemaVersion`, and layer 2's own);
- the seal grammar.

Each version closes its `CHANGELOG.md` section and gets a `v<version>` tag.

**Verdict lines** end every answer:

    verdict: FOUND <count>  coverage <read>/<total>              exit 0
    verdict: EMPTY  coverage <read>/<total>                      exit 0
    verdict: PARTIAL coverage incomplete  coverage <read>/<total>  exit 3   (an EMPTY that did not read everything)
    verdict: PARTIAL <why>  coverage <read>/<total>              exit 3
    verdict: NOT VERIFIED <diagnostic>                           exit 2   (the instrument could not look)

A build that looked and failed exits 1. `--json` prints one object: `verdict`, `count`,
`coverage {read, total}`, `reason` and `output`.

**The seal grammar**, inside a block of consecutive `//` lines:
- a declared `[TAG]`;
- for the image tag, the image name right after it (`[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z0-9_]+)*`);
- every cache-shaped address `0x(1[89a-f][0-9a-f]{7}|2[0-9a-f]{8})` in the tag's segment. The segment runs to the next occurrence of the same tag.

A segment yields one row per address, or one row with no address when it has none. A code line whose own `//` sits within the first 8 characters of its trimmed text, and which is not brace-only, continues an open block.

## Query

    Sherlock q 0x240622d98 --cache <cache> --store <store>
    Sherlock callers 0x2230edebc --store <store>
    Sherlock refs 0x29f60f388 --to 0x29f60f480 --cache <cache> --store <store>
    Sherlock fn 0x240622d98 [--asm] --store <store> [--cache <cache>]
    Sherlock grep <pattern> [--images A B] [--ignore-case] [--limit N] --store <store>
    Sherlock status --store <store>
    Sherlock build docs
    Sherlock find "shadow pool"
    Sherlock laudo <slug> [§n]

`SHERLOCK_CACHE` and `SHERLOCK_STORE` stand in for `--cache` and `--store`. Nothing builds layer 3
automatically: `q` and `status` say `layer 3: not built` and print the exact `build docs` command.

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
