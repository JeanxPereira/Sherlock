# Sherlock changelog

Each version states what it adds and the measurement behind every performance claim.

## Unreleased

- `DocumentIndex::Builder`'s `WalkSource` now shares `DocumentIndex::HasSealExtension` with `SealDump` -- it walked only `.h`/`.hpp`/`.cpp`, so `Documents.db`'s `Seal` table carried none of the `[BIN]` seals `Source/DesignLibrary/shaders`, `Source/Platform/shaders`, `Source/QuartzCore/shaders` and `Source/SwiftUICore/shaders` hold in `.frag`/`.vert`/`.glsl` files, while `SealDump`/`DocumentIndexParity.py` (which already scanned those extensions) kept reporting PASS: the parity gate agreed with itself over a set the built store never carried. `Source/`'s whole-corpus `[BIN]` count moves 2 144 -> 2 207 (`measure_document_counts.py`, widened the same way); a real `Sherlock build docs` over the repository now reports `2922 section(s), 52345 citation(s), 2277 seal(s)` (was `2212 seal(s)`). `Sherlock.QuerySealedAtShader` pins one shader seal (`Source/DesignLibrary/shaders/SiriEffect.frag:4`, `0x24058bd88`) as a positive control against this exact drift.
- DocumentIndex: fence-aware markdown heading parser (`Heading`, `ParseHeadings`) -- the first piece layer 3 needs to turn a laudo into addressable sections.
- DocumentIndex: `Heading` enforces CommonMark's two ATX-heading rules a naive `#`-scan misses -- a fence closes only on a line whose run is the SAME character and at least as long as the opening one (a shorter inner run of the same character stays content), and a line indented 4 or more columns is code, never a heading. Corrects the `docs/re` heading count measured for the phase-2 plan: 2 719 -> 2 713 with the fence-skip in place, 2 898 -> 2 860 with it removed (the mutation in Task 9 Step 5).
- `Sherlock.DocumentsExactCounts`: pins the whole-corpus `Section`/`Seal` totals, measured by `tests/Sherlock/measure_document_counts.py` on the current tree -- `docs/re` 2 713 sections, `docs/concepts` 209 sections, `Source/` 2 144 `[BIN]` seals. `Sherlock build docs` over the whole repository reports `2922 section(s), 52345 citation(s), 2212 seal(s)`.
- `SealExtractor`: a tag's segment can carry more than one cache-shaped address (`SnippetSizeConstants.h:6`'s own clean control seals two, a getter and an initializer) -- every one now becomes its own `Seal` row sharing the segment's File/Line/Tag/Image, instead of only the first. `q`'s "sealed at" answered empty for 900 of the corpus's 1 572 distinct sealed addresses before this; `Source/`'s `[BIN]` count moves 981 -> 2 144, and the whole-corpus seal total moves 1 008 -> 2 212 for the same reason plus the `INF`/`DEMO`/`ASSUMPTION` tags below.
- `SealExtractor`: recognises `INF`/`DEMO`/`ASSUMPTION` alongside `BIN`/`KIT`/`OBS`/`API`, matching every tag `lint_seals.py`'s own `TAG` treats as a real seal (it additionally refuses `RE`/`DOC`/`WEB` as unknown-tag, and this port does the same by construction -- its pattern never lists them).
- `SealDump`/`DocumentIndexParity`: scan `.frag`/`.vert`/`.glsl` alongside `.h`/`.cpp`/`.hpp` -- `Source/Platform/shaders` carries 50 real `[BIN]` seals in `.frag`/`.glsl` files that a `//`-comment-style scan excluding shader extensions never saw. The parity coverage line now states files SCANNED, not just files carrying a hit.
- `CitationExtractor`: the Address pattern is the cache-address SHAPE (`lint_seals.py`'s own `ADDR`), not a bare "six-plus hex digits" test -- a struct's `flags=0x00000052` or a float's `0x00000000` bit pattern no longer becomes an Address citation. The file-extension exclusion list gains `.framework`/`.dylib`/`.frag`/`.app`/`.plist`/`.ttf`/`.ttc`/`.car`/`.bundle`/`.metallib` (a framework, a dylib, a shader, a rootfs bundle, a font, a compiled asset catalog cited by name is not a Symbol), and the extension comparison folds case on BOTH operands, not only the known extension's. Whole-corpus citations move 55 264 -> 52 345 for these three reasons combined.

## 0.1.0

- Foundation: `Diagnostic`, `Expected`, `ByteReader`, `MappedFile`.
- Store: owning SQLite wrappers, schema version 1 for the catalog and the image stores.
- DyldSharedCache: the split cache as one address space, slide info v5 pointers and rebase chains.
- MachO: segments, sections, function starts and symbol table of one image; DyldSharedCache gains Owner and FindImage.
- Facts: capstone-backed extraction of functions, calls (direct/island/unresolved), literal reads and coverage; WriteImageFacts round-trips into an image store.
- Facts::Builder and Sherlock build facts: catalog + per-image stores, worker threads sized to hardware_concurrency; the 18 tower images build in 96.0s, peak 1180.2 MiB (measured 26A5416b).
- SherlockCli: q/callers/calls/refs/status, routed through DyldSharedCache::Cache::Owner; the verdict line matches verdict.py byte for byte, including the EMPTY -> PARTIAL coverage-incomplete downgrade.
- SherlockCli::Parity: five layer-1 checks against dsc_reader.py, symbols.py, xisland.py and litref.py.
- Phase 1 complete: q/callers/calls/refs/status over the 18 tower images, measured 96.0s / peak 1180.2 MiB (Task 6, Step 9).
