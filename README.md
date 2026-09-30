<p align="center">
  <img src=".github/AppIcon.png" alt="AppIcon" width="256" height="256">
  <h1 align="center">Sherlock</h1>
  <p align="center">
    <strong>A precomputed fact store over the dyld shared cache, queried instead of explored.</strong>
  </p>
  <p align="center">
    <img src="https://img.shields.io/badge/version-0.2.0-8B1E3F" alt="Version 0.2.0">
    <img src="https://img.shields.io/badge/platform-Windows-blue" alt="Platform">
    <img src="https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus" alt="C++23">
    <img src="https://img.shields.io/badge/cmake-3.28%2B-064F8C?logo=cmake" alt="CMake">
  </p>
</p>

---

Sherlock reads an extracted macOS dyld shared cache once and writes what an agent would otherwise
rediscover by hand on every question: functions, calls, references, pseudocode, and where the
consumer's own documents and source already cite an address. A query answers from the store and
ends in a **verdict line with its coverage**, so an empty answer always says how much was read.

The first consumer is [AquaKit](https://github.com/JeanxPereira/AquaKit), which reaches it through
a `sherlock.json` at its root.

## The three layers

| Layer | Store | Holds | Built by |
|---|---|---|---|
| 1 · Facts | `Catalog.db`, `Images/<Image>.db` | Functions, calls (direct, branch island, unresolved), literal reads, arm64e PAC virtual-call sites, per-image coverage | `build facts` — capstone over the whole cache |
| 2 · Pseudocode | `Images/<Image>.HexRays.db` | Hex-Rays pseudocode per function, zstd-compressed, with the IDA version that produced it | `build hexrays` — optional, needs IDA |
| 3 · Documents | `Documents.db` | The consumer's markdown split into sections, the citations in them, and the provenance seals in its source comments, with FTS5 search | `build docs` |

Layers 1 and 2 live in one store directory (`--store`). Layer 3 lives with the consumer, at
`<root>/build/Sherlock/Documents.db` by default.

## Commands

| Command | Answers |
|---|---|
| `q <address\|symbol>` | Everything the three layers know about one address |
| `callers <address>` · `calls <address>` | Who calls it · what it calls, cross-image targets resolved |
| `refs <address> [--to <end>]` | Who reads or references an address range |
| `vcall <site>` | The PAC virtual-dispatch family at a `blraa`/`braa` site |
| `fn <address> [--asm]` | Pseudocode of the containing function (layer 2), or its disassembly (layer 1) |
| `grep <pattern> [--images A B] [--ignore-case] [--limit N]` | A text search over layer 2's pseudocode |
| `find "<text>"` · `laudo <slug> [§n]` | A full-text search over the documents · one document or one section of it |
| `status` | What each layer holds and whether it is stale |
| `build facts \| hexrays \| docs` | Builds a layer |

`--cache`, `--store`, `--documents`, `--images-dir` and `--ida-dir` have environment twins:
`SHERLOCK_CACHE`, `SHERLOCK_STORE`, `SHERLOCK_DOCUMENTS`, `SHERLOCK_IMAGES`, `SHERLOCK_IDA_DIR`.
`--json` prints one object: `verdict`, `count`, `coverage {read, total}`, `reason`, `output`.

### Verdicts

```text
verdict: FOUND <count>  coverage <read>/<total>                exit 0
verdict: EMPTY  coverage <read>/<total>                        exit 0
verdict: PARTIAL coverage incomplete  coverage <read>/<total>  exit 3   an EMPTY that did not read everything
verdict: PARTIAL <why>  coverage <read>/<total>                exit 3
verdict: NOT VERIFIED <diagnostic>                             exit 2   the instrument could not look
```

A build that looked and failed exits 1.

### What the layers cannot see

- **A call that leaves the image has no name in layer 2.** The worker decompiles an image carved
  out of the cache, so a cross-image call reads as `MEMORY[0x...]`. `grep` cannot find a name
  defined in another image, and every run says so; `fn` counts those calls and names
  `Sherlock calls <address>`, which layer 1 resolves.
- **Pseudocode locates, it does not prove.** A decoded value closes on a layer-1 reading or on an
  instrument that reports its coverage.

## Quick start

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release
cmake --install build --config Release --prefix "$env:LOCALAPPDATA\Programs\Sherlock"
```

The install yields `bin\Sherlock.exe`, plus `bin\SherlockHexRays.exe` when layer 2 was built; that
`bin\` goes on `PATH` once. Presets `debug` and `release` exist for configure, build and test.

### Layer 2 (optional)

```powershell
cmake --preset release -DSHERLOCK_IDA_SDK=D:/CodingProjects/ida-sdk-9.2/src `
                       -DSHERLOCK_IDA_DIR="C:/Program Files/IDA Professional 9.2"

Sherlock build hexrays --towers --resume --store <store> `
                       --images-dir <extracted dylibs> --ida-dir "C:/Program Files/IDA Professional 9.2"
```

The SDK and the installation are paired by their decompiler API, checked byte-wise at configure
time; version numbers do not predict it. Without both variables the configure says `layer 2 off`
and everything else builds. The export runs one worker process per image, two at a time, smallest
first, writes every 1 000 functions, and `--resume` continues a partial store.

### Tests

Fixture gates always run, including a fixture consumer under `tests/fixtures/consumer/` driven
through `Sherlock.exe`. Corpus gates run when the configure names one:

```powershell
cmake --preset debug -DSHERLOCK_CACHE=<corpus>/<build>/dsc/<build>__MacOS `
                     -DSHERLOCK_STORE=<corpus>/<build>/Sherlock `
                     -DSHERLOCK_CONFIG=<consumer>/sherlock.json `
                     -DSHERLOCK_DYLIBS=<corpus>/<build>/extracted/dylibs
```

`SHERLOCK_DYLIBS` is needed only by the layer-2 export gates.

## The consumer: `sherlock.json`

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

| Key | Meaning |
|---|---|
| `schema` | Must be `1` |
| `sherlock` | Version requirement: `"0.2"` accepts every 0.2.x |
| `towers` | The image map `build facts` reads (optional) |
| `collections` | Paths relative to the file. `evidence`: markdown, one section per heading. `concept`: markdown with front matter, one section per page. `code`: seals from `//` comments, in files with the listed extensions |
| `seals` | The tags that exist and the one that carries an image name (optional) |

Discovery is `--config <file>`, else the first `sherlock.json` walking up from the current
directory. Every validation failure is `NOT VERIFIED` and names what it refused. `build facts`,
`build docs`, `find` and `laudo` require the file; `q` and `status` answer without it and report
layer 3 as `NOT VERIFIED`. `Documents.db` records the SHA-256 of the file it was built from, so
any edit marks it stale until the next `build docs`.

## Public contract

While in 0.x, a change to any of these bumps the minor version: the CLI syntax, the verdict lines
and exit codes, the `sherlock.json` schema, the store schemas and the seal grammar. Each version
closes its section in [CHANGELOG.md](CHANGELOG.md) and gets a `v<version>` tag.

## Dependencies

Fetched by CMake `FetchContent` on first configure, pinned by tag or hash.

| Library | Version | Purpose |
|---|---|---|
| [SQLite](https://www.sqlite.org/) | 3.53.0 | Every store, FTS5 for `find` |
| [Capstone](https://github.com/capstone-engine/capstone) | 5.0.9 | ARM64 disassembly for layer 1 |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | `sherlock.json` and `--json` output |
| [zstd](https://github.com/facebook/zstd) | 1.5.6 | Layer 2 pseudocode compression |
| [IDA SDK](https://github.com/HexRaysSA/ida-sdk) | 9.2 | Layer 2 only, optional |

Every target uses the release CRT in every configuration, because IDA's import libraries are
built against it; the Debug build therefore has no debug heap.

---

<p align="center">
  Made by <a href="https://github.com/JeanxPereira">JeanxPereira</a>
</p>
