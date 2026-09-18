# Sherlock changelog

Each version states what it adds and the measurement behind every performance claim.

## Unreleased

- DocumentIndex: fence-aware markdown heading parser (`Heading`, `ParseHeadings`) -- the first piece layer 3 needs to turn a laudo into addressable sections.
- DocumentIndex: `Heading` enforces CommonMark's two ATX-heading rules a naive `#`-scan misses -- a fence closes only on a line whose run is the SAME character and at least as long as the opening one (a shorter inner run of the same character stays content), and a line indented 4 or more columns is code, never a heading. Corrects the `docs/re` heading count measured for the phase-2 plan: 2 719 -> 2 713 with the fence-skip in place, 2 898 -> 2 860 with it removed (the mutation in Task 9 Step 5).

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
