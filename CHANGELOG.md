# Sherlock changelog

Each version states what it adds and the measurement behind every performance claim.

## 0.1.0

- Foundation: `Diagnostic`, `Expected`, `ByteReader`, `MappedFile`.
- Store: owning SQLite wrappers, schema version 1 for the catalog and the image stores.
- DyldSharedCache: the split cache as one address space, slide info v5 pointers and rebase chains.
- MachO: segments, sections, function starts and symbol table of one image; DyldSharedCache gains Owner and FindImage.
- Facts: capstone-backed extraction of functions, calls (direct/island/unresolved), literal reads and coverage; WriteImageFacts round-trips into an image store.
- Facts::Builder and Sherlock build facts: catalog + per-image stores, worker threads sized to hardware_concurrency; the 18 tower images build in 96.0s, peak 1180.2 MiB (measured 26A5416b).
- SherlockCli: q/callers/calls/refs/status, routed through DyldSharedCache::Cache::Owner; the verdict line matches verdict.py byte for byte, including the EMPTY -> PARTIAL coverage-incomplete downgrade.
